#!/usr/bin/env python3
"""portable_compile.py on a synthetic frozen build with stand-in NDK tools (no compiler, no game). The stand-in
clang++ writes small ELF files, so replay_build.py runs as it is. PORTABLE_COMPILE_DIR names another copy of the
tools folder (the mutant runner beside this file)."""
import hashlib
import importlib.util
import inspect
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
TOOLS = Path(os.environ.get('PORTABLE_COMPILE_DIR') or HERE.parent)
sys.path.insert(0, str(TOOLS))
spec = importlib.util.spec_from_file_location('portable_compile_under_test', TOOLS / 'portable_compile.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
spec = importlib.util.spec_from_file_location('elf_fixture', HERE / 'test_replay_tools.py')
replay_tests = importlib.util.module_from_spec(spec)
spec.loader.exec_module(replay_tests)

FIRST, NDK1 = '/first/repo', '/first/ndk/toolchains/llvm/prebuilt/darwin-x86_64'
FROZEN = 'local/run-1'
CLANG = '''#!/usr/bin/env python3
import struct, sys
PROGBITS, RELA, NOBITS, STRTAB = 1, 4, 8, 3
ALLOC, EXEC = 2, 4
%s
a = sys.argv[1:]
if a and a[0].startswith('@'):
    a = [w.strip('"') for w in open(a[0][1:]).read().split('\\n') if w]
out = a[a.index('-o') + 1]
if '-c' in a:
    src = a[a.index('-c') + 1]
    text = open(src, 'rb').read()
    if b'BROKEN' in text:
        sys.stderr.write('stand-in: cannot compile'); sys.exit(1)
    includes = b''.join(open(w[2:] + '/' + n, 'rb').read() for w in a if w.startswith('-I') for n in (b'pack.h'.decode(), 'support.h', 'runtime.h')
                        if __import__('os').path.isfile(w[2:] + '/' + n))
    open(out, 'wb').write(elf([('.text', PROGBITS, ALLOC | EXEC, text + includes), ('.comment', PROGBITS, 0, b'stand-in clang')]))
else:
    parts = b''.join(open(w, 'rb').read() for w in a if w.endswith(('.o', '.ld', '.a')))
    open(out, 'wb').write(elf([('.text', PROGBITS, ALLOC | EXEC, parts), ('.comment', PROGBITS, 0, b'stand-in linker')]))
'''
NM = '''#!/usr/bin/env python3
import re, sys
for kind, name in re.findall(rb'// SYM (\\S+) (\\S+)\\n', open(sys.argv[-1], 'rb').read()):
    print('0000000000000000', kind.decode(), name.decode())
'''
OBJCOPY = '''#!/usr/bin/env python3
import sys
assert sys.argv[1] == '--remove-section=.comment', sys.argv
open(sys.argv[3], 'wb').write(open(sys.argv[2], 'rb').read().replace(b'stand-in linker', b''))
'''
sha = lambda data: hashlib.sha256(data).hexdigest()
entry = lambda data: {'sha256': sha(data), 'size': len(data)}
GAME = b'// SYM W _Z12sub_82000010R10PPCContextPh\n// SYM W _Z12sub_82000020R10PPCContextPh\n// SYM T __imp__sub_82000010\nint game;\n'
HOOK = b'// SYM T _Z12sub_82000020R10PPCContextPh\nint hook;\n'
SUPPORT, PACK, RUNTIME = b'#define SUPPORT 1\n', b'#define PACK 1\n', b'#define RUNTIME 1\n'
SCRIPT = ('/* code-reach: bind unhooked generated weak wrappers to their bodies (layout-only, see redirect_script.py) */\n'
          '_Z12sub_82000010R10PPCContextPh = __imp__sub_82000010;\n').encode()


class Fixture:
    def __init__(self, root: Path):
        self.root = root
        self.repo, self.work, self.ndk = root / 'repo', root / 'this run', root / 'ndk'
        bin_dir = self.ndk / 'toolchains/llvm/prebuilt' / m.host_support.ndk_host_tag() / 'bin'
        bin_dir.mkdir(parents=True)
        for name, text in (('clang++', CLANG % inspect.getsource(replay_tests.elf)), ('llvm-nm', NM), ('llvm-objcopy', OBJCOPY)):
            (bin_dir / name).write_text(text, encoding='utf-8', newline='\n')
            (bin_dir / name).chmod(0o755)
        self.put(self.repo / 'src/runtime.h', RUNTIME)
        self.put(self.repo / 'src/hook.cpp', HOOK)
        self.stage = root / 'stage-recipe'; self.put(self.stage / 'chunks/ppc_recomp.0.cpp', GAME)
        self.supplied = root / 'supplied'; self.put(self.supplied / 'support.h', SUPPORT)
        self.put(self.work / 'packs/pack.h', PACK)                 # an earlier stage of this run wrote it in place
        w = f'{FIRST}/{FROZEN}'
        obj = lambda n: f'{w}/compile/attempts/v/build/obj/{n}.o'
        unit = lambda name, kind, source: dict(name=name, kind=kind, source=source, object=obj(name), depfile=obj(name)[:-2] + '.d', object_sha256='0' * 64,
                                               seconds_here=1.0, command=[NDK1 + '/bin/clang++', f'-I{w}/packs', f'-I{w}/compile/attempts/v/src',
                                                                          f'-I{FIRST}/src', '-c', source, '-o', obj(name)])
        self.manifest = dict(schema='frozen-build/1', attempt='v', repo_root=FIRST, compiler_root=NDK1, cwd=FIRST + '/build', build_jobs_here=2,
                             units=[unit('ppc_recomp.0.cpp', 'chunk', f'{w}/compile/attempts/v/src/ppc_recomp.0.cpp'), unit('hook.cpp', 'runtime', FIRST + '/src/hook.cpp')],
                             link=dict(command=[NDK1 + '/bin/clang++', '-shared', '-o', f'{w}/compile/attempts/v/build/libv.so', obj('ppc_recomp.0.cpp'), obj('hook.cpp'),
                                                f'{w}/compile/redirect.ld'], library=f'{w}/compile/attempts/v/build/libv.so', library_sha256='0' * 64, seconds_here=1, inputs=[]),
                             inputs={'src/runtime.h': entry(RUNTIME), 'src/hook.cpp': entry(HOOK), f'{FROZEN}/packs/pack.h': entry(PACK),
                                     f'{FROZEN}/compile/attempts/v/src/ppc_recomp.0.cpp': entry(GAME), f'{FROZEN}/compile/attempts/v/src/support.h': entry(SUPPORT),
                                     f'{FROZEN}/compile/redirect.ld': entry(SCRIPT)},
                             compiler_inputs={})
        self.path = root / 'manifest.json'
        self.save()

    def put(self, path: Path, data: bytes):
        path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data); return path

    def save(self):
        self.path.write_text(json.dumps(self.manifest), encoding='utf-8')

    def build(self, stages=None, supplied=None, later=None, host_built=None):
        return m.build(self.path, FROZEN, self.repo, self.work, self.ndk,
                       [('stage:recipe', self.stage)] if stages is None else stages,
                       [('supplied:support', self.supplied)] if supplied is None else supplied,
                       {'compile/redirect.ld'} if later is None else later, jobs=2, host_built=host_built)

    def with_library(self, here: bytes):
        """Adds a static library to the build: the manifest names the first machine's bytes as a link input, and an
        earlier stage of this run has written `here` in place. Returns the first machine's archive identity."""
        first = replay_tests.ar([('dsp.o', replay_tests.elf(replay_tests.BASE)), ('tables.o', replay_tests.elf(replay_tests.OTHER))])
        self.manifest['inputs'][f'{FROZEN}/decoder/lib.a'] = entry(first)
        self.manifest['link']['command'].append(f'{FIRST}/{FROZEN}/decoder/lib.a')
        self.save()
        self.put(self.work / 'decoder/lib.a', here)
        self.put(self.root / 'first.a', first)
        return m.elf_sections.archive_identity(self.root / 'first.a')[0], first


