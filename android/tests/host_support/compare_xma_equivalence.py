#!/usr/bin/env python3
"""Cooperative-equivalence check for the XMA pool (multicore plan WP4).

compare_xma_equivalence.py PREVIOUS_BIN WP4_BIN STEPS SEEDS

Both binaries are android/tests/native_xma_equivalence_driver.cpp, built with libc++ hardening
against the previous pool and against the WP4 pool (cooperative build). The previous pool is the
reference:
- a clean previous run: the WP4 output must be byte-identical;
- a previous run that aborts on its frame-buffer overflow (a zero compressed length kicked again
  wrote past Context::compressed; libc++ hardening reports it): every complete line before the
  crash must be identical, and the WP4 run continues with its bounded failure.
Any other difference, or any WP4 failure, is a mismatch.
"""
import subprocess
import sys


def main() -> int:
    previous_bin, wp4_bin, steps, seeds = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
    identical = prefix = bad = visible = 0
    for seed in range(1, seeds + 1):
        old = subprocess.run([previous_bin, steps, str(seed)], capture_output=True, text=True)
        new = subprocess.run([wp4_bin, steps, str(seed)], capture_output=True, text=True)
        if new.returncode != 0:
            bad += 1
            print('seed', seed, 'WP4 run failed', new.returncode, new.stderr[-300:])
            continue
        if old.returncode == 0:
            if old.stdout == new.stdout:
                identical += 1
            else:
                bad += 1
                print('seed', seed, 'differs although the previous run was clean')
            continue
        if 'out-of-bounds access in std::array' not in old.stderr:
            bad += 1
            print('seed', seed, 'previous run failed unexpectedly', old.returncode, old.stderr[-200:])
            continue
        old_lines = old.stdout.splitlines()
        complete = old_lines[:-1] if old_lines and not old.stdout.endswith('\n') else old_lines
        # The previous process died inside one step, before printing it: drop trailing log lines.
        while complete and complete[-1].startswith('  '):
            complete.pop()
        new_lines = new.stdout.splitlines()
        if new_lines[:len(complete)] != complete:
            bad += 1
            print('seed', seed, 'prefix differs')
            continue
        crash = '\n'.join(new_lines[len(complete):len(complete) + 12])
        visible += 'exceeds its assembly buffer' in crash  # pump failures log only their first 8
        prefix += 1
    print(f'seeds={seeds} identical={identical} previous_ub_prefix_identical={prefix} '
          f'(bounded failure visible in {visible}) mismatches={bad}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
