#!/usr/bin/env python3
"""Mutant check for the kept translator revisions: each mutant is a copy of tools/rpf7_inventory/history with one
thing changed; test_translator_history.py (beside this file) must fail on it. First a control on an unchanged copy.
Exits 1 if the control fails or a mutant survives. The folder itself is not touched."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
HISTORY = HERE.parents[1] / 'rpf7_inventory' / 'history'
TESTS = HERE / 'test_translator_history.py'
R = 'r20260910'


def flip(path: Path):
    data = bytearray(path.read_bytes())
    data[len(data) // 2] ^= 1
    path.write_bytes(bytes(data))


def sources(folder: Path, change):
    path = folder / R / 'SOURCES.json'
    table = json.loads(path.read_text())
    change(table)
    path.write_text(json.dumps(table, indent=1, sort_keys=True) + '\n')


MUTANTS = [
    ('one bit of the translator is changed', lambda f: flip(f / R / 'inline_shader_translation.py')),
    ('one bit of a sibling module is changed', lambda f: flip(f / R / 'scalar_constant.py')),
    ('a line is added to a file', lambda f: (f / R / 'raw_shader_analysis.py').write_bytes((f / R / 'raw_shader_analysis.py').read_bytes() + b'\n')),
    ('a module is added to the revision', lambda f: (f / R / 'extra_module.py').write_text('VALUE = 1\n')),
    ('a module is missing', lambda f: (f / R / 'unorm8_vertex_fetch.py').unlink()),
    ('another revision folder appears', lambda f: shutil.copytree(f / R, f / 'r20260911')),
    ('the list of sources names another hash', lambda f: sources(f, lambda t: t['files']['pixel_sample_dataflow.py'].update(sha256='0' * 64))),
    ('the list of sources leaves a file out', lambda f: sources(f, lambda t: t['files'].pop('scalar_mul_const.py'))),
    ('the list of sources names another revision', lambda f: sources(f, lambda t: t.update(revision='r1'))),
    ('the list of sources gives no commit', lambda f: sources(f, lambda t: t['files']['scalar_constant.py'].update(commit='unknown'))),
]


def run(folder: Path):
    env = dict(os.environ, TRANSLATOR_HISTORY=str(folder))
    return subprocess.run([sys.executable, '-B', str(TESTS)], capture_output=True, text=True, env=env)


def main() -> int:
    killed = 0
    with tempfile.TemporaryDirectory() as tmp:
        control = Path(tmp) / 'control'
        shutil.copytree(HISTORY, control, ignore=shutil.ignore_patterns('__pycache__'))
        if run(control).returncode != 0:
            print('CONTROL FAILED: the tests do not pass on an unchanged copy')
            return 1
        print('control: the tests pass on an unchanged copy of the folder', flush=True)
        for index, (what, change) in enumerate(MUTANTS, 1):
            folder = Path(tmp) / f'm{index}'
            shutil.copytree(HISTORY, folder, ignore=shutil.ignore_patterns('__pycache__'))
            change(folder)
            r = run(folder)
            failed = [line.split(' ... ')[0].strip() for line in r.stderr.splitlines() if line.startswith(('FAIL:', 'ERROR:'))]
            if r.returncode == 0:
                print(f'mutant {index:3d} SURVIVED: {what}')
            else:
                killed += 1
                print(f'mutant {index:3d} killed by {failed[0].split(" (")[0] if failed else "the test run"}: {what}')
    print(f'{killed} of {len(MUTANTS)} mutants killed')
    return 0 if killed == len(MUTANTS) else 1


if __name__ == '__main__':
    sys.exit(main())
