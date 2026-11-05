#!/usr/bin/env python3
"""Mutant check for compile_closure.py: each mutant is a copy of the tool with one rule removed or changed;
test_compile_closure.py (beside this file) must fail on it. First a control on an unchanged copy. Exits 1 if the
control fails, a mutant survives, or a mutant no longer applies. The tool itself is not touched.

  mutants_closure.py [--only TEXT]
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL = HERE.parent / 'compile_closure.py'
TESTS = HERE / 'test_compile_closure.py'

MUTANTS = [
    ('a folder is matched as a prefix of the name', "return path == folder or path.startswith(folder + '/')", "return path.startswith(folder)"),
    ('files of the work folder are not set apart', "        if under(path, work):\n", "        if False:\n"),
    ('tracked files are not set apart', "        if path in tracked:\n", "        if False:\n"),
    ('a file entry also covers what lies under that name', "else path == e['path'])]", "else under(path, e['path']))]"),
    ('a folder entry covers only a file of exactly its name', "(under(path, e['path']) if e['path'].endswith('/') else path == e['path'])", "(path == e['path'])"),
    ('the least specific entry owns a file', "covered[max(owners, key=len)].append(path)", "covered[min(owners, key=len)].append(path)"),
    ('what nobody declared counts as declared', "            groups['undeclared'].append(path)", "            groups['declared'].append(path)"),
    ('an undeclared file does not fail the check', "    return 0 if not result['groups']['undeclared'] else 1", "    return 0"),
    ('an absolute work folder is accepted', "    if not work.strip('/') or work.startswith('/'):", "    if False:"),
    ('an absolute declared path is accepted', "                or entry['path'].startswith('/') or '..' in entry['path'].split('/') \\\n", "                or '..' in entry['path'].split('/') \\\n"),
    ('a declared path may climb out with ..', "or entry['path'].startswith('/') or '..' in entry['path'].split('/') \\\n", "or entry['path'].startswith('/') \\\n"),
    ('an empty declared path is accepted', "or not entry['path'].strip('/') \\\n", "\\\n"),
    ('game_derived may be any value', "type(entry.get('game_derived')) is not bool", "entry.get('game_derived') is None"),
    ('an entry need not say what it is', " or not isinstance(entry.get('what'), str) or not entry['what']:", ":"),
    ('a path may be declared twice', "        if entry['path'] in seen:\n            raise ValueError('declared twice: ' + entry['path'])\n", ""),
    ('the declaration schema is not checked', "or table.get('schema') != DECLARED_SCHEMA ", ""),
    ('sizes are not added up', "    size = lambda paths: sum(int(inputs[p].get('size', 0)) for p in paths)", "    size = lambda paths: 0"),
    ('declared entries nobody read are not reported', "            'unused': [e['path'] for e in declared if not covered[e['path']]]}", "            'unused': []}"),
    ('a manifest without inputs is accepted', " or not manifest['inputs']:", ":"),
    ('the report names no undeclared file', "    for path in result['groups']['undeclared']:\n        print(f'  UNDECLARED", "    for path in []:\n        print(f'  UNDECLARED"),
]


def run(folder: Path) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, '-B', str(TESTS), '-v', '--failfast'], env=dict(os.environ, CLOSURE_TOOL_DIR=str(folder)),
                          capture_output=True, text=True, timeout=120)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--only')
    a = ap.parse_args()
    source = TOOL.read_text()
    chosen = [(i, m) for i, m in enumerate(MUTANTS, 1) if not a.only or a.only in m[0]]
    killed = 0
    with tempfile.TemporaryDirectory() as tmp:
        control = Path(tmp) / 'control'
        control.mkdir()
        (control / TOOL.name).write_text(source)
        r = run(control)
        if r.returncode != 0:
            print('CONTROL FAILED: the tests do not pass on an unchanged copy\n' + r.stderr[-2000:])
            return 1
        print('control: the tests pass on an unchanged copy of the tool', flush=True)
        for index, (what, old, new) in chosen:
            if source.count(old) != 1:
                print(f'mutant {index:3d} DOES NOT APPLY ({source.count(old)} matches): {what}')
                continue
            folder = Path(tmp) / f'm{index}'
            folder.mkdir()
            (folder / TOOL.name).write_text(source.replace(old, new))
            r = run(folder)
            failed = [line.split(' ... ')[0].strip() for line in r.stderr.splitlines() if line.rstrip().endswith(('... FAIL', '... ERROR'))]
            if r.returncode == 0:
                print(f'mutant {index:3d} SURVIVED: {what}')
            else:
                killed += 1
                print(f'mutant {index:3d} killed by {failed[0].split(" (")[0] if failed else "the test run (it could not start)"}: {what}')
    print(f'{killed} of {len(chosen)} mutants killed')
    return 0 if killed == len(chosen) else 1


if __name__ == '__main__':
    sys.exit(main())
