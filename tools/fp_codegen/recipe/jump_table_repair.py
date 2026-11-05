#!/usr/bin/env python3
"""jump_table_repair.py IN.cpp OUT.cpp TABLES.json KEY: deterministic, parameter-driven form of attempt v101's hand repair.

For each table record of TABLES.json["chunks"][KEY] (private, game-derived; under local-analysis/):
    {"function": F, "site": S, "table": T, "entries": E, "targets": [X1, X2, ...], "tag": "v101"}   (hex addresses)
the generated function sub_F, whose inline jump table the generator left as an indirect dispatch at the bctr at S
(`\\t// bctr ` followed by `\\tPPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);`), gets
  1. the declaration `extern "C" bool recomp_jump_table_repair(uint32_t site, uint32_t target) noexcept;` on the line
     before its PPC_FUNC_IMPL header (once per function),
  2. right after the bctr comment, a local dispatch guarded by the runtime switch:
        // Jump-table repair (<tag>): table T, E entries, all inside sub_F.
        if (recomp_jump_table_repair(0xSU, ctx.ctr.u32)) { switch (ctx.ctr.u32) { case 0xXU: goto loc_X; ... default: break; } }
     (one case per target, in the recorded order; an unmatched target falls through to the original indirect call),
  3. a label `loc_X:` before the generator's instruction comment of every target X that has none yet.
Instruction addresses come from the generator's one comment per instruction (textpass.instruction_lines, with the label and
LR consistency checks); every target must be an instruction of sub_F. Every other line is copied unchanged. Contains no
game code. Exit 1 when a record does not apply exactly.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import textpass  # noqa: E402

DECL = 'extern "C" bool recomp_jump_table_repair(uint32_t site, uint32_t target) noexcept;\n'
BCTR = '\t// bctr \n'
INDIRECT = '\tPPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);\n'


def block(rec: dict) -> list[str]:
    f, s, t = (int(rec[k], 16) for k in ('function', 'site', 'table'))
    out = [f'\t// Jump-table repair ({rec.get("tag", "v101")}): table {t:08X}, {int(rec["entries"])} entries, all inside sub_{f:08X}.\n',
           f'\tif (recomp_jump_table_repair(0x{s:08X}U, ctx.ctr.u32)) {{\n',
           '\t\tswitch (ctx.ctr.u32) {\n']
    out += [f'\t\tcase 0x{x:08X}U: goto loc_{x:08X};\n' for x in (int(v, 16) for v in rec['targets'])]
    out += ['\t\tdefault: break;\n', '\t\t}\n', '\t}\n']
    return out


def apply(text: str, records: list[dict]) -> tuple[str, dict]:
    lines = text.splitlines(keepends=True)
    before: dict[int, list[str]] = {}   # line index -> lines inserted before it
    after: dict[int, list[str]] = {}    # line index -> lines inserted after it
    stats = {'tables': 0, 'labels_added': 0, 'labels_present': 0}
    declared = set()
    for rec in records:
        name = f'{int(rec["function"], 16):08X}'
        i, j = textpass.find_function(lines, name)
        pos = textpass.instruction_lines(lines, i, j, int(name, 16))
        site = int(rec['site'], 16)
        k = pos.get(site)
        if k is None or lines[k] != BCTR or k + 1 >= j or lines[k + 1] != INDIRECT:
            raise SystemExit(f'sub_{name}: no generator indirect dispatch at {site:08X}')
        if k in after:
            raise SystemExit(f'sub_{name}: two records for the site {site:08X}')
        if name not in declared:
            if i > 0 and lines[i - 1] == DECL:
                raise SystemExit(f'sub_{name}: already declared (repair applied twice?)')
            before.setdefault(i, []).append(DECL)
            declared.add(name)
        after[k] = block(rec)
        for x in (int(v, 16) for v in rec['targets']):
            t = pos.get(x)
            if t is None:
                raise SystemExit(f'sub_{name}: target {x:08X} is not an instruction of the function')
            label = f'loc_{x:08X}:\n'
            if lines[t - 1] == label or label in before.get(t, []):
                stats['labels_present'] += 1
                continue
            before.setdefault(t, []).append(label)
            stats['labels_added'] += 1
        stats['tables'] += 1
    out = []
    for idx, line in enumerate(lines):
        out += before.get(idx, [])
        out.append(line)
        out += after.get(idx, [])
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
