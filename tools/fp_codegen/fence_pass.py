#!/usr/bin/env python3
"""fence_pass.py IN.cpp OUT.cpp: candidate deterministic form of attempt v28's hand edit (own-disc reproducibility study).

XenonRecomp emits the PowerPC barriers lwsync / sync / eieio as comments only. v28 inserted, after each such comment
line, one C++ fence with the same indentation (lwsync -> acq_rel, sync -> seq_cst, eieio -> release) carrying the marker
"// attempt v28: PowerPC barrier as a fence". This pass does exactly that and nothing else; every other line is copied.
Never overwrites OUT. Prints the number of fences inserted. Contains no game code.
"""
import re
import sys
from pathlib import Path

BARRIER = re.compile(r'^(\s*)// (lwsync|sync|eieio)\s*$')
ORDER = {'lwsync': '__ATOMIC_ACQ_REL', 'sync': '__ATOMIC_SEQ_CST', 'eieio': '__ATOMIC_RELEASE'}
MARK = ' // attempt v28: PowerPC barrier as a fence'


def rewrite(lines):
    out, n = [], 0
    for line in lines:
        out.append(line)
        m = BARRIER.match(line.rstrip('\n'))
        if m:
            out.append(f'{m.group(1)}__atomic_thread_fence({ORDER[m.group(2)]});{MARK}\n')
            n += 1
    return out, n


def main():
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    if dst.exists():
        raise SystemExit(f'{dst} exists')
    out, n = rewrite(src.read_text(errors='strict').splitlines(keepends=True))
    dst.write_text(''.join(out), encoding='utf-8', newline='\n')
    print(f'{src.name}: {n} fences')


if __name__ == '__main__':
    main()
