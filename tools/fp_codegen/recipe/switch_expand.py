#!/usr/bin/env python3
"""switch_expand.py IN.cpp OUT.cpp RECORDS.json KEY: replay the corpus's post-generation switch expansions (design gap G4).

The validated corpus was made from XenonRecomp output whose explicit switch metadata lacked some tables; a later text
repair expanded those dispatches in place. This pass is that repair as a deterministic, parameter-driven function. Per
record of RECORDS.json["chunks"][KEY] (private, game-derived; written by switch_recover.py records):
    {"function": F, "bctr": B, "r": R, "labels": [X0, X1, ...], "form": FORM, "mode": MODE, "drop": [D0, D1]?}
in sub_F at the bctr at B (addresses from the generator's instruction comments, textpass.instruction_lines):
  mode "expand":   the generator's `\\tPPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);` + `\\treturn;` after the bctr comment become a
                   switch on ctx.rR.u32 with case i -> Xi in FORM, and `loc_X:` is inserted before the instruction comment
                   of every in-function target that has no label yet (the generator's own labels stay; unlike a regeneration
                   with the table, no CSR/flush-mode state is reset at the new labels: that is what the corpus did);
  mode "reformat": the generator's live-target switch (patch 0014 lowering) with exactly these targets is re-emitted in FORM;
  "drop":          the generator's translation of the inline jump-table words [D0, D1) after the dispatch (data decoded as
                   instructions) is removed, from D0's instruction comment to the line before D1's label/comment.
FORM "live" = patch 0014's text (a target outside the function: `// ERROR: 0xX` + `return;`, as XenonRecomp prints it),
"compact" = `case i: if (ctx.ctr.u32 == 0xXU) goto loc_X; break;` lines, then `}`, `PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);`
and `return;`; "compact-default" adds `\\tdefault: break;` before the brace. Both forms keep the live CTR check: a target
that differs from the recorded case falls back to the indirect call. Every other line is copied unchanged. Contains no
game code. Exit 1 when a record does not apply exactly.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import textpass  # noqa: E402
from g4text import positions  # noqa: E402

BCTR = '\t// bctr \n'
INDIRECT = ['\tPPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);\n', '\treturn;\n']
FORMS = ('live', 'compact', 'compact-default')


def live_block(sel: int, labels: list[int], inside) -> list[str]:
    out = [f'\tswitch (ctx.r{sel}.u32) {{\n']
    for i, x in enumerate(labels):
        out.append(f'\tcase {i}:\n')
        if inside(x):
            out += [f'\t\tif (ctx.ctr.u32 != 0x{x:X}U) {{\n', '\t\t\tPPC_CALL_INDIRECT_FUNC(ctx.ctr.u32); return;\n',
                    '\t\t}\n', f'\t\tgoto loc_{x:X};\n']
        else:
            out += [f'\t\t// ERROR: 0x{x:X}\n', '\t\treturn;\n']
    return out + ['\tdefault:\n', '\t\tPPC_CALL_INDIRECT_FUNC(ctx.ctr.u32); return;\n', '\t}\n']


def compact_block(sel: int, labels: list[int], default_break: bool) -> list[str]:
    out = [f'\tswitch (ctx.r{sel}.u32) {{\n']
    out += [f'\tcase {i}: if (ctx.ctr.u32 == 0x{x:X}U) goto loc_{x:X}; break;\n' for i, x in enumerate(labels)]
    return out + (['\tdefault: break;\n'] if default_break else []) + ['\t}\n'] + INDIRECT


def block(form: str, sel: int, labels: list[int], inside) -> list[str]:
    if form == 'live':
        return live_block(sel, labels, inside)
    if form in ('compact', 'compact-default'):
        if not all(inside(x) for x in labels):
            raise SystemExit('a compact switch needs every target inside the function')
        return compact_block(sel, labels, form == 'compact-default')
    raise SystemExit(f'unknown switch form {form}')


def apply(text: str, records: list[dict]) -> tuple[str, dict]:
    lines = text.splitlines(keepends=True)
    replace: dict[int, tuple[int, list[str]]] = {}   # first line index -> (end index exclusive, new lines)
    before: dict[int, list[str]] = {}                # line index -> lines inserted before it
    stats = {'expanded': 0, 'reformatted': 0, 'labels_added': 0, 'labels_present': 0, 'dropped_words': 0}
    for rec in records:
        name = f'{int(rec["function"], 16):08X}'
        i, j = textpass.find_function(lines, name)
        pos = positions(lines, i, j, int(name, 16))
        site, sel = int(rec['bctr'], 16), int(rec['r'])
        labels = [int(x, 16) for x in rec['labels']]
        if rec['form'] not in FORMS or not labels or not 0 <= sel <= 31:
            raise SystemExit(f'sub_{name}: bad record at {site:08X}')
        k = pos.get(site)
        if k is None or lines[k] != BCTR:
            raise SystemExit(f'sub_{name}: no bctr at {site:08X}')
        inside = lambda x: x in pos  # noqa: E731
        new = block(rec['form'], sel, labels, inside)
        if rec['mode'] == 'expand':
            if lines[k + 1:k + 3] != INDIRECT:
                raise SystemExit(f'sub_{name}: no generator indirect dispatch at {site:08X}')
            end = k + 3
            stats['expanded'] += 1
        elif rec['mode'] == 'reformat':
            old = live_block(sel, labels, inside)
            if lines[k + 1:k + 1 + len(old)] != old:
                raise SystemExit(f'sub_{name}: the generator switch at {site:08X} differs from the recorded targets')
            end = k + 1 + len(old)
            stats['reformatted'] += 1
        else:
            raise SystemExit(f'sub_{name}: unknown mode {rec["mode"]}')
        if any(a <= k + 1 < b or k + 1 <= a < end for a, (b, _) in replace.items()):
            raise SystemExit(f'sub_{name}: overlapping records at {site:08X}')
        replace[k + 1] = (end, new)
        if rec.get('drop'):
            d0, d1 = (int(x, 16) for x in rec['drop'])
            a, b = pos.get(d0), pos.get(d1)
            if a is None or b is None or a != end or not d0 < d1:
                raise SystemExit(f'sub_{name}: inline data [{d0:08X}, {d1:08X}) does not follow the dispatch')
            if lines[b - 1] == f'loc_{d1:X}:\n':
                b -= 1
            if any(textpass.LABEL.fullmatch(x) for x in lines[a:b]):
                raise SystemExit(f'sub_{name}: a label inside the inline data [{d0:08X}, {d1:08X})')
            replace[k + 1] = (b, new)
            stats['dropped_words'] += (d1 - d0) // 4
        for x in sorted(set(labels)):
            t = pos.get(x)
            if t is None:
                continue  # outside the function: the live form prints the generator's ERROR case
            label = f'loc_{x:X}:\n'
            if lines[t - 1] == label or label in before.get(t, []):
                stats['labels_present'] += 1
                continue
            before.setdefault(t, []).append(label)
            stats['labels_added'] += 1
    out, idx = [], 0
    while idx < len(lines):
        out += before.get(idx, [])
        if idx in replace:
            end, new = replace[idx]
            if any(before.get(q) for q in range(idx + 1, end)):
                raise SystemExit('a target label falls inside a replaced or dropped range')
            out += new
            idx = end
            continue
        out.append(lines[idx])
        idx += 1
    return ''.join(out), stats


def main() -> int:
    if len(sys.argv) != 5:
        print(__doc__)
        return 2
    src, dst, params, key = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4]
    records = json.loads(params.read_text())['chunks'][key]
    text, stats = apply(textpass.read_text(src), records)
    textpass.write_new(dst, text)
    print(f'{src.name}: {stats}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
