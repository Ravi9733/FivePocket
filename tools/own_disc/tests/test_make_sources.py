#!/usr/bin/env python3
"""make_sources.py: the sources folder of a package is written from a checked git checkout and a checked archive; what
is taken, what stays out, what refuses the run. A small git repository and archive made here, no game data.
MAKE_SOURCES names another copy of the tool (the mutant runner beside this file)."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL = Path(os.environ.get('MAKE_SOURCES', HERE.parent / 'make_sources.py'))
spec = importlib.util.spec_from_file_location('make_sources_under_test', TOOL)
ms = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ms)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class Case(unittest.TestCase):
    def git(self, *args):
        done = subprocess.run(['git', '-C', str(self.tree), '-c', 'user.name=t', '-c', 'user.email=t@example.invalid', '-c', 'commit.gpgsign=false', *args],
                              capture_output=True)
        self.assertEqual(done.returncode, 0, done.stderr.decode(errors='replace'))
        return done.stdout

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.tree = self.dir / 'checkout'
        (self.tree / 'src').mkdir(parents=True)
        subprocess.run(['git', 'init', '-q', str(self.tree)], check=True, capture_output=True)
        (self.tree / 'src' / 'main.cpp').write_bytes(b'int main() { return 0; }\n')
        (self.tree / 'README.md').write_bytes(b'upstream readme\n')
        (self.tree / '.gitignore').write_bytes(b'build/\n')
        self.git('add', '.')
        self.git('commit', '-q', '-m', 'base')
        (self.tree / 'src' / 'main.cpp').write_bytes(b'int main() { return 1; } // patched\n')        # this project's patch
        (self.tree / 'src' / 'added_test.cc').write_bytes(b'// an added test\n')                         # untracked, kept
        (self.tree / 'build').mkdir()
        (self.tree / 'build' / 'out.o').write_bytes(b'object code')                                     # ignored
        self.members = {'tools/small/tool.cpp': b'int tool();\n', 'support/build.ps1': b'cmake ...\r\n', 'machine/paths.json': b'{"home": "/Users/somebody"}\n',
                        'machine-notes/keep.txt': b'kept: only the name begins alike\n', 'xenos/base/blob.bin': b'\0\1\2 somebody \3'}
        self.tar = self.dir / 'host-src.tar'
        self.pack(self.members)

    def tearDown(self):
        self.tmp.cleanup()

    def pack(self, members, extra=()):
        with tarfile.open(self.tar, 'w') as tar:
            for name, data in members.items():
                info = tarfile.TarInfo('./' + name)
                info.size = len(data)
                tar.addfile(info, io.BytesIO(data))
            for info in extra:
                tar.addfile(info)

    def identity(self):
        return self.git('rev-parse', 'HEAD').decode().strip(), digest(self.git('diff'))

    def run_tool(self, out=None, commit=None, diff=None, tar_hash=None, leave_out=('machine',), forbid=('somebody',), note=None):
        head, patch = self.identity()
        out = out or self.dir / f'out-{len(list(self.dir.glob("out-*")))}'
        args = ['--generator', str(self.tree), '--generator-commit', commit or head, '--generator-diff-sha256', diff or patch,
                '--host-src', str(self.tar), '--host-src-sha256', tar_hash or digest(self.tar.read_bytes()), '--out', str(out)]
        for prefix in leave_out:
            args += ['--leave-out', prefix]
        if note is not None:
            path = self.dir / 'note.txt'
            path.write_text(note, encoding='utf-8')
            args += ['--note', str(path)]
        for word in forbid:
            args += ['--forbid', word]
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = ms.main(args)
        return code, out, stderr.getvalue()

    def refused(self, needle, **kw):
        code, out, stderr = self.run_tool(**kw)
        self.assertEqual(code, 2, 'the run was not refused')
        self.assertIn(needle, stderr)
        self.assertFalse(out.exists(), 'a refused run left something behind')

    # ---- what is taken
    def test_writes_both_sets_and_the_record(self):
        code, out, stderr = self.run_tool()
        self.assertEqual(code, 0, stderr)
        self.assertEqual((out / 'XenonRecomp/src/main.cpp').read_bytes(), b'int main() { return 1; } // patched\n')     # as built, not as committed
        self.assertEqual((out / 'XenonRecomp/src/added_test.cc').read_bytes(), b'// an added test\n')
        self.assertEqual((out / 'XenonRecomp/README.md').read_bytes(), b'upstream readme\n')
        self.assertFalse((out / 'XenonRecomp/build').exists())                                                           # what git ignores stays out
        self.assertEqual((out / 'host-programs/support/build.ps1').read_bytes(), b'cmake ...\r\n')                       # byte for byte
        self.assertEqual((out / 'host-programs/xenos/base/blob.bin').read_bytes(), self.members['xenos/base/blob.bin'])
        self.assertTrue((out / 'host-programs/machine-notes/keep.txt').is_file())                                        # only the name begins alike
        self.assertFalse((out / 'host-programs/machine').exists())
        record = json.loads((out / 'sources.json').read_text())
        head, patch = self.identity()
        today = __import__('time').strftime('%Y-%m-%d', __import__('time').gmtime())
        self.assertEqual(record['generator'], {'commit': head, 'diff_sha256': patch, 'files': 4, 'links_not_written': [],
                                               'changed': [{'path': 'src/main.cpp', 'last_written': today}], 'added': [{'path': 'src/added_test.cc', 'last_written': today}]})
        self.assertEqual(record['host_programs'], {'archive_sha256': digest(self.tar.read_bytes()), 'files': 4, 'links_not_written': [], 'left_out': ['machine/paths.json']})
        on_disk = sorted(p.relative_to(out).as_posix() for p in out.rglob('*') if p.is_file() and p.name not in ('sources.json', 'CHANGES.txt'))
        self.assertEqual([f['path'] for f in record['files']], on_disk)
        for entry in record['files']:
            data = (out / entry['path']).read_bytes()
            self.assertEqual((entry['sha256'], entry['size']), (digest(data), len(data)))

    def test_changes_file_names_what_this_project_changed(self):
        code, out, stderr = self.run_tool()
        self.assertEqual(code, 0, stderr)
        text = (out / 'CHANGES.txt').read_text()
        head, _ = self.identity()
        self.assertIn(f'upstream tree at commit {head}', text)
        lines = text.splitlines()
        self.assertEqual([l.split()[-1] for l in lines if l.startswith('  changed')], ['src/main.cpp'])
        self.assertEqual([l.split()[-1] for l in lines if l.startswith('  added')], ['src/added_test.cc'])
        self.assertNotIn('README.md', text)                                    # untouched upstream files are not named
        outside = [l.strip() for l in lines[lines.index(next(l for l in lines if l.startswith('host-programs/'))) + 2:]]
        self.assertEqual(outside, ['machine-notes/keep.txt', 'support/build.ps1', 'tools/small/tool.cpp'])        # not the base tree's blob, not what was left out

    def test_note_ends_the_changes_file(self):
        code, out, stderr = self.run_tool(note='The base tree differs from its upstream in two files.\n')
        self.assertEqual(code, 0, stderr)
        self.assertTrue((out / 'CHANGES.txt').read_text().endswith('\n\nThe base tree differs from its upstream in two files.\n'))

    def test_forbidden_text_in_the_note_is_refused(self):
        self.refused('the note note.txt holds "somebody"', note='compared by somebody\n')

    def test_links_are_listed_not_written(self):
        os.symlink('main.cpp', self.tree / 'src' / 'alias.cpp')
        link = tarfile.TarInfo('./tools/small/alias')
        link.type, link.linkname = tarfile.SYMTYPE, 'tool.cpp'
        self.pack(self.members, [link])
        code, out, stderr = self.run_tool()
        self.assertEqual(code, 0, stderr)
        record = json.loads((out / 'sources.json').read_text())
        self.assertEqual(record['generator']['links_not_written'], ['src/alias.cpp'])
        self.assertEqual(record['host_programs']['links_not_written'], ['tools/small/alias'])
        self.assertFalse((out / 'XenonRecomp/src/alias.cpp').exists() or (out / 'XenonRecomp/src/alias.cpp').is_symlink())
        self.assertFalse((out / 'host-programs/tools/small/alias').exists() or (out / 'host-programs/tools/small/alias').is_symlink())

    # ---- the identities
    def test_another_commit_is_refused(self):
        self.refused('not at', commit='0' * 40)

    def test_other_changes_than_the_recorded_ones_are_refused(self):
        _, recorded = self.identity()
        (self.tree / 'README.md').write_bytes(b'changed after the build\n')
        self.refused('are not the recorded ones', diff=recorded)

    def test_another_archive_is_refused(self):
        self.refused('is not the recorded archive', tar_hash=digest(b'another archive'))

    def test_missing_tracked_file_is_refused(self):
        (self.tree / 'README.md').unlink()
        self.refused('a tracked file is missing')

    # ---- what must not get out
    def test_forbidden_text_in_the_checkout_is_refused(self):
        (self.tree / 'src' / 'added_test.cc').write_bytes(b'// built by somebody\n')
        self.refused('XenonRecomp/src/added_test.cc holds "somebody"')

    def test_forbidden_text_in_the_archive_is_refused(self):
        self.refused('host-programs/machine/paths.json holds "somebody"', leave_out=())

    def test_a_name_that_leaves_the_folder_is_refused(self):
        for bad in ('../outside.txt', 'tools/../../outside.txt', '/etc/outside.txt'):
            info = tarfile.TarInfo(bad)
            info.size = 1
            with tarfile.open(self.tar, 'w') as tar:
                tar.addfile(info, io.BytesIO(b'x'))
            self.refused('a name leaves the folder')
        self.assertFalse((self.dir / 'outside.txt').exists())

    def test_something_that_is_no_file_is_refused(self):
        pipe = tarfile.TarInfo('./tools/pipe')
        pipe.type = tarfile.FIFOTYPE
        self.pack(self.members, [pipe])
        self.refused('neither a file nor a link')

    def test_existing_output_folder_is_refused(self):
        out = self.dir / 'taken'
        out.mkdir()
        code, _, stderr = self.run_tool(out=out)
        self.assertEqual(code, 2)
        self.assertIn('exists already', stderr)
        self.assertEqual(list(out.iterdir()), [])

    def test_an_empty_set_is_refused(self):
        self.pack({'machine/paths.json': b'{}\n'})
        self.refused('is empty')


if __name__ == '__main__':
    unittest.main()
