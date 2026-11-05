#!/usr/bin/env python3
"""Run the release checks that need no game files: names, secrets, personal details, and the checks' own tests.

    run_checks.py [ROOT] [--extra PRIVATE_LITERALS]

Exit 0 when every check passes, 1 otherwise. The taint scan needs your own game files and is run separately
(taint_scan.py; its tests here prove that it finds a planted run and reports a retail file it could not read).
"""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    extra = []
    if '--extra' in argv:
        i = argv.index('--extra')
        extra = ['--extra', str(Path(argv[i + 1]).resolve())]  # the checks run from this folder
        argv = argv[:i] + argv[i + 2:]
    root = str(Path(argv[0]).resolve()) if argv else str(HERE.parents[1])
    steps = [
        [sys.executable, str(HERE / 'scan_names.py'), root],
        [sys.executable, str(HERE / 'scan_secrets.py'), root],
        [sys.executable, str(HERE / 'scan_owner.py'), root, *extra],
        [sys.executable, '-m', 'unittest', '-q', 'test_release_checks'],
    ]
    failed = 0
    for cmd in steps:
        out = subprocess.run(cmd, cwd=HERE, capture_output=True, text=True)
        lines = (out.stdout + out.stderr).strip().splitlines()
        if out.returncode:
            failed += 1
            print('\n'.join(lines[-40:]))
        print(f'{"PASS" if out.returncode == 0 else "FAIL"}: {" ".join(Path(c).name for c in cmd[1:3])}')
    print(f'RELEASE CHECKS {"PASS" if not failed else "FAIL"} failed={failed}')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
