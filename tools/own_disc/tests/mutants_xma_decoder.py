#!/usr/bin/env python3
"""Mutant check for tools/host_guest_trace/build_xma_decoder.sh: copies of the script with one rule changed;
test_build_xma_decoder.py (beside this file) must fail on each. First a control. Exits 1 if the control fails, a
mutant survives or no longer applies. The script itself is not touched."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL = HERE.parents[1] / 'host_guest_trace' / 'build_xma_decoder.sh'
TESTS = HERE / 'test_build_xma_decoder.py'
MUTANTS = [
    ('git may look above the build folder', 'GIT_CEILING_DIRECTORIES=$(dirname "$xma_build")\nexport GIT_CEILING_DIRECTORIES\n', ''),
    ('the ceiling is set but not handed to the build', 'export GIT_CEILING_DIRECTORIES\n', ''),
    ('the ceiling is the build folder itself', 'GIT_CEILING_DIRECTORIES=$(dirname "$xma_build")', 'GIT_CEILING_DIRECTORIES=$xma_build'),
    ('a source at any revision is built', 'test "$(git -C "$xma_source" rev-parse HEAD)" = "$xma_revision"\n', ''),
]


def run(script):
    return subprocess.run([sys.executable, '-B', str(TESTS)], capture_output=True, text=True, env=dict(os.environ, BUILD_XMA_DECODER=str(script)))


def main() -> int:
    source = TOOL.read_text()
    killed = 0
    with tempfile.TemporaryDirectory() as tmp:
        copy = Path(tmp) / 'build_xma_decoder.sh'
        copy.write_text(source)
        if run(copy).returncode != 0:
            print('CONTROL FAILED: the tests do not pass on an unchanged copy')
            return 1
        print('control: the tests pass on an unchanged copy of the script', flush=True)
        for index, (what, old, new) in enumerate(MUTANTS, 1):
            if source.count(old) != 1:
                print(f'mutant {index:3d} DOES NOT APPLY ({source.count(old)} matches): {what}')
                continue
            copy.write_text(source.replace(old, new))
            if run(copy).returncode == 0:
                print(f'mutant {index:3d} SURVIVED: {what}')
            else:
                killed += 1
                print(f'mutant {index:3d} killed: {what}')
    print(f'{killed} of {len(MUTANTS)} mutants killed')
    return 0 if killed == len(MUTANTS) else 1


if __name__ == '__main__':
    sys.exit(main())
