#!/usr/bin/env python3
"""case_blocks.py: the corpus's "original short case" repairs as a generator-derived pass (design gap G4).

  case_blocks.py IN.cpp OUT.cpp RECORDS.json KEY      apply the records of RECORDS.json["chunks"][KEY]
  case_blocks.py derive GEN_DIR CODE_DIR OUT.json [--chunks N,M,... | @FILE]
                                                      recover the records from repaired code (switch_recover.py style)

Some functions end in a jump-table dispatch whose case targets are not function entries (the analyser cut the function
after the bctr; the table words and the case code became separate "functions"). The validated corpus repaired them by
inserting, right after the bctr comment and before the generator's indirect call, one guarded block per such target X:
    \\t// Original <title> case at 0xX; retain live target validation.
    \\tif ((ctx.rR.u32 == i0 || ctx.rR.u32 == i1 ...) && ctx.ctr.u32 == 0xXU) {
    \\t\\t<locals the copied lines use: PPCContext env{}; PPCRegister temp{}; PPCVRegister vTemp{}; uint32_t ea{};>
    \\t\\t<the generator's own translation of the COUNT instructions at X..., one more tab of indentation>
    \\t}
The block body is not stored: it is copied from the generator output of the same chunk (the function whose instruction
comments contain X; textpass.instruction_lines), so a record holds addresses only:
    {"function": F, "bctr": B, "blocks": [{"title": "short", "target": X, "r": R, "cases": [i0, i1, ...], "count": C}]}
A copied range must end with an unconditional blr/b/bctr translation and contain no label. Exit 1 when a record does not
apply exactly. Contains no game code or game-derived data.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import textpass  # noqa: E402
from g4text import Index, positions  # noqa: E402

BCTR = '\t// bctr \n'
INDIRECT = ['\tPPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);\n', '\treturn;\n']
LOCALS = (('env', '\tPPCContext env{};\n'), ('temp', '\tPPCRegister temp{};\n'), ('vTemp', '\tPPCVRegister vTemp{};\n'),
          ('ea', '\tuint32_t ea{};\n'))
TITLE = re.compile(r'\t// Original ([a-z-]+) case at 0x([0-9A-F]+); retain live target validation\.\n')
COND = re.compile(r'\tif \(\(((?:ctx\.r\d+\.u32 == \d+(?: \|\| )?)+)\) && ctx\.ctr\.u32 == 0x([0-9A-F]+)U\) \{\n')
INSN = re.compile(r'\t// (?!ERROR )')


def copy_lines(idx: Index, x: int, count: int) -> list[str]:
    name, pos = idx.find(x)
    i, j = idx.fn[name]
    addrs = [x + 4 * n for n in range(count)]
    if any(a not in pos for a in addrs):
        raise SystemExit(f'{x:08X}: {count} instructions are not all in sub_{name}')
    start = pos[x]
    nxt = pos.get(x + 4 * count, j)
    end = nxt
    while end > start and textpass.LABEL.fullmatch(idx.lines[end - 1]):
        end -= 1  # a label of the next instruction stays with it
    body = idx.lines[start:end]
    if any(textpass.LABEL.fullmatch(x_) for x_ in body):
        raise SystemExit(f'{x:08X}: a label inside the copied range')
    last = idx.lines[pos[addrs[-1]]]
    if not textpass.UNCONDITIONAL.match(last):
        raise SystemExit(f'{x:08X}: the copied range does not end with an unconditional transfer')
    return body


def render(idx: Index, b: dict) -> list[str]:
    x, r = int(b['target'], 16), int(b['r'])
    body = copy_lines(idx, x, int(b['count']))
    cond = ' || '.join(f'ctx.r{r}.u32 == {int(i)}' for i in b['cases'])
    out = [f'\t// Original {b["title"]} case at 0x{x:X}; retain live target validation.\n',
           f'\tif (({cond}) && ctx.ctr.u32 == 0x{x:X}U) {{\n']
    for name, decl in LOCALS:
        if any(re.search(rf'\b{name}\b', ln) for ln in body):
            out.append('\t' + decl)
    out += ['\t' + ln for ln in body]
    return out + ['\t}\n']


def apply(text: str, records: list[dict]) -> tuple[str, dict]:
    lines = text.splitlines(keepends=True)
    idx = Index(lines)
    after: dict[int, list[str]] = {}
    stats = {'sites': 0, 'blocks': 0}
    for rec in records:
        name = f'{int(rec["function"], 16):08X}'
        if name not in idx.fn:
            raise SystemExit(f'sub_{name}: not in this chunk')
        pos = idx.positions(name)
        site = int(rec['bctr'], 16)
        k = pos.get(site)
        if k is None or lines[k] != BCTR or lines[k + 1:k + 3] != INDIRECT or k in after:
            raise SystemExit(f'sub_{name}: no free generator indirect dispatch at {site:08X}')
        after[k] = [ln for b in rec['blocks'] for ln in render(idx, b)]
        stats['sites'] += 1
        stats['blocks'] += len(rec['blocks'])
    out = []
    for n, line in enumerate(lines):
        out.append(line)
        out += after.get(n, [])
    return ''.join(out), stats


# ------------------------------------------------------------------------------------------------ derive

def parse_blocks(c: list[str], k: int) -> tuple[list[dict], int] | None:
    """the standard blocks right after the bctr comment at c[k]; None if anything else is there"""
    blocks, i = [], k + 1
    while (t := TITLE.fullmatch(c[i])):
        m = COND.fullmatch(c[i + 1])
        if not m or m.group(2) != t.group(2):
            return None
        conds = re.findall(r'ctx\.r(\d+)\.u32 == (\d+)', m.group(1))
        regs = {int(a) for a, _ in conds}
        if len(regs) != 1:
            return None
        j = i + 2
        while c[j] != '\t}\n':
            if not c[j].startswith('\t\t'):
                return None
            j += 1
        body = [ln[1:] for ln in c[i + 2:j]]
        decl = [d for _, d in LOCALS]
        while body and body[0] in decl:
            body = body[1:]
        count = sum(1 for ln in body if INSN.match(ln))
        blocks.append({'title': t.group(1), 'target': t.group(2), 'r': regs.pop(), 'cases': [int(b) for _, b in conds],
                       'count': count})
        i = j + 1
    if not blocks or c[i:i + 2] != INDIRECT:
        return None
    return blocks, i


def derive(a) -> int:
    from g4text import segments, bctr_lines  # noqa: E402
    gen, code = Path(a.gen), Path(a.code)
    spec = Path(a.chunks[1:]).read_text() if a.chunks and a.chunks.startswith('@') else (a.chunks or '')
    chunks = [int(x) for x in re.split(r'[\s,]+', spec) if x]
    out, n_sites, n_blocks, unparsed = {}, 0, 0, []
    for n in chunks:
        gt, ct = textpass.read_text(gen / f'ppc_recomp.{n}.cpp'), textpass.read_text(code / f'ppc_recomp.{n}.cpp')
        g, c = gt.splitlines(keepends=True), ct.splitlines(keepends=True)
        gs, cs = segments(g), segments(c)
        recs = []
        for name, (gi, gj) in gs.items():
            ci, cj = cs[name]
            if g[gi:gj] == c[ci:cj]:
                continue
            gb, cb = bctr_lines(g, gi, gj), bctr_lines(c, ci, cj)
            if len(gb) != len(cb):
                continue
            pos = None
            for kg, kc in zip(gb, cb):
                if c[kc + 1:kc + 3] == INDIRECT or c[kc + 1].startswith('\tswitch (') or g[kg + 1:kg + 3] != INDIRECT:
                    continue
                parsed = parse_blocks(c, kc)
                if parsed is None:
                    unparsed.append(f'{n}:{name}')
                    continue
                if pos is None:
                    pos = positions(g, gi, gj, int(name, 16))
                site = {v: k for k, v in pos.items()}[kg]
                recs.append({'function': name, 'bctr': f'{site:08X}', 'blocks': parsed[0]})
                n_sites += 1
                n_blocks += len(parsed[0])
        if recs:
            out[str(n)] = recs
    if Path(a.out).exists():
        raise SystemExit(f'{a.out} exists')
    Path(a.out).write_text(json.dumps({'schema': 'gta5-case-blocks/1', 'note': 'private: guest addresses recovered from '
                                       'repaired generated code (case_blocks.py derive)', 'chunks': out,
                                       'unparsed_sites': unparsed}, indent=1) + '\n', encoding='utf-8', newline='\n')
    print(f'{n_sites} sites, {n_blocks} blocks in {len(out)} chunks; {len(unparsed)} non-standard sites left out')
    return 0


def main() -> int:
    if len(sys.argv) > 1 and sys.argv[1] == 'derive':
        ap = argparse.ArgumentParser()
        ap.add_argument('cmd'); ap.add_argument('gen'); ap.add_argument('code'); ap.add_argument('out')
        ap.add_argument('--chunks')
        return derive(ap.parse_args())
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
