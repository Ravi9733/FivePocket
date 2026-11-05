#!/usr/bin/env python3
"""fence_duplicate.py IN.cpp OUT.cpp DUPS.json KEY: replay of attempt v28's one hand artifact, a duplicated fence line.

fence_pass.py inserts one `__atomic_thread_fence(...); // attempt v28: PowerPC barrier as a fence` line after every
PowerPC barrier comment. In one place v28's hand edit has the same fence line twice in a row (semantically identical to
one fence). DUPS.json["chunks"][KEY] (private: it names guest functions) lists [{"function": F, "ordinal": K,
"copies": C}]: the K-th (1-based) fence_pass line inside the generated function F appears C times in total (C >= 2).
Refused when F or its K-th fence line is missing, or when that line already repeats. Every other line is copied
unchanged. Contains no game code.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import textpass  # noqa: E402

MARK = ' // attempt v28: PowerPC barrier as a fence\n'   # fence_pass.MARK + newline


def fence_lines(lines: list[str], i: int, j: int) -> list[int]:
    return [k for k in range(i + 1, j) if lines[k].endswith(MARK) and lines[k].lstrip().startswith('__atomic_thread_fence(')]


def apply(text: str, records: list[dict]) -> str:
    lines = text.splitlines(keepends=True)
    extra: dict[int, int] = {}
    for r in records:
        i, j = textpass.find_function(lines, r['function'])
        fences = fence_lines(lines, i, j)
        if not 1 <= r['ordinal'] <= len(fences):
            raise SystemExit(f'{r["function"]}: fence {r["ordinal"]} of {len(fences)}')
        k = fences[r['ordinal'] - 1]
        if lines[k + 1] == lines[k]:
            raise SystemExit(f'{r["function"]}: fence {r["ordinal"]} already repeats')
        if int(r['copies']) < 2:
            raise SystemExit('copies must be >= 2')
        extra[k] = int(r['copies']) - 1
    out = []
    for k, line in enumerate(lines):
        out.append(line)
        out += [line] * extra.get(k, 0)
    return ''.join(out)


def find(text: str) -> list[dict]:
    """the records that describe every repeated fence line of a chunk (used to record the hand artifact)"""
    lines = text.splitlines(keepends=True)
    recs = []
    for i, j, name in textpass.functions(lines):
        fences = fence_lines(lines, i, j)
        ordinal, n = 0, 0
        while n < len(fences):
            k = fences[n]
            run = 1
            while n + run < len(fences) and fences[n + run] == k + run and lines[k + run] == lines[k]:
                run += 1
            ordinal += 1
            if run > 1:
                recs.append(dict(function=name, ordinal=ordinal, copies=run))
            n += run
    return recs


def main() -> int:
    if len(sys.argv) != 5:
        print(__doc__)
        return 2
    src, dst, params, key = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4]
    records = json.loads(params.read_text())['chunks'][key]
    textpass.write_new(dst, apply(textpass.read_text(src), records))
    print(f'{src.name}: {len(records)} repeated fences')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
