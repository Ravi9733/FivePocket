#!/usr/bin/env python3
"""Tests for the tools that carry a finished build to another machine: elf_sections.py, replay_build.py and the pure
parts of freeze_build.py. No compiler, no game data: ELF files are assembled here byte by byte.

REPLAY_TOOLS_DIR names the folder holding the three tools (the mutant runner points it at changed copies).
"""
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = os.environ.get('REPLAY_TOOLS_DIR') or str(Path(__file__).resolve().parents[1])
sys.path.insert(0, TOOLS)
import elf_sections  # noqa: E402
import freeze_build  # noqa: E402
import replay_build  # noqa: E402

PROGBITS, RELA, NOBITS, STRTAB = 1, 4, 8, 3
ALLOC, EXEC = 2, 4


def elf(sections, extended=False, cut=0) -> bytes:
    """An ELF64 little-endian relocatable file with these (name, type, flags, content-or-size) sections."""
    names = b'\0'
    offsets = {}
    for name, *_ in list(sections) + [('.shstrtab',)]:
        offsets[name] = len(names)
        names += name.encode() + b'\0'
    body = b''
    heads = [struct.pack('<IIQQQQIIQQ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]
    for name, kind, flags, content in list(sections) + [('.shstrtab', STRTAB, 0, names)]:
        size = content if kind == NOBITS else len(content)
        heads.append(struct.pack('<IIQQQQIIQQ', offsets[name], kind, flags, 0, 64 + len(body), size, 0, 0, 1, 0))
        if kind != NOBITS:
            body += content
    count, strings = len(heads), len(heads) - 1
    if extended:  # the form for more than 65,279 sections: the counts live in section 0
        heads[0] = struct.pack('<IIQQQQIIQQ', 0, 0, 0, 0, 0, count, strings, 0, 0, 0)
    header = b'\x7fELF\x02\x01\x01' + b'\0' * 9 + struct.pack('<HHIQQQIHHHHHH', 1, 183, 1, 0, 0, 64 + len(body), 0, 64, 0, 0, 64,
                                                             0 if extended else count, 0xFFFF if extended else strings)
    data = header + body + b''.join(heads)
    return data[:len(data) - cut] if cut else data


BASE = [('.text', PROGBITS, ALLOC | EXEC, b'\x1f\x20\x03\xd5' * 4), ('.rela.text', RELA, 0, b'R' * 24),
        ('.comment', PROGBITS, 0, b'\0compiler on host A\0'), ('.debug_info', PROGBITS, 0, b'/home/a/x.cpp'),
        ('.rela.debug_info', RELA, 0, b'D' * 24), ('.bss', NOBITS, ALLOC | 1, 4096)]


def changed(name, content=None, flags=None, rename=None):
    out = []
    for n, kind, f, c in BASE:
        if n == name:
            out.append((rename or n, kind, f if flags is None else flags, c if content is None else content))
        else:
            out.append((n, kind, f, c))
    return out


def ar(members, symbol_table=b'SYMBOLS') -> bytes:
    """A GNU-format `ar` archive: the symbol table, the table of long names when one is needed, the members."""
    def record(name: bytes, body: bytes) -> bytes:
        head = name.ljust(16) + b'0'.ljust(12) + b'0'.ljust(6) + b'0'.ljust(6) + b'644'.ljust(8) + str(len(body)).encode().ljust(10) + b'`\n'
        return head + body + (b'\n' if len(body) & 1 else b'')
    table, fields = b'', []
    for name, _ in members:
        if len(name) > 15:
            fields.append(b'/' + str(len(table)).encode())
            table += name.encode() + b'/\n'
        else:
            fields.append(name.encode() + b'/')
    out = b'!<arch>\n'
    if symbol_table is not None:
        out += record(b'/', symbol_table)
    if table:
        out += record(b'//', table)
    return out + b''.join(record(field, body) for field, (_, body) in zip(fields, members))


HOST_B = [(n, k, f, b'\0compiler on host B, named at more length\0' if n == '.comment' else c) for n, k, f, c in BASE]
OTHER = [('.text', PROGBITS, ALLOC | EXEC, b'\x00\x00\x80\xd2' * 4)] + BASE[1:]


class Files(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def file(self, name, data: bytes) -> str:
        path = os.path.join(self.tmp.name, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as f:
            f.write(data)
        return path

    def identity(self, name, sections, **kw):
        return elf_sections.code_identity(self.file(name, elf(sections, **kw)))


class CodeIdentity(Files):
    def test_the_compilers_own_name_is_not_part_of_the_identity_but_changes_the_file(self):
        a = self.file('a.o', elf(BASE))
        b = self.file('b.o', elf(changed('.comment', b'\0compiler on host B, longer\0')))
        self.assertNotEqual(elf_sections.file_sha256(a), elf_sections.file_sha256(b))
        self.assertEqual(elf_sections.code_identity(a)[0], elf_sections.code_identity(b)[0])

    def test_debug_sections_and_their_relocations_are_left_out_and_named(self):
        a, left = self.identity('a.o', BASE)
        b, _ = self.identity('b.o', changed('.debug_info', b'D:/recomp/x.cpp'))
        c, _ = self.identity('c.o', changed('.rela.debug_info', b'E' * 48))
        self.assertEqual(a, b)
        self.assertEqual(a, c)
        self.assertEqual(left, ['.comment', '.debug_info', '.rela.debug_info'])

    def test_one_changed_instruction_changes_the_identity(self):
        self.assertNotEqual(self.identity('a.o', BASE)[0], self.identity('b.o', changed('.text', b'\x1f\x20\x03\xd5' * 3 + b'\xc0\x03\x5f\xd6'))[0])

    def test_relocations_of_code_count_as_code(self):
        self.assertNotEqual(self.identity('a.o', BASE)[0], self.identity('b.o', changed('.rela.text', b'R' * 23 + b'S'))[0])

    def test_only_the_exact_names_are_left_out(self):
        for name in ('.comment.keep', '.debugger_note'):
            extra = BASE + [(name, PROGBITS, 0, b'one')]
            other = BASE + [(name, PROGBITS, 0, b'two')]
            self.assertNotEqual(self.identity('a.o', extra)[0], self.identity('b.o', other)[0], name)
            self.assertNotIn(name, self.identity('a.o', extra)[1])

    def test_section_flags_are_part_of_the_identity(self):
        self.assertNotEqual(self.identity('a.o', BASE)[0], self.identity('b.o', changed('.text', flags=ALLOC))[0])

    def test_a_section_without_file_bytes_counts_by_its_size(self):
        self.assertNotEqual(self.identity('a.o', BASE)[0], self.identity('b.o', changed('.bss', 8192))[0])
        self.assertEqual([s['size'] for s in elf_sections.sections(self.file('c.o', elf(BASE))) if s['name'] == '.bss'], [4096])

    def test_more_sections_than_the_header_can_count(self):
        plain = elf_sections.sections(self.file('a.o', elf(BASE)))
        extended = elf_sections.sections(self.file('b.o', elf(BASE, extended=True)))
        self.assertEqual([(s['name'], s['sha256']) for s in plain[1:]], [(s['name'], s['sha256']) for s in extended[1:]])
        self.assertEqual(len(extended), len(BASE) + 2)

    def test_a_cut_off_file_is_refused(self):
        whole = elf(BASE)
        with self.assertRaises(ValueError):      # the section table itself is incomplete
            elf_sections.sections(self.file('a.o', whole[:-40]))
        table, = struct.unpack_from('<Q', whole, 0x28)
        longer = bytearray(whole)                # the first section claims bytes beyond the end of the file
        struct.pack_into('<Q', longer, table + 64 + 32, len(whole))
        with self.assertRaises(ValueError):
            elf_sections.sections(self.file('b.o', bytes(longer)))

    def test_a_file_that_is_not_elf_is_refused(self):
        with self.assertRaises(ValueError):
            elf_sections.sections(self.file('a.o', b'MZ' + b'\0' * 200))

    def test_diff_names_what_differs_and_what_is_missing(self):
        a = self.file('a.o', elf(BASE))
        b = self.file('b.o', elf([s for s in changed('.text', b'\0' * 16) if s[0] != '.debug_info'] + [('.note', PROGBITS, 0, b'n')]))
        self.assertEqual(elf_sections.diff(a, b), ['.text', '.debug_info', '.shstrtab', '.note'])
        self.assertEqual(elf_sections.diff(a, a), [])


class ArchiveIdentity(Files):
    MEMBERS = [('short.o', BASE), ('a_name_longer_than_the_field.o', OTHER), ('odd.o', BASE[:1] + [('.data', PROGBITS, ALLOC | 1, b'odd')] + BASE[2:3])]

    def library(self, name, members, **kw):
        return self.file(name, ar([(n, elf(s)) for n, s in members], **kw))

    def test_the_same_objects_with_another_compiler_name_are_another_file_and_the_same_library(self):
        a = self.library('a.a', self.MEMBERS)
        b = self.library('b.a', [(n, [(x, k, f, b'\0compiler on host B, named at more length\0' if x == '.comment' else c) for x, k, f, c in s]) for n, s in self.MEMBERS])
        self.assertNotEqual(elf_sections.file_sha256(a), elf_sections.file_sha256(b))
        self.assertNotEqual(os.path.getsize(a), os.path.getsize(b))
        self.assertEqual(elf_sections.archive_identity(a), elf_sections.archive_identity(b))
        self.assertEqual(elf_sections.archive_identity(a)[1], 3)

    def test_members_are_read_by_name_in_order_and_the_archives_own_records_are_no_members(self):
        path = self.library('a.a', self.MEMBERS)
        self.assertEqual([(n, len(b)) for n, b in elf_sections.archive_members(path)],
                         [(n, len(elf(s))) for n, s in self.MEMBERS])                     # the long name through the name table, the odd length padded
        self.assertEqual(elf_sections.archive_members(path)[2][1], elf(self.MEMBERS[2][1]))
        wide = self.file('w.a', ar([(n, elf(s)) for n, s in self.MEMBERS]).replace(b'/               0', b'/SYM64/         0', 1))
        self.assertEqual(elf_sections.archive_identity(wide), elf_sections.archive_identity(path))
        bare = self.library('n.a', self.MEMBERS, symbol_table=None)
        self.assertEqual(elf_sections.archive_identity(bare), elf_sections.archive_identity(path))

    def test_another_object_another_order_or_another_name_is_another_library(self):
        base = elf_sections.archive_identity(self.library('a.a', self.MEMBERS))[0]
        cases = {'a changed instruction': [self.MEMBERS[0], ('a_name_longer_than_the_field.o', BASE), self.MEMBERS[2]],
                 'another order': [self.MEMBERS[1], self.MEMBERS[0], self.MEMBERS[2]],
                 'another name': [('other.o', BASE)] + self.MEMBERS[1:],
                 'a member less': self.MEMBERS[:2]}
        for index, (what, members) in enumerate(cases.items()):
            self.assertNotEqual(elf_sections.archive_identity(self.library(f'{index}.a', members))[0], base, what)

    def test_what_is_no_archive_of_objects_is_refused(self):
        whole = ar([(n, elf(s)) for n, s in self.MEMBERS])
        cases = {'not an ar archive': b'PK' + whole[2:], 'truncated member': whole[:-40], 'damaged member header': whole.replace(b'`\n', b'`\r', 1),
                 'no members': ar([]), 'not an ELF64': ar([('text.o', b'no object at all')]),
                 'another archive format': ar([('short.o', elf(BASE))]).replace(b'short.o/ ', b'#1/20    ', 1),
                 'outside the name table': whole.replace(b'/0  ', b'/999', 1)}
        for index, (word, data) in enumerate(cases.items()):
            with self.subTest(case=word), self.assertRaisesRegex(ValueError, word):
                elf_sections.archive_identity(self.file(f'{index}.a', data))
        size_text = whole.replace(b'644     7 ', b'644     x ', 1)
        self.assertNotEqual(size_text, whole)
        with self.assertRaisesRegex(ValueError, 'damaged member header'):
            elf_sections.archive_identity(self.file('s.a', size_text))

    def test_the_command_line_prints_file_hash_identity_and_member_count(self):
        import subprocess
        path = self.library('a.a', self.MEMBERS)
        done = subprocess.run([sys.executable, '-B', os.path.join(TOOLS, 'elf_sections.py'), '--archive', path], capture_output=True, text=True)
        identity, count = elf_sections.archive_identity(path)
        self.assertEqual(done.stdout.split(), [elf_sections.file_sha256(path), identity, str(count), path])


class Replay(Files):
    MANIFEST = dict(compiler_root='/opt/ndk/prebuilt/darwin-x86_64', repo_root='/Users/a/repo')

    def translator(self):
        return replay_build.Translator(self.MANIFEST, 'D:\\work\\r\\', 'D:\\ndk\\prebuilt\\windows-x86_64')

    def test_both_roots_are_replaced_also_inside_option_values(self):
        tr = self.translator()
        self.assertEqual(tr.word('/Users/a/repo/src/x.cpp'), 'D:/work/r/src/x.cpp')
        self.assertEqual(tr.word('--sysroot=/opt/ndk/prebuilt/darwin-x86_64/sysroot'), '--sysroot=D:/ndk/prebuilt/windows-x86_64/sysroot')
        self.assertEqual(tr.word('-Wl,--version-script=/Users/a/repo/e.exports'), '-Wl,--version-script=D:/work/r/e.exports')
        self.assertEqual(tr.word('-DNAME=/Users/b/other'), '-DNAME=/Users/b/other')

    def test_the_driver_gets_its_windows_name_only_on_windows(self):
        tr = self.translator()
        words = ['/opt/ndk/prebuilt/darwin-x86_64/bin/clang++', '-c', '/Users/a/repo/x.cpp']
        was = replay_build.WINDOWS
        self.addCleanup(setattr, replay_build, 'WINDOWS', was)
        replay_build.WINDOWS = True
        self.assertEqual(tr.command(words)[0], 'D:/ndk/prebuilt/windows-x86_64/bin/clang++.exe')
        replay_build.WINDOWS = False
        self.assertEqual(tr.command(words), ['D:/ndk/prebuilt/windows-x86_64/bin/clang++', '-c', 'D:/work/r/x.cpp'])

    def test_paths_written_into_objects_are_mapped_back_to_the_first_machine(self):
        flags = self.translator().reproducible_flags('/Users/a/repo/build')
        self.assertIn('-ffile-prefix-map=D:/work/r=/Users/a/repo', flags)
        self.assertIn('-ffile-prefix-map=D:/ndk/prebuilt/windows-x86_64=/opt/ndk/prebuilt/darwin-x86_64', flags)
        self.assertIn('-ffile-reproducible', flags)
        self.assertIn('-fdebug-compilation-dir=/Users/a/repo/build', flags)

    def moved(self, root='D:\\r', maps=(('local/run-15', 'E:\\out\\work\\'), ('local/run-15/deep', 'F:/deep'))):
        return replay_build.Translator(self.MANIFEST, root, 'D:\\ndk\\prebuilt\\windows-x86_64', maps)

    def test_a_moved_folder_is_replaced_before_the_root_and_the_longest_first(self):
        tr = self.moved()
        self.assertEqual(tr.word('/Users/a/repo/local/run-15/compile/x.cpp'), 'E:/out/work/compile/x.cpp')
        self.assertEqual(tr.word('-I/Users/a/repo/local/run-15/deep/inc'), '-IF:/deep/inc')
        self.assertEqual(tr.word('/Users/a/repo/local/run-16/x.cpp'), 'D:/r/local/run-16/x.cpp')         # another folder: the root's rule
        self.assertEqual(tr.word('/Users/a/repo/src/x.cpp'), 'D:/r/src/x.cpp')
        self.assertEqual(tr.word('--sysroot=/opt/ndk/prebuilt/darwin-x86_64/sysroot'), '--sysroot=D:/ndk/prebuilt/windows-x86_64/sysroot')
        # one pass: a new path that holds the first machine's root as text is not replaced again
        again = self.moved(root='/Users/b/copy', maps=(('local/run-15', '/Users/a/repo/local/run-17'),))
        self.assertEqual(again.word('/Users/a/repo/local/run-15/x.cpp'), '/Users/a/repo/local/run-17/x.cpp')
        self.assertEqual(again.word('/Users/a/repo/src/x.cpp'), '/Users/b/copy/src/x.cpp')
        self.assertEqual(self.translator().pairs, [('/opt/ndk/prebuilt/darwin-x86_64', 'D:/ndk/prebuilt/windows-x86_64'), ('/Users/a/repo', 'D:/work/r')])

    def test_an_input_of_a_moved_folder_is_looked_for_where_it_lies_here(self):
        tr = self.moved()
        self.assertEqual(tr.local('local/run-15/compile/a.h'), 'E:/out/work/compile/a.h')
        self.assertEqual(tr.local('local/run-15/deep/b.h'), 'F:/deep/b.h')
        self.assertEqual(tr.local('local/run-15'), 'E:/out/work')
        self.assertEqual(tr.local('local/run-150/c.h'), 'D:/r/local/run-150/c.h')      # a name that only starts alike is not moved
        self.assertEqual(tr.local('src/c.h'), 'D:/r/src/c.h')
        here = self.file('work/compile/a.h', b'moved\n')
        self.file('root/src/c.h', b'stays\n')
        base = os.path.dirname(os.path.dirname(os.path.dirname(here)))
        real = replay_build.Translator(self.MANIFEST, base + '/root', '/ndk', [('local/run-15', base + '/work')])
        table = {'local/run-15/compile/a.h': dict(sha256=elf_sections.file_sha256(here), size=6),
                 'src/c.h': dict(sha256=elf_sections.file_sha256(base + '/root/src/c.h'), size=6)}
        self.assertEqual(replay_build.check_inputs(table, real.local, 'input'), [])
        plain = replay_build.Translator(self.MANIFEST, base + '/root', '/ndk')
        self.assertEqual(replay_build.check_inputs(table, plain.local, 'input'), ['input missing: local/run-15/compile/a.h'])

    def test_maps_of_another_shape_are_refused(self):
        for maps in ((('', 'D:/x'),), (('a/../b', 'D:/x'),), (('a', 'D:/x'), ('a/', 'D:/y'))):
            with self.subTest(maps=maps), self.assertRaises(ValueError):
                self.moved(maps=maps)

    def test_paths_of_a_moved_folder_are_mapped_back_last(self):
        flags = self.moved().reproducible_flags('/Users/a/repo/build')
        maps = [f for f in flags if f.startswith('-ffile-prefix-map=')]
        self.assertEqual(maps, ['-ffile-prefix-map=D:/ndk/prebuilt/windows-x86_64=/opt/ndk/prebuilt/darwin-x86_64',
                                '-ffile-prefix-map=D:/r=/Users/a/repo',
                                '-ffile-prefix-map=E:/out/work=/Users/a/repo/local/run-15',
                                '-ffile-prefix-map=F:/deep=/Users/a/repo/local/run-15/deep'])
        self.assertEqual(flags[-2:], ['-ffile-reproducible', '-fdebug-compilation-dir=/Users/a/repo/build'])
        self.assertEqual([f for f in self.translator().reproducible_flags('/x') if 'prefix-map' in f],
                         ['-ffile-prefix-map=D:/ndk/prebuilt/windows-x86_64=/opt/ndk/prebuilt/darwin-x86_64', '-ffile-prefix-map=D:/work/r=/Users/a/repo'])

    def test_an_input_made_later_is_left_out_of_the_check_only_when_named(self):
        good = self.file('in/a.h', b'int a;\n')
        table = {'a.h': dict(sha256=elf_sections.file_sha256(good), size=7), 'later.ld': dict(sha256='0' * 64, size=3)}
        base = os.path.dirname(good)
        self.assertEqual(replay_build.check_inputs(table, base, 'input'), ['input missing: later.ld'])
        self.assertEqual(replay_build.check_inputs(table, base, 'input', {'later.ld'}), [])
        self.assertEqual(replay_build.check_inputs(table, base, 'input', {'other'}), ['input missing: later.ld'])

    def test_a_library_this_run_built_is_recognised_by_its_objects_when_its_bytes_are_another_hosts(self):
        base = self.tmp.name
        first = ar([('one.o', elf(BASE)), ('two.o', elf(OTHER))])
        here = self.file('lib/here.a', ar([('one.o', elf(HOST_B)), ('two.o', elf(OTHER))]))        # this host's compiler name in one object
        identity = elf_sections.archive_identity(self.file('first.a', first))[0]
        table = {'lib/here.a': dict(sha256=elf_sections.hashlib.sha256(first).hexdigest(), size=len(first))}
        self.assertEqual(replay_build.check_inputs(table, base, 'input'), ['input differs: lib/here.a'])              # not named: bytes decide
        self.assertEqual(replay_build.check_inputs(table, base, 'input', identities={'lib/here.a': identity}), [])
        self.assertEqual(replay_build.check_inputs(table, base, 'input', identities={'lib/other.a': identity}), ['input differs: lib/here.a'])
        self.assertEqual(replay_build.check_inputs(table, base, 'input', identities={'lib/here.a': '0' * 64}), ['input differs: lib/here.a'])
        self.file('lib/here.a', ar([('one.o', elf(HOST_B)), ('two.o', elf(BASE))]))                                    # other objects
        self.assertEqual(replay_build.check_inputs(table, base, 'input', identities={'lib/here.a': identity}), ['input differs: lib/here.a'])
        self.file('lib/here.a', b'not an archive')
        self.assertEqual(replay_build.same_objects(os.path.join(base, 'lib/here.a')), '')
        self.assertEqual(replay_build.check_inputs(table, base, 'input', identities={'lib/here.a': ''}), ['input differs: lib/here.a'])
        self.file('lib/here.a', first)                                                                                # the first machine's bytes: accepted as ever
        self.assertEqual(replay_build.check_inputs(table, base, 'input', identities={'lib/here.a': '0' * 64}), [])
        os.remove(os.path.join(base, 'lib/here.a'))
        self.assertEqual(replay_build.check_inputs(table, base, 'input', identities={'lib/here.a': identity}), ['input missing: lib/here.a'])

    def test_the_command_line_takes_an_identity_only_for_an_input_and_only_as_a_hash(self):
        import json, subprocess
        first = ar([('one.o', elf(BASE))])
        here = ar([('one.o', elf(HOST_B))])
        identity = elf_sections.archive_identity(self.file('first.a', first))[0]
        self.file('root/lib/one.a', here)
        manifest = Path(self.tmp.name) / 'manifest.json'
        manifest.write_text(json.dumps(dict(self.MANIFEST, cwd='/Users/a/repo/build', units=[],
                                            inputs={'lib/one.a': dict(sha256=elf_sections.hashlib.sha256(first).hexdigest(), size=len(first))},
                                            compiler_inputs={}, link=dict(command=[], library='x.so', library_sha256='0' * 64))))
        tool = os.path.join(TOOLS, 'replay_build.py')
        run = lambda *more: subprocess.run([sys.executable, '-B', tool, '--manifest', str(manifest), '--root', self.tmp.name + '/root',
                                            '--compiler', self.tmp.name + '/ndk', '--no-link', '--state-dir', self.tmp.name + '/state', *more], capture_output=True, text=True)
        self.assertIn('input differs: lib/one.a', run().stdout)
        ok = run('--input-identity', 'lib/one.a=' + identity)
        self.assertEqual(ok.returncode, 0, ok.stdout + ok.stderr)
        self.assertIn('0 wrong', ok.stdout)
        for bad in ('lib/one.a', 'lib/never.a=' + identity, 'lib/one.a=xyz', 'lib/one.a=' + identity.upper()):
            refused = run('--input-identity', bad)
            self.assertEqual(refused.returncode, 2, bad)
            self.assertIn('--input-identity wants REL=HEX', refused.stderr)
        self.assertIn('input differs: lib/one.a', run('--input-identity', 'lib/one.a=' + '0' * 64).stdout)

    def test_the_state_folder_is_absolute_whatever_is_named(self):
        # the link runs in the first machine's build folder: a response file named relative to the caller would not be found
        self.assertEqual(replay_build.state_folder('some/state', '/root'), Path(os.getcwd()) / 'some/state')
        self.assertEqual(replay_build.state_folder(None, '/root/r'), Path('/root/r'))
        self.assertEqual(replay_build.state_folder('/abs/state', '/root'), Path('/abs/state'))

    def test_the_command_line_ties_a_pending_input_to_a_run_without_link(self):
        import json, subprocess
        manifest = Path(self.tmp.name) / 'manifest.json'
        manifest.write_text(json.dumps(dict(self.MANIFEST, cwd='/Users/a/repo/build', units=[], inputs={'later.ld': dict(sha256='0' * 64, size=3)},
                                            compiler_inputs={}, link=dict(command=[], library='x.so', library_sha256='0' * 64))))
        tool = os.path.join(TOOLS, 'replay_build.py')
        run = lambda *more: subprocess.run([sys.executable, '-B', tool, '--manifest', str(manifest), '--root', self.tmp.name + '/root',
                                            '--compiler', self.tmp.name + '/ndk', *more], capture_output=True, text=True)
        self.assertEqual(run('--pending-input', 'later.ld').returncode, 2)                        # the link would read it
        self.assertIn('needs --no-link', run('--pending-input', 'later.ld').stderr)
        self.assertIn('names no input', run('--no-link', '--pending-input', 'never.ld').stderr)
        self.assertIn('--map wants REL=DIR', run('--no-link', '--map', 'local').stderr)
        ok = run('--no-link', '--pending-input', 'later.ld', '--state-dir', self.tmp.name + '/state')
        self.assertEqual(ok.returncode, 0, ok.stdout + ok.stderr)
        self.assertIn('0 wrong', ok.stdout)
        self.assertTrue(os.path.isfile(self.tmp.name + '/state/replay-results.json'))               # results where asked, not in the root
        self.assertFalse(os.path.exists(self.tmp.name + '/root/replay-results.json'))
        missing = run('--no-link')
        self.assertEqual(missing.returncode, 2)
        self.assertIn('input missing: later.ld', missing.stdout)

    def test_admission_two_always_then_only_with_room_and_spacing(self):
        rule = lambda running, available, since: replay_build.may_start(running, available, since, 8, 1.5, 1.0, 4.0)
        self.assertTrue(rule(0, 0.0, 0.0))
        self.assertTrue(rule(1, 0.0, 0.0))
        self.assertFalse(rule(2, 2.49, 60.0))      # under reserve + one job's room
        self.assertTrue(rule(2, 2.5, 4.0))
        self.assertFalse(rule(2, 9.0, 3.9))        # the previous start is too recent to have shown its appetite
        self.assertTrue(rule(7, 9.0, 4.0))
        self.assertFalse(rule(8, 99.0, 99.0))      # never more than the job count
        self.assertFalse(replay_build.may_start(1, 99.0, 99.0, 1, 1.5, 1.0, 4.0))

    def test_response_file_quotes_only_what_needs_it(self):
        path = Path(self.tmp.name) / 'link.rsp'
        replay_build.response_file(path, ['-o', 'D:/out/lib.so', 'D:/with space/a.o', '-DX="q"', 'C:\\back\\slash', ''])
        self.assertEqual(path.read_text().splitlines(),
                         ['-o', 'D:/out/lib.so', '"D:/with space/a.o"', '"-DX=\\"q\\""', '"C:\\\\back\\\\slash"', '""'])

    def test_input_check_finds_missing_resized_and_changed_files(self):
        good = self.file('in/a.h', b'int a;\n')
        table = {'a.h': dict(sha256=elf_sections.file_sha256(good), size=7)}
        base = os.path.dirname(good)
        self.assertEqual(replay_build.check_inputs(table, base, 'input'), [])
        self.assertEqual(replay_build.check_inputs({'b.h': table['a.h']}, base, 'input'), ['input missing: b.h'])
        self.file('in/a.h', b'int b;\n')
        self.assertEqual(replay_build.check_inputs(table, base, 'input'), ['input differs: a.h'])
        self.file('in/a.h', b'int a;\n\n')
        self.assertEqual(replay_build.check_inputs(table, base, 'input'), ['input differs: a.h'])


class Freeze(Files):
    def test_paths_are_found_bare_and_behind_options(self):
        words = ['/ndk/bin/clang++', '-I', 'relative/dir', '/abs/x.cpp', '-fprofile-use=/abs/p.profdata', '--sysroot=/ndk/sysroot',
                 '-Wl,--version-script=/abs/e.exports', '-Wl,--symbol-ordering-file=/abs/order.txt', '-DX=/not/a/path/option']
        self.assertEqual(freeze_build.path_words(words), ['/ndk/bin/clang++', '/abs/x.cpp', '/abs/p.profdata', '/ndk/sysroot',
                                                          '/abs/e.exports', '/abs/order.txt'])

    def test_dependency_file_paths_are_taken_as_the_compiler_named_them(self):
        real = os.path.join(self.tmp.name, 'real')
        os.makedirs(os.path.join(real, 'inc'))
        os.symlink(real, os.path.join(self.tmp.name, 'link'))
        cwd = os.path.join(self.tmp.name, 'link', 'build')
        dep = Path(self.file('x.d', b'obj/x.o: \\\n  ../inc/a.h \\\n  /abs/b.h rel/c.h\n'))
        self.assertEqual(freeze_build.dependency_paths(dep, cwd),
                         [os.path.join(self.tmp.name, 'link', 'inc', 'a.h'), '/abs/b.h', os.path.join(cwd, 'rel', 'c.h')])

    def test_a_dependency_path_with_spaces_stays_one_path(self):
        import subprocess
        # a real compiler's dependency file, with a header in a folder named with spaces next to a plain one (control)
        src = os.path.join(self.tmp.name, 'src'); os.makedirs(os.path.join(src, 'headers with spaces')); os.makedirs(os.path.join(src, 'plain'))
        Path(src, 'headers with spaces', 'config.h').write_text('#define A 1\n'); Path(src, 'plain', 'b.h').write_text('#define B 2\n')
        Path(src, 'x.cpp').write_text('#include "headers with spaces/config.h"\n#include "plain/b.h"\nint x = A + B;\n')
        dep = Path(src, 'x.d')
        subprocess.run(['clang++', '-c', 'x.cpp', '-o', 'x.o', '-MD', '-MF', 'x.d'], cwd=src, check=True)
        paths = freeze_build.dependency_paths(dep, src)
        self.assertEqual(paths, [os.path.join(src, 'x.cpp'), os.path.join(src, 'headers with spaces', 'config.h'), os.path.join(src, 'plain', 'b.h')])
        self.assertTrue(all(os.path.isfile(p) for p in paths))

    def test_make_escapes_in_a_dependency_file(self):
        words = freeze_build.make_dependency_words('obj/a\\ b.o: a\\ b.cpp \\\n  cost$$.h C:\\sdk\\x.h\n')
        self.assertEqual(words, ['a b.cpp', 'cost$.h', 'C:\\sdk\\x.h'])
        with self.assertRaises(SystemExit):
            freeze_build.make_dependency_words('no separator here\n')

    def test_the_compilers_host_folder_comes_from_the_driver(self):
        self.assertEqual(freeze_build.compiler_root(['/ndk/prebuilt/darwin-x86_64/bin/clang++', '-c']), '/ndk/prebuilt/darwin-x86_64')
        for driver in ('/usr/bin/g++', '/ndk/prebuilt/darwin-x86_64/clang++', '/ndk/bin/clang'):
            with self.assertRaises(SystemExit):
                freeze_build.compiler_root([driver, '-c'])


if __name__ == '__main__':
    unittest.main()
