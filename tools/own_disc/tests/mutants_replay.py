#!/usr/bin/env python3
"""Mutant check for elf_sections.py, replay_build.py and freeze_build.py: each mutant is a copy of the three tools with
one rule removed or one value changed; test_replay_tools.py (beside this file) must fail on it.

  mutants_replay.py [--only TEXT]

First a control: the tests must pass on unchanged copies placed where the mutants are placed. Exits 1 if the control
fails, a mutant survives, or a mutant no longer applies to the source. The tools themselves are not touched.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent
FILES = ('elf_sections.py', 'replay_build.py', 'freeze_build.py')
TESTS = HERE / 'test_replay_tools.py'

MUTANTS = [
    ('elf_sections.py', 'anything is read as an archive',
     "    if data[:8] != AR_MAGIC:\n        raise ValueError(f'not an ar archive: {path}')\n", ''),
    ('elf_sections.py', 'a damaged member header is read on',
     "        if len(head) != 60 or head[58:60] != b'`\\n' or not head[48:58].strip().isdigit():", '        if len(head) != 60 or not head[48:58].strip().isdigit():'),
    ('elf_sections.py', 'a member size that is no number is tried',
     "        if len(head) != 60 or head[58:60] != b'`\\n' or not head[48:58].strip().isdigit():", "        if len(head) != 60 or head[58:60] != b'`\\n':"),
    ('elf_sections.py', 'a cut-off member is taken',
     "        if len(body) != size:\n            raise ValueError(f'truncated member in {path}')\n", ''),
    ('elf_sections.py', 'members are not padded to even offsets',
     '        at += 60 + size + (size & 1)             # members start at even offsets', '        at += 60 + size'),
    ('elf_sections.py', 'the symbol table counts as a member',
     "        if raw in (b'/', b'/SYM64/'):            # the symbol table\n            continue\n", "        if raw in (b'/SYM64/',):\n            continue\n"),
    ('elf_sections.py', 'the wide symbol table counts as a member',
     "        if raw in (b'/', b'/SYM64/'):            # the symbol table", "        if raw in (b'/',):"),
    ('elf_sections.py', 'long names are not looked up',
     '            name = names[start:end]\n', '            name = raw\n'),
    ('elf_sections.py', 'a name outside the table is taken',
     "            if end < 0 or start >= len(names):\n                raise ValueError(f'member name outside the name table in {path}')\n", ''),
    ('elf_sections.py', 'a name of another archive format is taken as it is',
     "        else:\n            raise ValueError(f'member name of another archive format in {path}')\n", '        else:\n            name = raw\n'),
    ('elf_sections.py', 'an archive without members has an identity',
     "    if not members:\n        raise ValueError(f'no members in {path}')\n", ''),
    ('elf_sections.py', 'member names are not part of the library',
     "    plain = [[name, identity_of(sections_of(body, f'{path}({name})'))[0]] for name, body in members]", "    plain = [identity_of(sections_of(body, f'{path}({name})'))[0] for name, body in members]"),
    ('elf_sections.py', 'member order is not part of the library',
     "    plain = [[name, identity_of(sections_of(body, f'{path}({name})'))[0]] for name, body in members]", "    plain = sorted([name, identity_of(sections_of(body, f'{path}({name})'))[0]] for name, body in members)"),
    ('elf_sections.py', 'members count by their bytes, compiler name and all',
     "    plain = [[name, identity_of(sections_of(body, f'{path}({name})'))[0]] for name, body in members]", '    plain = [[name, hashlib.sha256(body).hexdigest()] for name, body in members]'),
    ('elf_sections.py', 'the member count is always one',
     '    return hashlib.sha256(json.dumps(plain).encode()).hexdigest(), len(plain)', '    return hashlib.sha256(json.dumps(plain).encode()).hexdigest(), 1'),
    ('elf_sections.py', 'the command line knows no archives',
     "    if len(sys.argv) > 2 and sys.argv[1] == '--archive':", '    if False:'),
    ('replay_build.py', 'a named library is taken whatever its objects',
     '            if not found or found != identities[rel]:', '            if rel not in (identities or {}):'),
    ('replay_build.py', 'a named file needs no objects at all',
     '            if not found or found != identities[rel]:', '            if found != (identities or {}).get(rel, None):'),
    ('replay_build.py', 'every differing input is looked at as a library',
     "            found = same_objects(path) if rel in (identities or {}) else ''", '            found = same_objects(path)\n            identities = dict(identities or {}, **{rel: (identities or {}).get(rel, found)})'),
    ('replay_build.py', 'the command line takes an identity for anything',
     "any(rel not in manifest['inputs'] or not re.fullmatch('[0-9a-f]{64}', value) for rel, value in identities.items())", "any(not re.fullmatch('[0-9a-f]{64}', value) for rel, value in identities.items())"),
    ('replay_build.py', 'the command line takes any text as an identity',
     "any(rel not in manifest['inputs'] or not re.fullmatch('[0-9a-f]{64}', value) for rel, value in identities.items())", "any(rel not in manifest['inputs'] for rel, value in identities.items())"),
    ('replay_build.py', 'an identity without a name is dropped silently',
     '    if len(identities) != len(args.input_identity) or any(', '    if any('),
    ('replay_build.py', 'the identities are not handed to the check',
     "        wrong = check_inputs(manifest['inputs'], tr.local, 'input', set(args.pending_input), identities) \\", "        wrong = check_inputs(manifest['inputs'], tr.local, 'input', set(args.pending_input)) \\"),
    ('replay_build.py', 'a moved folder is replaced after the root',
     "pattern = re.compile('|'.join(re.escape(old) for old in sorted(table, key=len, reverse=True)))", "pattern = re.compile('|'.join(re.escape(old) for old in sorted(table, key=len)))"),
    ('replay_build.py', 'a replaced path is replaced again',
     "        self._replace = lambda w: pattern.sub(lambda m: table[m.group(0)], w)", "        self._replace = lambda w: pattern.sub(lambda m: table[m.group(0)], pattern.sub(lambda m: table[m.group(0)], w))"),
    ('replay_build.py', 'moved folders are not put into the command words',
     "            + [(manifest['repo_root'] + '/' + rel, new) for rel, new in self.maps] + [(manifest['repo_root'], self.root)]", "            + [(manifest['repo_root'], self.root)]"),
    ('replay_build.py', 'the shortest moved folder is tried first for an input',
     "key=lambda m: -len(m[0]))", "key=lambda m: len(m[0]))"),
    ('replay_build.py', 'a name that only starts like a moved folder is moved',
     "            if rel == moved or rel.startswith(moved + '/'):", "            if rel.startswith(moved):"),
    ('replay_build.py', 'inputs of a moved folder are looked for under the root',
     "                return new + rel[len(moved):]\n", "                return self.root + '/' + rel\n"),
    ('replay_build.py', 'an empty or climbing or repeated map is accepted',
     "        if any(not rel or '..' in rel.split('/') for rel, _ in self.maps) or len({rel for rel, _ in self.maps}) != len(self.maps):\n            raise ValueError('--map wants REL=DIR with a path below the root, each once')\n", ""),
    ('replay_build.py', 'a repeated map is accepted',
     " or len({rel for rel, _ in self.maps}) != len(self.maps):", ":"),
    ('replay_build.py', 'a climbing map is accepted',
     "        if any(not rel or '..' in rel.split('/') for rel, _ in self.maps)", "        if any(not rel for rel, _ in self.maps)"),
    ('replay_build.py', 'the moved folder is mapped back before the root',
     "        ordered = [self.pairs[0], self.pairs[-1]] + self.pairs[-2:0:-1]", "        ordered = list(self.pairs)"),
    ('replay_build.py', 'the longest moved folder is mapped back first of the moved ones',
     "        ordered = [self.pairs[0], self.pairs[-1]] + self.pairs[-2:0:-1]", "        ordered = [self.pairs[0], self.pairs[-1]] + self.pairs[1:-1]"),
    ('replay_build.py', 'every input may be pending',
     "        if rel in pending:\n            continue\n", "        if pending:\n            continue\n"),
    ('replay_build.py', 'a pending input is checked all the same',
     "        if rel in pending:\n            continue\n", ""),
    ('replay_build.py', 'a pending input is accepted for a run that links',
     "    if args.pending_input and not args.no_link:\n        ap.error('--pending-input needs --no-link: the link reads every input')\n", ""),
    ('replay_build.py', 'a pending input the manifest does not name is accepted',
     "    if any(rel not in manifest['inputs'] for rel in args.pending_input):\n        ap.error('--pending-input names no input of the manifest')\n", ""),
    ('replay_build.py', 'a map without a folder is accepted',
     "    if any('=' not in m for m in args.map):\n        ap.error('--map wants REL=DIR')\n", "    args.map = [m for m in args.map if '=' in m]\n"),
    ('replay_build.py', 'results go into the root whatever is asked',
     "    state_dir = state_folder(args.state_dir, root)\n", "    state_dir = state_folder(None, root)\n"),
    ('replay_build.py', 'the state folder stays as it was named',
     "    return Path(named or root).absolute()\n", "    return Path(named or root)\n"),
    ('replay_build.py', 'the state folder is always the root',
     "    return Path(named or root).absolute()\n", "    return Path(root).absolute()\n"),
    ('replay_build.py', 'the pending inputs are not handed to the check',
     "tr.local, 'input', set(args.pending_input), identities)", "tr.local, 'input', set(), identities)"),

    # ---- what counts as the same code
    ('elf_sections.py', "the compiler's name is part of the identity",
     r"LEFT_OUT = re.compile(r'^\.comment$|^\.(rela\.)?debug_')", r"LEFT_OUT = re.compile(r'^\.(rela\.)?debug_')"),
    ('elf_sections.py', 'relocations of debug sections are part of the identity',
     r"^\.(rela\.)?debug_')", r"^\.debug_')"),
    ('elf_sections.py', 'every relocation section is left out, also those of code',
     r"LEFT_OUT = re.compile(r'^\.comment$|^\.(rela\.)?debug_')", r"LEFT_OUT = re.compile(r'^\.comment$|^\.rela|^\.debug_')"),
    ('elf_sections.py', 'any section whose name starts like the comment section is left out',
     r"r'^\.comment$|", r"r'^\.comment|"),
    ('elf_sections.py', 'any section whose name starts with .debug is left out',
     r"^\.(rela\.)?debug_')", r"^\.(rela\.)?debug')"),
    ('elf_sections.py', 'section contents are not part of the identity',
     "s['align'], s['entsize'], s['sha256']) for s in kept]", "s['align'], s['entsize']) for s in kept]"),
    ('elf_sections.py', 'section flags are not part of the identity',
     "plain = [(s['name'], s['type'], s['flags'], s['size'],", "plain = [(s['name'], s['type'], s['size'],"),
    ('elf_sections.py', 'section sizes are not part of the identity',
     "s['flags'], s['size'], s['align'],", "s['flags'], s['align'],"),
    ('elf_sections.py', 'sections without file bytes are read from the file',
     "SHT_NOBITS = 8", "SHT_NOBITS = 9"),
    ('elf_sections.py', 'the long form of the section count is not read',
     "            shnum = shnum or first[5]\n", ""),
    ('elf_sections.py', 'the long form of the string table index is not read',
     "shstrndx = first[6] if shstrndx == 0xFFFF else shstrndx", "shstrndx = shstrndx"),
    ('elf_sections.py', 'a section cut off by the end of the file is accepted',
     "        if len(body) != (0 if kind == SHT_NOBITS else size):", "        if False:"),
    ('elf_sections.py', 'a cut-off section table is not reported as such',
     "    except struct.error:\n        raise ValueError(f'truncated section table in {path}') from None", "    except ZeroDivisionError:\n        raise"),
    ('elf_sections.py', 'any file is taken for ELF',
     "    if data[:6] != b'\\x7fELF\\x02\\x01':", "    if False:"),
    ('elf_sections.py', 'diff ignores sections only the second file has',
     "    for key in list(x) + [k for k in y if k not in x]:", "    for key in list(x):"),
    ('elf_sections.py', 'diff compares sizes only',
     "            out[(s['name'], n)] = (s['type'], s['flags'], s['size'], s['sha256'])", "            out[(s['name'], n)] = (s['type'], s['flags'], s['size'])"),
    # ---- carrying a command to another machine
    ('replay_build.py', "the repository root is not translated",
     " + [(manifest['repo_root'], self.root)]", " + [(manifest['repo_root'], manifest['repo_root'])]"),
    ('replay_build.py', "the compiler root is not translated",
     "        self.pairs = [(manifest['compiler_root'], self.compiler)] \\", "        self.pairs = [(manifest['compiler_root'], manifest['compiler_root'])] \\"),
    ('replay_build.py', 'backslashes of the local root are kept',
     "        clean = lambda path: path.replace('\\\\', '/').rstrip('/')", "        clean = lambda path: path.rstrip('/')"),
    ('replay_build.py', 'the driver gets no Windows name',
     "        if WINDOWS and not out[0].lower().endswith('.exe'):", "        if False:"),
    ('replay_build.py', 'the driver gets the Windows name everywhere',
     "        if WINDOWS and not out[0].lower().endswith('.exe'):", "        if not out[0].lower().endswith('.exe'):"),
    ('replay_build.py', 'path maps point the wrong way',
     "        flags = [f'-ffile-prefix-map={new}={old}' for old, new in ordered]", "        flags = [f'-ffile-prefix-map={old}={new}' for old, new in ordered]"),
    ('replay_build.py', 'macro paths keep the host separator',
     "        return flags + ['-ffile-reproducible', f'-fdebug-compilation-dir={cwd_there}']", "        return flags + [f'-fdebug-compilation-dir={cwd_there}']"),
    ('replay_build.py', 'the debug compilation directory is not pinned',
     "        return flags + ['-ffile-reproducible', f'-fdebug-compilation-dir={cwd_there}']", "        return flags + ['-ffile-reproducible']"),
    # ---- how many compiles at once
    ('replay_build.py', 'three compiles may always run',
     "    if running < 2:\n        return True", "    if running < 3:\n        return True"),
    ('replay_build.py', 'only one compile may always run',
     "    if running < 2:\n        return True", "    if running < 1:\n        return True"),
    ('replay_build.py', 'the job count is no limit',
     "    if running >= jobs:\n        return False\n", ""),
    ('replay_build.py', "a further compile needs no room of its own",
     "    return available >= reserve + per_job and since_last_start >= spacing", "    return available >= reserve and since_last_start >= spacing"),
    ('replay_build.py', 'starts are not spaced',
     "    return available >= reserve + per_job and since_last_start >= spacing", "    return available >= reserve + per_job"),
    ('replay_build.py', 'memory is not looked at',
     "    return available >= reserve + per_job and since_last_start >= spacing", "    return since_last_start >= spacing"),
    # ---- response file and input check
    ('replay_build.py', 'words with spaces are written bare',
     "if (not w or any(c in w for c in ' \\t\"\\\\')) else w", "if (not w or any(c in w for c in '\\t\"\\\\')) else w"),
    ('replay_build.py', 'quotes inside a word are not escaped',
     ".replace('\"', '\\\\\"') + '\"'", " + '\"'"),
    ('replay_build.py', 'an empty word vanishes',
     "if (not w or any(c in w", "if (any(c in w"),
    ('replay_build.py', 'a missing input is not reported',
     "        if not os.path.isfile(path):\n            wrong.append(f'{what} missing: {rel}')\n        elif", "        if not os.path.isfile(path):\n            pass\n        elif"),
    ('replay_build.py', 'an input of another size with the same start is accepted',
     "        elif os.path.getsize(path) != want['size'] or elf_sections.file_sha256(path) != want['sha256']:",
     "        elif os.path.getsize(path) < want['size']:"),
    ('replay_build.py', 'only the size of an input is checked',
     "        elif os.path.getsize(path) != want['size'] or elf_sections.file_sha256(path) != want['sha256']:",
     "        elif os.path.getsize(path) != want['size']:"),
    # ---- reading the first machine's build
    ('freeze_build.py', 'paths behind the profile option are missed',
     "PATH_OPTIONS = ('-fprofile-use=', ", "PATH_OPTIONS = ("),
    ('freeze_build.py', 'paths behind the version script option are missed',
     "'-Wl,--version-script=',\n", "\n"),
    ('freeze_build.py', 'relative words are taken for paths',
     "        if w.startswith('/'):\n            found.append(w)", "        if w.startswith('/') or '/' in w:\n            found.append(w)"),
    ('freeze_build.py', 'dependency paths are not made absolute',
     "        result.append(os.path.normpath(p if os.path.isabs(p) else os.path.join(cwd, p)))", "        result.append(os.path.normpath(p))"),
    ('freeze_build.py', 'dependency paths follow links',
     "        result.append(os.path.normpath(p if os.path.isabs(p) else os.path.join(cwd, p)))",
     "        result.append(os.path.realpath(p if os.path.isabs(p) else os.path.join(cwd, p)))"),
    ('freeze_build.py', 'line continuations of the dependency file are not joined',
     "    text = depfile.read_text().replace('\\\\\\n', ' ')", "    text = depfile.read_text()"),
    ('freeze_build.py', 'any driver is accepted as the compiler',
     "    if driver.name != 'clang++' or driver.parent.name != 'bin':", "    if False:"),
    ('freeze_build.py', "the driver's folder need not be bin",
     "    if driver.name != 'clang++' or driver.parent.name != 'bin':", "    if driver.name != 'clang++':"),
]


def run(folder: Path) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, '-B', str(TESTS), '-v', '--failfast'], env=dict(os.environ, REPLAY_TOOLS_DIR=str(folder)),
                          capture_output=True, text=True, timeout=120)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--only', help='run only the mutants whose description contains this text')
    a = ap.parse_args()
    sources = {f: (TOOLS / f).read_text() for f in FILES}
    chosen = [(i, m) for i, m in enumerate(MUTANTS, 1) if not a.only or a.only in m[1]]
    killed = 0
    with tempfile.TemporaryDirectory() as tmp:
        control = Path(tmp) / 'control'
        control.mkdir()
        for f in FILES:
            shutil.copy(TOOLS / f, control / f)
        r = run(control)
        if r.returncode != 0:
            print('CONTROL FAILED: the tests do not pass on unchanged copies; no mutant result would mean anything\n' + r.stderr[-2000:])
            return 1
        print('control: the tests pass on unchanged copies of the tools', flush=True)
        for index, (name, what, old, new) in chosen:
            if sources[name].count(old) != 1:
                print(f'mutant {index:3d} DOES NOT APPLY ({sources[name].count(old)} matches in {name}): {what}')
                continue
            folder = Path(tmp) / f'm{index}'
            folder.mkdir()
            for f in FILES:
                (folder / f).write_text(sources[f].replace(old, new) if f == name else sources[f])
            r = run(folder)
            failed = [line.split(' ... ')[0].strip() for line in r.stderr.splitlines() if line.rstrip().endswith(('... FAIL', '... ERROR'))]
            if r.returncode == 0:
                print(f'mutant {index:3d} SURVIVED ({name}): {what}')
            else:
                killed += 1
                print(f'mutant {index:3d} killed by {failed[0].split(" (")[0] if failed else "the test run (it could not start)"}: {what}')
    print(f'{killed} of {len(chosen)} mutants killed')
    return 0 if killed == len(chosen) else 1


if __name__ == '__main__':
    sys.exit(main())