class PortableCompile(unittest.TestCase):
    def fixture(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        return Fixture(Path(tmp.name).resolve())

    def test_inputs_are_laid_out_by_content_the_script_is_made_and_the_library_linked(self):
        f = self.fixture()
        report = f.build()
        src = f.work / 'compile/attempts/v/src'
        self.assertEqual(((src / 'ppc_recomp.0.cpp').read_bytes(), (src / 'support.h').read_bytes()), (GAME, SUPPORT))
        self.assertEqual(report['origins'], {'stage:recipe': 1, 'supplied:support': 1, 'this run, in place': 1, 'made later from this run\'s objects': 1})
        self.assertEqual(report['supplied_inputs'], ['compile/attempts/v/src/support.h'])
        self.assertEqual((f.work / 'compile/redirect.ld').read_bytes(), SCRIPT)
        self.assertEqual(report['redirect'], {'sha256': sha(SCRIPT), 'assignments': 1, 'hooked_wrappers_left_alone': 1, 'objects': 2})
        self.assertEqual(report['units']['total'], 2)
        linked = f.work / 'compile/attempts/v/build/libv.so'
        library = f.work / 'compile/attempts/v/build/libgame-portable.so'
        self.assertIn(GAME + PACK + SUPPORT + RUNTIME, linked.read_bytes())           # the unit read this run's pack, the laid-out header, the tracked header
        self.assertIn(SCRIPT, linked.read_bytes())                                    # the link read the script made from these objects
        self.assertEqual(library.read_bytes(), linked.read_bytes().replace(b'stand-in linker', b''))
        self.assertEqual(report['library'], {'path': str(library), 'sha256': sha(library.read_bytes()), 'bytes': library.stat().st_size})
        self.assertEqual(report['linked']['sha256'], sha(linked.read_bytes()))
        self.assertEqual(json.loads((f.work / 'compile/portable-compile.json').read_text(encoding='utf-8')), report)
        self.assertEqual(report['manifest_sha256'], sha(f.path.read_bytes()))
        self.assertEqual((f.stage / 'chunks/ppc_recomp.0.cpp').read_bytes(), GAME)   # sources are read, never moved
        self.assertFalse((f.repo / 'replay-results.json').exists())                  # nothing of the replay is written into the tree

    def test_an_input_found_nowhere_refuses_the_run_before_anything_is_compiled(self):
        f = self.fixture()
        (f.supplied / 'support.h').write_bytes(b'#define SUPPORT 2\n')
        with self.assertRaisesRegex(ValueError, '1 inputs of the compile are neither a stage output of this run nor a supplied file: compile/attempts/v/src/support.h'):
            f.build()
        self.assertFalse((f.work / 'compile/attempts/v/build').exists())
        self.assertFalse((f.work / 'compile/attempts/v/src/support.h').exists())

    def test_this_runs_own_file_with_other_bytes_is_refused(self):
        f = self.fixture()
        (f.work / 'packs/pack.h').write_bytes(b'#define PACK 2\n')
        with self.assertRaisesRegex(ValueError, 'packs/pack.h: this run\'s file is not the one'):
            f.build()
        f = self.fixture()
        f.put(f.work / 'compile/attempts/v/src/support.h', b'left by something else\n')
        with self.assertRaisesRegex(ValueError, 'a different file lies there already'):
            f.build()
        self.assertEqual((f.work / 'compile/attempts/v/src/support.h').read_bytes(), b'left by something else\n')

    def test_a_stage_output_is_named_before_a_supplied_file_of_the_same_bytes(self):
        f = self.fixture()
        f.put(f.supplied / 'old-copy-of-the-game-code.cpp', GAME)
        report = f.build()
        self.assertEqual(report['origins']['stage:recipe'], 1)
        self.assertEqual(report['supplied_inputs'], ['compile/attempts/v/src/support.h'])
        # without the stage the same bytes would have to come from the supplied folder, and the record says so
        f = self.fixture()
        f.put(f.supplied / 'old-copy-of-the-game-code.cpp', GAME)
        report = f.build(stages=[])
        self.assertEqual(report['supplied_inputs'], ['compile/attempts/v/src/ppc_recomp.0.cpp', 'compile/attempts/v/src/support.h'])

    def test_a_script_that_is_not_the_one_the_link_was_written_for_fails_the_run(self):
        f = self.fixture()
        f.manifest['inputs'][f'{FROZEN}/compile/redirect.ld'] = entry(SCRIPT + b'extra = 1;\n'); f.save()
        with self.assertRaisesRegex(RuntimeError, 'linker script made from this run\'s objects is not the one'):
            f.build()
        self.assertFalse((f.work / 'compile/attempts/v/build/libv.so').exists())
        self.assertFalse((f.work / 'compile/attempts/v/build/libgame-portable.so').exists())

    HERE_BUILT = lambda self: replay_tests.ar([('dsp.o', replay_tests.elf(replay_tests.HOST_B)), ('tables.o', replay_tests.elf(replay_tests.OTHER))])

    def test_a_library_built_on_this_host_is_taken_by_its_objects_and_linked(self):
        f = self.fixture()
        identity, first = f.with_library(self.HERE_BUILT())
        self.assertNotEqual(sha(self.HERE_BUILT()), sha(first))
        report = f.build(host_built={'decoder/lib.a': identity})
        self.assertEqual(report['origins'], {'stage:recipe': 1, 'supplied:support': 1, 'this run, in place': 1, 'made later from this run\'s objects': 1,
                                             'this run, in place (the first machine\'s objects with this host\'s compiler name in them)': 1})
        self.assertEqual((f.work / 'decoder/lib.a').read_bytes(), self.HERE_BUILT())                    # read, never replaced
        self.assertIn(self.HERE_BUILT(), (f.work / 'compile/attempts/v/build/libv.so').read_bytes())    # the link took this run's library
        # the first machine's own bytes in place need no word about objects
        g = self.fixture()
        identity, first = g.with_library(b'')
        (g.work / 'decoder/lib.a').write_bytes(first)
        self.assertEqual(g.build(host_built={'decoder/lib.a': identity})['origins']['this run, in place'], 2)

    def test_a_library_with_other_bytes_is_refused_unless_named_with_its_identity(self):
        for what, here, named in (('not named', self.HERE_BUILT(), {}),
                                  ('another identity', self.HERE_BUILT(), {'decoder/lib.a': '0' * 64}),
                                  ('other objects', replay_tests.ar([('dsp.o', replay_tests.elf(replay_tests.HOST_B)), ('tables.o', replay_tests.elf(replay_tests.BASE))]), None),
                                  ('no library at all', b'text', None), ('no library and no identity', b'text', {'decoder/lib.a': ''})):
            f = self.fixture()
            identity, _ = f.with_library(here)
            with self.subTest(case=what), self.assertRaisesRegex(ValueError, 'decoder/lib.a: this run\'s file is not the one'):
                f.build(host_built={'decoder/lib.a': identity} if named is None else named)
            self.assertFalse((f.work / 'compile').exists())
        f = self.fixture()
        identity, _ = f.with_library(self.HERE_BUILT())
        with self.assertRaisesRegex(ValueError, '--host-built names no input of the manifest: decoder/other.a'):
            f.build(host_built={'decoder/lib.a': identity, 'decoder/other.a': identity})

    def test_the_command_line_takes_a_host_built_library_as_name_and_hash(self):
        f = self.fixture()
        identity, _ = f.with_library(self.HERE_BUILT())
        argv = ['--manifest', str(f.path), '--frozen-work', FROZEN, '--repo', str(f.repo), '--work', str(f.work), '--ndk', str(f.ndk),
                '--stage', f'recipe={f.stage}', '--supplied', f'support={f.supplied}', '--made-later', 'compile/redirect.ld', '--jobs', '2']
        for bad in ('decoder/lib.a', 'decoder/lib.a=xyz', 'decoder/lib.a=' + identity.upper(), 'decoder/lib.a=' + identity[:-1]):
            with mock.patch('sys.stderr', new=io.StringIO()) as said:
                self.assertEqual(m.main(argv + ['--host-built', bad]), 2, bad)
            self.assertIn('--host-built wants REL=HEX', said.getvalue())
        self.assertFalse((f.work / 'compile').exists())
        with mock.patch('sys.stdout', new=io.StringIO()) as said:
            self.assertEqual(m.main(argv + ['--host-built', 'decoder/lib.a=' + identity]), 0)
        self.assertIn('this host\'s compiler name in them', said.getvalue())

    def test_the_script_must_be_named_as_made_later_and_only_inputs_can_be(self):
        f = self.fixture()
        with self.assertRaisesRegex(ValueError, 'name it with --made-later'):
            f.build(later=set())
        with self.assertRaisesRegex(ValueError, '--made-later names no input'):
            f.build(later={'compile/redirect.ld', 'compile/other.ld'})
        self.assertFalse((f.work / 'compile').exists())

    def test_a_unit_that_does_not_compile_fails_the_run(self):
        f = self.fixture()
        (f.repo / 'src/hook.cpp').write_bytes(b'BROKEN\n')
        f.manifest['inputs']['src/hook.cpp'] = entry(b'BROKEN\n'); f.save()
        with self.assertRaisesRegex(RuntimeError, 'the replay ended with 1; log: .*replay-compile.log'):
            f.build()
        self.assertFalse((f.work / 'compile/redirect.ld').exists())                   # nothing follows a failed compile
        self.assertFalse((f.work / 'compile/attempts/v/build/libgame-portable.so').exists())

    def test_a_link_that_fails_fails_the_run(self):
        f = self.fixture()
        f.manifest['link']['command'].append(f'{FIRST}/{FROZEN}/compile/attempts/v/build/obj/absent.o'); f.save()
        with self.assertRaisesRegex(RuntimeError, 'the replay ended with 1; log: .*replay-link.log'):
            f.build()
        self.assertEqual((f.work / 'compile/redirect.ld').read_bytes(), SCRIPT)       # the compile and the script were done
        self.assertFalse((f.work / 'compile/attempts/v/build/libgame-portable.so').exists())
        self.assertFalse((f.work / 'compile/portable-compile.json').exists())

    def test_refusals_of_shape(self):
        f = self.fixture()
        with self.assertRaisesRegex(ValueError, 'NDK tool is missing'):
            m.build(f.path, FROZEN, f.repo, f.work, f.root / 'no-ndk', [], [], {'compile/redirect.ld'})
        with self.assertRaisesRegex(ValueError, 'path below'):
            m.build(f.path, '../' + FROZEN, f.repo, f.work, f.ndk, [], [], {'compile/redirect.ld'})
        with self.assertRaisesRegex(ValueError, 'names no input in'):
            m.build(f.path, 'local/run-9', f.repo, f.work, f.ndk, [], [], set())
        with self.assertRaisesRegex(ValueError, 'is not there'):
            f.build(stages=[('stage:gone', f.root / 'gone')])
        f.manifest['inputs'][f'{FROZEN}/../escape.h'] = entry(b'x'); f.save()
        with self.assertRaisesRegex(ValueError, 'outside the work folder'):
            f.build()
        f = self.fixture()
        f.manifest['link']['library'] = FIRST + '/elsewhere/libv.so'; f.save()
        with self.assertRaisesRegex(ValueError, 'links outside'):
            f.build()

    def test_the_command_line_names_origins_and_gives_the_exit_codes(self):
        f = self.fixture()
        argv = ['--manifest', str(f.path), '--frozen-work', FROZEN, '--repo', str(f.repo), '--work', str(f.work), '--ndk', str(f.ndk),
                '--stage', f'recipe={f.stage}', '--supplied', f'support={f.supplied}', '--made-later', 'compile/redirect.ld', '--jobs', '2']
        with mock.patch('sys.stdout', new=io.StringIO()) as out:
            self.assertEqual(m.main(argv), 0)
        self.assertIn('PORTABLE_COMPILE 2 units', out.getvalue())
        self.assertIn('inputs 1 stage:recipe', out.getvalue()); self.assertIn('inputs 1 supplied:support', out.getvalue())
        g = self.fixture()
        argv = [str(g.path) if a == str(f.path) else a.replace(str(f.root), str(g.root)) for a in argv]
        with mock.patch('sys.stderr', new=io.StringIO()) as err:
            self.assertEqual(m.main(argv[:argv.index('--supplied')] + ['--made-later', 'compile/redirect.ld']), 2)      # the supplied folder is not named
        self.assertIn('PORTABLE_COMPILE_REFUSED', err.getvalue())
        (g.repo / 'src/hook.cpp').write_bytes(b'BROKEN\n'); g.manifest['inputs']['src/hook.cpp'] = entry(b'BROKEN\n'); g.save()
        with mock.patch('sys.stderr', new=io.StringIO()) as err:
            self.assertEqual(m.main(argv), 1)
        self.assertIn('PORTABLE_COMPILE_FAILED', err.getvalue())
        with self.assertRaises(ValueError):
            m.named(['no-equals-sign'], '--stage')


if __name__ == '__main__':
    unittest.main()
