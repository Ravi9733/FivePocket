#!/usr/bin/env python3
"""Mutant check for the repository question of xenos_preflight_batch.py (Git is asked about a private path only where a
repository holds the root): each mutant is the tool with one rule changed; test_xenos_preflight_batch.py must fail on
it. First a control. The tool is changed in place for each mutant and restored afterwards (the tests import it by
name); exits 1 if the control fails, a mutant survives or no longer applies."""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL = HERE.parent / 'xenos_preflight_batch.py'
RULE = '    return any((parent / ".git").exists() for parent in (root, *root.parents))\n'
INPUT = '    if _in_repository(repository):\n        if _git_says(repository, ["ls-files", "--error-unmatch", "--", str(repository_relative)]):\n            raise XenosPreflightError("tracked inputs are not accepted")'
REPORT = '    if _in_repository(repository):\n        if _git_says(repository, ["ls-files", "--error-unmatch", "--", str(repository_relative)]):\n            raise XenosPreflightError("tracked reports are not accepted")'
MUTANTS = [
    ('Git is never asked', RULE, '    return False\n'),
    ('Git is always asked', RULE, '    return True\n'),
    ('only the root itself is looked at, not the folders above', RULE, '    return (root / ".git").exists()\n'),
    ('only the folders above are looked at, not the root', RULE, '    return any((parent / ".git").exists() for parent in root.parents)\n'),
    ('an input in a repository is not asked about', INPUT, INPUT.replace('if _in_repository(repository):', 'if False:')),
    ('an input outside a repository is asked about', INPUT, INPUT.replace('if _in_repository(repository):', 'if True:')),
    ('a report in a repository is not asked about', REPORT, REPORT.replace('if _in_repository(repository):', 'if False:')),
    ('a report outside a repository is asked about', REPORT, REPORT.replace('if _in_repository(repository):', 'if True:')),
]


def run():
    return subprocess.run([sys.executable, '-B', '-m', 'unittest', 'test_xenos_preflight_batch'], cwd=HERE, capture_output=True, text=True)


def main() -> int:
    source = TOOL.read_text(encoding='utf-8')
    killed = 0
    try:
        if run().returncode != 0:
            print('CONTROL FAILED: the tests do not pass on the tool as it is')
            return 1
        print('control: the tests pass on the tool as it is', flush=True)
        for index, (what, old, new) in enumerate(MUTANTS, 1):
            if source.count(old) != 1:
                print(f'mutant {index:3d} DOES NOT APPLY ({source.count(old)} matches): {what}')
                continue
            TOOL.write_text(source.replace(old, new), encoding='utf-8', newline='\n')
            r = run()
            if r.returncode == 0:
                print(f'mutant {index:3d} SURVIVED: {what}')
            else:
                killed += 1
                print(f'mutant {index:3d} killed: {what}')
    finally:
        TOOL.write_text(source, encoding='utf-8', newline='\n')
    print(f'{killed} of {len(MUTANTS)} mutants killed')
    return 0 if killed == len(MUTANTS) else 1


if __name__ == '__main__':
    sys.exit(main())
