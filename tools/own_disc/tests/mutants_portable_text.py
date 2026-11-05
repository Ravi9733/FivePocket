#!/usr/bin/env python3
"""Mutant check for test_portable_text.py: a copy of the tool folders in which one text write at a time loses its
line ending or its encoding (one mutant per file that writes text, alternating the two), and three mutants of the
check itself; the test must fail on each. First a control. Exits 1 if the control fails or a mutant survives. The
tools are not touched."""
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
TEST = HERE / 'test_portable_text.py'
FOLDERS = ('tools/own_disc', 'tools/iter', 'tools/fp_codegen', 'tools/rpf7_inventory', 'tools/host_guest_trace', 'tools/vulkan_probe')
ENDING = re.compile(r''',\s*newline=(?:'\\n'|"\\n")''')
ENCODING = re.compile(r''',\s*encoding=(?:'utf-8'|"utf-8")(?=,\s*newline=)''')
CHECK = [   # (what, old, new) in the test itself
    ('a write without a line ending passes', "            out.append((node.lineno, 'no newline=\"\\\\n\"'))\n", "            pass\n"),
    ('a write without an encoding passes', "            out.append((node.lineno, 'no encoding=\"utf-8\"'))\n", "            pass\n"),
    ('write_text is not looked at', "    if isinstance(f, ast.Attribute) and f.attr == 'write_text':\n        return keywords\n", ""),
    ('imported files are not followed', "                    if module.is_file():\n                        todo.append(module)\n                        break\n", "                    if module.is_file():\n                        break\n"),
    ('folders stand for nothing', "            found |= {p for p in path.rglob('*.py') if 'tests' not in p.relative_to(root).parts and '__pycache__' not in p.parts}\n", "            pass\n"),
]


def run(root: Path, test: Path = TEST):
    return subprocess.run([sys.executable, '-B', str(test)], capture_output=True, text=True, env=dict(os.environ, PORTABLE_TEXT_ROOT=str(root)))


def main() -> int:
    killed = total = 0
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp).resolve() / 'copy'
        for folder in FOLDERS:
            shutil.copytree(REPO / folder, root / folder, ignore=shutil.ignore_patterns('__pycache__', 'tests', '*.cpp', '*.h', '*.o', '*.spv', '*.inc'))
        if run(root).returncode != 0:
            print('CONTROL FAILED: the test does not pass on an unchanged copy'); return 1
        print('control: the test passes on an unchanged copy of the tools', flush=True)
        index = 0
        for path in sorted(root.rglob('*.py')):
            text = path.read_text(encoding='utf-8')
            pattern = ENDING if index % 2 == 0 else ENCODING
            if not pattern.search(text):
                pattern = ENDING
                if not pattern.search(text): continue
            index += 1; total += 1
            what = 'line ending' if pattern is ENDING else 'encoding'
            path.write_text(pattern.sub('', text, count=1), encoding='utf-8', newline='\n')
            result = run(root)
            path.write_text(text, encoding='utf-8', newline='\n')
            name = path.relative_to(root)
            # only the files the stages run are in the test's closure: a file outside it is no mutant of this check
            if result.returncode == 0 and str(name) not in (result.stdout + result.stderr):
                inside = subprocess.run([sys.executable, '-B', '-c', 'import sys; sys.path.insert(0, sys.argv[1]); import test_portable_text as t; '
                                         'print(t.ROOT / sys.argv[2] in t.closure(t.ROOT, t.python_files(t.ROOT, t.TOOLS)))', str(HERE), str(name)],
                                        capture_output=True, text=True, env=dict(os.environ, PORTABLE_TEXT_ROOT=str(root))).stdout.strip()
                if inside != 'True':
                    total -= 1; index -= 1; continue
                print(f'mutant SURVIVED: {name} loses a {what}')
            else:
                killed += 1
        print(f'{killed} of {total} tool mutants killed (one text write per file)', flush=True)
        source = TEST.read_text(encoding='utf-8')
        copy = Path(tmp) / 'tests' / 'own_disc' / 'tests' / 'test_portable_text.py'      # the same depth under a root as the real one
        copy.parent.mkdir(parents=True)
        for what, old, new in CHECK:
            total += 1
            if source.count(old) != 1:
                print(f'check mutant DOES NOT APPLY ({source.count(old)} matches): {what}'); continue
            copy.write_text(source.replace(old, new), encoding='utf-8', newline='\n')
            # a weakened check must be caught by the check's own self-tests (run on the real tools)
            result = subprocess.run([sys.executable, '-B', str(copy)], capture_output=True, text=True, env=dict(os.environ, PORTABLE_TEXT_ROOT=str(REPO)))
            if result.returncode == 0: print(f'check mutant SURVIVED: {what}')
            else: killed += 1; print(f'check mutant killed: {what}')
    print(f'{killed} of {total} mutants killed')
    return 0 if killed == total else 1


if __name__ == '__main__':
    sys.exit(main())
