#!/usr/bin/env python3
"""Mutant check for make_notices.py: copies of the tool with one rule changed; test_make_notices.py (beside this file)
must fail on each. First two controls: the tool as it is, and an unchanged copy. Exits 1 if a control fails, a mutant
survives or no longer applies. The tool itself is not touched."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL = HERE.parent / 'make_notices.py'
TESTS = HERE / 'test_make_notices.py'
MUTANTS = [
    ('another terms file than the pinned one is taken', "    if sha256(whole) != text.get('sha256'):\n", "    if False:\n"),
    ('a missing terms file is not noticed here', "        if not path.is_file():\n            raise Refusal(f'the terms file is not there: {root}:{text[\"path\"]}')\n", ""),
    ('lines outside the file are taken', "        if not (1 <= first <= last <= len(lines)):\n", "        if False:\n"),
    ('the first line of an extract is dropped', "        whole = b'\\n'.join(lines[first - 1:last]) + b'\\n'\n", "        whole = b'\\n'.join(lines[first:last]) + b'\\n'\n"),
    ('the whole file is taken for an extract', "        whole = b'\\n'.join(lines[first - 1:last]) + b'\\n'\n", "        whole = whole\n"),
    ('an empty extract is taken', "        if not whole.strip():\n", "        if False:\n"),
    ('a root that was not given is looked for in the current folder', "    if root not in roots:\n        raise Refusal(f'no --root was given for \"{root}\"')\n    base = roots[root]\n", "    base = roots.get(root, Path('.'))\n"),
    ('a root given twice takes the later one', "        if name in roots:\n            raise Refusal(f'--root {name} is given twice')\n", ""),
    ('two parts may share an id', "        if part['id'] in seen:\n", "        if False:\n"),
    ('a part needs no terms text and no reason', "        if not ways:\n", "        if False:\n"),
    ('a part needs no place in the package', "        for key in ('id', 'name', 'origin', 'terms', 'in_package'):\n", "        for key in ('id', 'name', 'origin', 'terms'):\n"),
    ('any table is read', "    if table.get('schema') != SCHEMA:\n", "    if False:\n"),
    ('a pending part does not stop the run', "    if pending and not allow_pending:\n", "    if False:\n"),
    ('incomplete notices do not say so', "    if pending:\n        out.append('INCOMPLETE: the terms texts of these parts are still to be collected: ' + ', '.join(pending) + '.')\n        out.append('')\n", ""),
    ('the record calls incomplete notices complete', "'complete': not pending,", "'complete': True,"),
    ('programs of the package are not checked', "        if missing:\n            raise Refusal('programs in the package", "        if False:\n            raise Refusal('programs in the package"),
    ('a runtime linked into everything covers a new program', "    patterns = [pattern for part in parts if not part.get('general') for pattern in part['in_package']]\n", "    patterns = [pattern for part in parts for pattern in part['in_package']]\n"),
    ('a dll is not a program', "PROGRAM_SUFFIXES = ('.exe', '.dll', '.pyd')", "PROGRAM_SUFFIXES = ('.exe', '.pyd')"),
    ('program suffixes are compared with their case', "            if Path(name).suffix.lower() in PROGRAM_SUFFIXES", "            if Path(name).suffix in PROGRAM_SUFFIXES"),
    ('backslashes in the package list are kept', "        name = line.split('\\t')[0].strip().replace('\\\\', '/')\n", "        name = line.split('\\t')[0].strip()\n"),
    ('the whole line of the package list is the name', "        name = line.split('\\t')[0].strip().replace('\\\\', '/')\n", "        name = line.strip().replace('\\\\', '/')\n"),
    ('an existing output folder is written into', "    if out.exists():\n        raise Refusal(f'the output folder exists already: {out}')\n", ""),
    ('text that is not UTF-8 is written with replacement marks', "                body = data.decode('utf-8')\n", "                body = data.decode('utf-8', 'replace')\n"),
    ('the texts in the folder are the cleaned ones', "            files[place] = data\n", "            files[place] = data.replace(b'\\r\\n', b'\\n')\n"),
    ('the record holds the pinned hash as the output hash', "'sha256': sha256(data), 'source_sha256': source['sha256']", "'sha256': source['sha256'], 'source_sha256': source['sha256']"),
    ('the record does not hold the notices\' hash', "'notices_sha256': sha256(notices),", "'notices_sha256': sha256(b''),"),
    ('the note of a part is left out', "        if part.get('note'):\n            out.append(part['note'])\n", ""),
    ('the revision is left out', " + (f' at {part[\"revision\"]}' if part.get('revision') else '')", ""),
    ('where a text travels is left out', "        if part.get('travels'):\n            out.append(f'Its terms travel with it: {part[\"travels\"]}')\n", ""),
    ('what is fetched is left out', "        if part.get('fetched'):\n            out.append(f'Not carried by this package: {part[\"fetched\"]}')\n", ""),
    ('a check writes the notices', "        if args.check:\n            render(gathered, pending)\n", "        if args.check:\n            write(gathered, pending, Path(args.components).parent / 'checked')\n"),
    ('--out and --check together are taken', "        if args.check == (args.out is not None):\n", "        if not args.check and args.out is None:\n"),
    ('a missing archive member raises instead of refusing', "            except KeyError:\n                raise Refusal(f'{base.name} has no member {text[\"member\"]}') from None\n", "            except ZeroDivisionError:\n                pass\n"),
]


def run(tool):
    return subprocess.run([sys.executable, '-B', str(TESTS)], capture_output=True, text=True, env=dict(os.environ, MAKE_NOTICES=str(tool)))


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
