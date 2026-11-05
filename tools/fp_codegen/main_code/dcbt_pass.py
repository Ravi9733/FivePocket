#!/usr/bin/env python3
"""dcbt_pass.py IN.cpp OUT.cpp [--variant switch|line|block] [--mutant r0-reg|swap-ab]: rewrite pass for one generated
chunk. After every "// dcbt rA,rB" / "// dcbtst rA,rB" comment (XenonRecomp emits these hints without host code) insert
PPC_MC_DCBT(EA); / PPC_MC_DCBTST(EA); with the Power ISA effective address EA = (rA|0) + rB (rA = r0 means the literal 0,
rB is always a register), as ctx.rX.u32 32-bit arithmetic (guest addresses are 32-bit). The chunk includes the variant's
header (mc_dcbt_switch.h: runtime modes; mc_dcbt_fixed.h: compiled-in line / block) right after ppc_recomp_shared.h.
Only comment lines are matched (the instruction text after '// '); every other line is copied unchanged. Mutants (for the
unit test): r0-reg treats rA = r0 as the register r0; swap-ab is harmless semantically (same sum) and must PASS the unit
test's EA check only where rA != r0. Prints the number of sites. Never overwrites OUT."""
import re, sys
from pathlib import Path

HINT = re.compile(r'^(\s*)// (dcbt|dcbtst) r(\d+),r(\d+)\s*$')
OTHER = re.compile(r'^\s*// (dcbt|dcbtst)\b')
INCLUDE = '#include "ppc_recomp_shared.h"'


def ea_expr(ra: int, rb: int, mutant: str = '') -> str:
    if ra == 0 and mutant != 'r0-reg':
        return f'ctx.r{rb}.u32'
    if mutant == 'swap-ab':
        return f'ctx.r{rb}.u32 + ctx.r{ra}.u32'
    return f'ctx.r{ra}.u32 + ctx.r{rb}.u32'


def rewrite_lines(lines, mutant=''):
    out, n = [], 0
    for line in lines:
        out.append(line)
        m = HINT.match(line.rstrip('\n'))
        if m:
            ind, op, ra, rb = m.group(1), m.group(2), int(m.group(3)), int(m.group(4))
            mac = 'PPC_MC_DCBT' if op == 'dcbt' else 'PPC_MC_DCBTST'
            out.append(f'{ind}{mac}({ea_expr(ra, rb, mutant)});\n')
            n += 1
        elif OTHER.match(line):
            raise SystemExit(f'unrecognized cache hint form: {line.strip()}')
    return out, n


def header_line(variant: str) -> str:
    if variant == 'switch':
        return '#include "mc_dcbt_switch.h" // dcbt_pass.py\n'
    if variant == 'block':
        return '#define RECOMP_MC_DCBT_FIXED_BLOCK 1\n#include "mc_dcbt_fixed.h" // dcbt_pass.py\n'
    return '#include "mc_dcbt_fixed.h" // dcbt_pass.py\n'


def main():
    a = sys.argv[1:]
    variant = a[a.index('--variant') + 1] if '--variant' in a else 'switch'
    mutant = a[a.index('--mutant') + 1] if '--mutant' in a else ''
    src, dst = Path(a[0]), Path(a[1])
    if dst.exists():
        raise SystemExit(f'{dst} exists')
    lines = src.read_text(errors='strict').splitlines(keepends=True)
    k = next(i for i, l in enumerate(lines[:5]) if l.strip() == INCLUDE)
    body, n = rewrite_lines(lines, mutant)
    body.insert(k + 1, header_line(variant))
    dst.write_text(''.join(body), encoding='utf-8', newline='\n')
    print(f'{src.name}: {n} sites -> {dst}')


if __name__ == '__main__':
    main()
