#!/usr/bin/env python3
"""Mutant check for make_sources.py: copies of the tool with one rule changed; test_make_sources.py (beside this file)
must fail on each. First two controls: the tool as it is, and an unchanged copy. Exits 1 if a control fails, a mutant
survives or no longer applies. The tool itself is not touched."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL = HERE.parent / 'make_sources.py'
TESTS = HERE / 'test_make_sources.py'
MUTANTS = [
    ('any commit is taken', "    if head != commit:\n", "    if False:\n"),
    ('any changes are taken', "    if found != diff_sha256:\n", "    if False:\n"),
    ('any archive is taken', "    if found != archive_sha256:\n", "    if False:\n"),
    ('untracked files are not taken', "'ls-files', '-z', '--cached', '--others', '--exclude-standard'", "'ls-files', '-z', '--cached'"),
    ('ignored files are taken too', "'ls-files', '-z', '--cached', '--others', '--exclude-standard'", "'ls-files', '-z', '--cached', '--others'"),
    ('a missing tracked file is passed over', "        elif not path.exists():\n            raise Refusal(f'a tracked file is missing from the generator checkout: {name}')\n", ""),
    ('a link in the checkout is written as the file it points to', "        if path.is_symlink():\n            links.append(name)\n        elif path.is_file():", "        if path.is_file():"),
    ('a link in the archive is passed over unlisted', "            elif member.issym() or member.islnk():\n                links.append(name)\n", "            elif member.issym() or member.islnk():\n                pass\n"),
    ('nothing is left out', "            if any(name == prefix.rstrip('/') or name.startswith(prefix.rstrip('/') + '/') for prefix in leave_out):\n", "            if False:\n"),
    ('a name that only begins like the prefix is left out', "name.startswith(prefix.rstrip('/') + '/')", "name.startswith(prefix.rstrip('/'))"),
    ('what is left out is not listed', "                left.append(name)\n", "                pass\n"),
    ('a name with .. is taken', "    if path.is_absolute() or not parts or any(part == '..' for part in parts):\n", "    if path.is_absolute() or not parts:\n"),
    ('an absolute name is taken', "    if path.is_absolute() or not parts or any(part == '..' for part in parts):\n", "    if not parts or any(part == '..' for part in parts):\n"),
    ('something that is no file is passed over', "            else:\n                raise Refusal(f'{archive.name} holds something that is neither a file nor a link: {name}')\n", ""),
    ('forbidden text is not looked for', "        if word.encode('utf-8') in data:\n", "        if False:\n"),
    ('forbidden text is looked for in the archive\'s files only', "    for folder, files in (('XenonRecomp', gen_files), ('host-programs', host)):\n        for name, data in files.items():\n            check_text(f'{folder}/{name}', data, forbidden)\n", "    for folder, files in (('XenonRecomp', gen_files), ('host-programs', host)):\n        for name, data in files.items():\n            if folder != 'XenonRecomp':\n                check_text(f'{folder}/{name}', data, forbidden)\n"),
    ('a file with a zero byte is searched like text', "    if b'\\0' in data[:8192]:\n        return", "    if False:\n        return"),
    ('an existing output folder is written into', "    if out.exists():\n        raise Refusal(f'the output folder exists already: {out}')\n", ""),
    ('an empty set is written', "    if not gen_files or not host:\n", "    if False:\n"),
    ('the record holds the name\'s hash', "'sha256': sha256(data), 'size': len(data)", "'sha256': sha256(name.encode()), 'size': len(data)"),
    ('the record holds no sizes', "'sha256': sha256(data), 'size': len(data)", "'sha256': sha256(data), 'size': 0"),
    ('the record does not name the checked commit', "'generator': {'commit': commit,", "'generator': {'commit': '',"),
    ('the record does not name the checked archive', "'host_programs': {'archive_sha256': host_sha256,", "'host_programs': {'archive_sha256': '',"),
    ('the generator\'s files go beside the others', "    for folder, files in (('XenonRecomp', gen_files), ('host-programs', host)):", "    for folder, files in (('host-programs', gen_files), ('host-programs', host)):"),
    ('the changed files are not named', "    out += [f'  changed  {entry[\"last_written\"]}  {entry[\"path\"]}' for entry in changes['changed']] or ['  (none)']\n", "    out += ['  (none)']\n"),
    ('the added files are named as changed', "'added': [{'path': name, 'last_written': day(name)} for name in added if name in files]}", "'added': []}\n    changes['changed'] += [{'path': name, 'last_written': day(name)} for name in added if name in files]"),
    ('every tracked file is named as changed', "git(tree, 'diff', '--name-only', '-z')", "git(tree, 'ls-files', '-z')"),
    ('the base tree\'s files are listed as this project\'s', "for name in sorted(host) if not name.startswith(base)]", "for name in sorted(host)]"),
    ('the changes file is not written', "        (out / 'CHANGES.txt').write_text(changes_text(commit, changes, host, base_prefix, note_text), encoding='utf-8')\n", ""),
    ('the note is left out', "    if note:\n        out += ['', note.rstrip('\\n')]\n", ""),
    ('the note is not searched for forbidden text', "    check_text(f'the note {note.name if note else \"\"}', note_text.encode('utf-8'), forbidden)\n", ""),
    ('the committed text is written, not the checkout\'s', "            files[name] = path.read_bytes()\n", "            files[name] = git(tree, 'show', f'HEAD:{name}') if name != 'src/added_test.cc' else path.read_bytes()\n"),
]


def run(tool):
    return subprocess.run([sys.executable, '-B', str(TESTS)], capture_output=True, text=True, env=dict(os.environ, MAKE_SOURCES=str(tool)))


def main() -> int:
    source = TOOL.read_text(encoding='utf-8')
    if run(TOOL).returncode != 0:
        print('CONTROL FAILED: the tests do not pass on the tool as it is')
        return 1
    print('control: the tests pass on the tool as it is', flush=True)
    killed = 0
    with tempfile.TemporaryDirectory() as folder:
        same = Path(folder) / 'unchanged.py'
        same.write_text(source, encoding='utf-8', newline='\n')
        if run(same).returncode != 0:
            print('CONTROL FAILED: an unchanged copy of the tool does not pass (a mutant would be killed by being a copy)')
            return 1
        print('control: an unchanged copy passes too', flush=True)
        for index, (what, old, new) in enumerate(MUTANTS, 1):
            if source.count(old) != 1:
                print(f'mutant {index:3d} DOES NOT APPLY ({source.count(old)} matches): {what}')
                continue
            copy = Path(folder) / f'mutant_{index}.py'
            copy.write_text(source.replace(old, new), encoding='utf-8', newline='\n')
            if run(copy).returncode == 0:
                print(f'mutant {index:3d} SURVIVED: {what}')
            else:
                killed += 1
                print(f'mutant {index:3d} killed: {what}')
    print(f'{killed} of {len(MUTANTS)} mutants killed')
    return 0 if killed == len(MUTANTS) else 1


if __name__ == '__main__':
    sys.exit(main())
