#!/usr/bin/env python3
"""gpr_helpers_v243_pass.py IN.cpp OUT.cpp: deterministic form of attempt v243's hand edit of the GPR save/restore helpers.

XenonRecomp emits the 36 register save/restore helpers __imp____savegprlr_N / __imp____restgprlr_N (N = 14..31) as
ordinary generated bodies. v243 made each one try its block-level fast path first (gpr_helpers_v243.h, runtime code of
that attempt): right after the body's `\\tPPC_FUNC_PROLOGUE();` line it inserted
    \\tif (recomp_v243::save<N>(ctx, base)) return; // v243        (restore<N> for the restgprlr helpers)
and, after the chunk's first line, an empty line, `#include "gpr_helpers_v243.h" // v243` and another empty line.
This pass does exactly that. A chunk without any helper definition is returned unchanged; a chunk with some but not all
36, or a helper whose first body line is not the prologue, or one already rewritten, is refused. Parameter-free: the
helper names are XenonRecomp's naming convention, not game data. Contains no game code.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import textpass  # noqa: E402

HELPER = re.compile(r'^PPC_FUNC_IMPL\(__imp____(save|rest)gprlr_(\d+)\)\s*\{\s*$')
PROLOGUE = '\tPPC_FUNC_PROLOGUE();\n'
INCLUDE_BLOCK = ['\n', '#include "gpr_helpers_v243.h" // v243\n', '\n']
FIRST = '#include "ppc_recomp_shared.h"\n'


def apply(text: str) -> tuple[str, int]:
    lines = text.splitlines(keepends=True)
    out, done = [], set()
    for k, line in enumerate(lines):
        out.append(line)
        m = HELPER.match(line)
        if not m:
            continue
        kind, n = m.group(1), int(m.group(2))
        if not 14 <= n <= 31 or (kind, n) in done:
            raise SystemExit(f'unexpected helper {line.strip()}')
        if k + 1 >= len(lines) or lines[k + 1] != PROLOGUE:
            raise SystemExit(f'{line.strip()}: first body line is not the prologue')
        if k + 2 < len(lines) and 'recomp_v243::' in lines[k + 2]:
            raise SystemExit(f'{line.strip()}: already rewritten')
        done.add((kind, n))
    if not done:
        return text, 0
    if len(done) != 36:
        raise SystemExit(f'expected the 36 GPR helpers, found {len(done)}')
    if not lines or lines[0] != FIRST:
        raise SystemExit('unexpected first line')
    # second walk: insert after each helper's prologue (indices shift, so rebuild from the original lines)
    res, pending = [lines[0]] + INCLUDE_BLOCK, None
    for line in lines[1:]:
        res.append(line)
        if pending is not None and line == PROLOGUE:
            fn = 'save' if pending[0] == 'save' else 'restore'
            res.append(f'\tif (recomp_v243::{fn}<{pending[1]}>(ctx, base)) return; // v243\n')
            pending = None
            continue
        m = HELPER.match(line)
        pending = (m.group(1), int(m.group(2))) if m else None
    return ''.join(res), len(done)


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    text, n = apply(textpass.read_text(src))
    textpass.write_new(dst, text)
    print(f'{src.name}: {n} helpers')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
