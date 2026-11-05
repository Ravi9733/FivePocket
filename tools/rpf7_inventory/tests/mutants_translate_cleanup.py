#!/usr/bin/env python3
"""Mutant check for the worker-group cleanup of xenos_translate_batch.py: each mutant is the tool with one rule of
_terminate_worker_group changed; the cleanup tests of test_xenos_translate_batch.py must fail on it. First a control.
The tool is changed in place for each mutant and restored afterwards (the tests import it by name); exits 1 if the
control fails, a mutant survives or no longer applies."""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL = HERE.parent / 'xenos_translate_batch.py'
MUTANTS = [
    ('a denied group is an error at once (no second look)', "                if time.monotonic() >= deadline:\n                    raise\n                time.sleep(0.02)\n", "                raise\n"),
    ('a group that stays denied is accepted when the wait is over', "                if time.monotonic() >= deadline:\n                    raise\n", "                if time.monotonic() >= deadline:\n                    return\n"),
    ('the wait never ends', "                if time.monotonic() >= deadline:\n                    raise\n", ""),
    ('the checks follow each other without a pause', "                time.sleep(0.02)\n", ""),
    ('a denied group with a live leader is accepted', "        if process.poll() is None:\n            raise\n        deadline", "        process.poll()\n        deadline"),
    ('the leader is not reaped before the second look', "        if process.poll() is None:\n            raise\n        deadline", "        deadline"),
    ('the wait is ten times as long', "ZOMBIE_GROUP_WAIT_SECONDS = 2.0", "ZOMBIE_GROUP_WAIT_SECONDS = 20.0"),
    ('a group that is gone at the second look is an error', "                return\n            except ProcessLookupError:\n                return\n", "                return\n            except ProcessLookupError:\n                raise\n"),
    ('a missing group at the first look is an error', "    except ProcessLookupError:\n        pass\n    except PermissionError:", "    except PermissionError:"),
]


def run():
    return subprocess.run([sys.executable, '-B', '-m', 'unittest', '-k', 'cleanup', 'test_xenos_translate_batch'], cwd=HERE, capture_output=True, text=True)


def main() -> int:
    source = TOOL.read_text()
    killed = 0
    try:
        if run().returncode != 0:
            print('CONTROL FAILED: the cleanup tests do not pass on the tool as it is')
            return 1
        print('control: the cleanup tests pass on the tool as it is', flush=True)
        for index, (what, old, new) in enumerate(MUTANTS, 1):
            if source.count(old) != 1:
                print(f'mutant {index:3d} DOES NOT APPLY ({source.count(old)} matches): {what}')
                continue
            TOOL.write_text(source.replace(old, new))
            r = run()
            if r.returncode == 0:
                print(f'mutant {index:3d} SURVIVED: {what}')
            else:
                killed += 1
                print(f'mutant {index:3d} killed: {what}')
    finally:
        TOOL.write_text(source)
    print(f'{killed} of {len(MUTANTS)} mutants killed')
    return 0 if killed == len(MUTANTS) else 1


if __name__ == '__main__':
    sys.exit(main())
