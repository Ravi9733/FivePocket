#!/usr/bin/env python3
"""vmx_const_mask.py OUT_SRC_DIR CHUNK [CHUNK ...] [--attempt v407] [--mode rev|const]: exact generated-source pass for the
byte-reversing VMX loads/stores (lvx/stvx/lvx128/stvx128 and their indexed forms) of the effective corpus of the attempt
(attempts/<attempt>/src/ppc_recomp.N.cpp when present, else the validated-ppc chunk). The corpus emits every one of them as
    simde_mm_shuffle_epi8(X, simde_mm_load_si128((simde__m128i*)VectorMaskL))
i.e. a tbl through row 0 of VectorMaskL, the full 16-byte reversal {15, ..., 0}.
  --mode rev (default): the whole shuffle becomes PPC_VMX_REV128(X), a 16-byte reversal written as __builtin_shufflevector
    (LLVM lowers it to rev64 + ext #8, no mask, no constant register; and the reversal is visible to the optimizer).
  --mode const: only the mask operand becomes PPC_VMX_BSWAP128_MASK = simde_mm_setr_epi8(15, ..., 0) (SIMDe's tbl stays).
    Measured on chunk 17: the constant is hoisted into a register that is live across calls and spilled (vector stack
    traffic 1120 -> 2522 instructions), so this mode is kept only for comparison.
Why both are exact: VectorMaskL (validated-ppc/ppc_context.h) is a non-const `inline uint8_t[]`, but nothing ever writes it:
the corpus only loads it (vmx_inventory.py: 136,175 row-0 loads, 13,054 row loads, 0 other uses) and no runtime or tool
source names it. So each load returns {0x0F, ..., 0x00} and SIMDe's shuffle_epi8 (tbl with index & 0x8F; all indices < 16)
moves host byte 15 - j to byte j: exactly the reversal. Bit patterns are moved, never interpreted (NaN payloads, denormals
and flush mode are irrelevant). Tested by vmx_const_mask_micro.py and vmx_const_mask_check.py (with mutants).
Why it can be faster: with the mask in memory and -fno-strict-aliasing, LLVM must reload it after every store (ctx or guest)
and cannot fold SIMDe's `vandq_u8(mask, 0x8F)`: per use adrp/add + ldr q + movi + and before the tbl.

Writes OUT_SRC_DIR/ppc_recomp.N.cpp per chunk (game-derived: keep under local-analysis/), never overwriting, and prints the
replacement counts. rewrite_lines() / define_block() are shared with the host differential tests."""
from __future__ import annotations
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vmx_inventory  # noqa: E402

ROW0 = 'simde_mm_load_si128((simde__m128i*)VectorMaskL)'
SHUF = 'simde_mm_shuffle_epi8('
MACRO_CONST = 'PPC_VMX_BSWAP128_MASK'
MACRO_REV = 'PPC_VMX_REV128'
REV = '15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0'
MUTANTS = {  # deliberately wrong reversals the differential tests must reject
    'set-order': '15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0|set',  # set/setr confusion: identity
    'swap-bytes': '14, 15, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0',     # two lanes exchanged
}


def define_block(mode: str = 'rev', mutant: str = '') -> str:
    idx = MUTANTS[mutant] if mutant else REV
    if mode == 'const':
        fn = 'simde_mm_set_epi8' if idx.endswith('|set') else 'simde_mm_setr_epi8'
        return ('// vmx_const_mask.py --mode const: VectorMaskL row 0 (full 16-byte reversal) as a compile-time constant;\n'
                '// VectorMaskL is never written, so every load of it returned exactly these bytes.\n'
                f'#ifndef {MACRO_CONST}\n#define {MACRO_CONST} {fn}({idx.split("|")[0]})\n#endif\n')
    order = '0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15' if idx.endswith('|set') else idx
    return ('// vmx_const_mask.py --mode rev: simde_mm_shuffle_epi8(x, VectorMaskL row 0) is the full 16-byte reversal of x\n'
            '// (VectorMaskL is never written); written as a generic shuffle (AArch64: rev64 + ext #8, no mask register).\n'
            f'#ifndef {MACRO_REV}\n'
            'static inline __attribute__((always_inline)) simde__m128i ppc_vmx_rev128_(simde__m128i x) {\n'
            '    static_assert(sizeof(simde__m128i) == 16, "simde__m128i");\n'
            '    uint8x16_t b; __builtin_memcpy(&b, &x, 16);\n'
            f'    b = __builtin_shufflevector(b, b, {order});\n'
            '    simde__m128i r; __builtin_memcpy(&r, &b, 16); return r;\n'
            '}\n'
            f'#define {MACRO_REV}(x) ppc_vmx_rev128_(x)\n#endif\n')


def _rewrite_rev(line: str) -> tuple[str, int]:
    out, i, n = [], 0, 0
    while True:
        k = line.find(SHUF, i)
        if k < 0:
            out.append(line[i:]); break
        j = k + len(SHUF); depth = 0; p = j
        while p < len(line):  # the first argument: up to the top-level comma
            c = line[p]
            if c == '(': depth += 1
            elif c == ')':
                if depth == 0: break
                depth -= 1
            elif c == ',' and depth == 0: break
            p += 1
        tail = line[p:]
        if tail.startswith(', ' + ROW0 + ')'):
            out.append(line[i:k] + f'{MACRO_REV}(' + line[j:p] + ')')
            i = p + len(', ' + ROW0 + ')'); n += 1
        else:
            out.append(line[i:j]); i = j
    return ''.join(out), n


def rewrite_lines(lines: list[str], mode: str = 'rev') -> tuple[list[str], int]:
    out, n = [], 0
    for line in lines:
        if ROW0 in line:
            if mode == 'const':
                k = line.count(ROW0); line = line.replace(ROW0, MACRO_CONST)
            else:
                line, k = _rewrite_rev(line)
                if ROW0 in line:
                    raise SystemExit(f'row-0 mask load outside simde_mm_shuffle_epi8(X, mask): {line.strip()[:200]}')
            n += k
        out.append(line)
    return out, n


def rewrite_chunk(text: str, mode: str = 'rev') -> tuple[str, int]:
    lines = text.splitlines(keepends=True)
    new, n = rewrite_lines(lines, mode)
    if not n:
        return text, 0
    last = max(i for i, l in enumerate(new[:40]) if l.startswith('#include'))
    new.insert(last + 1, define_block(mode))
    return ''.join(new), n


def main() -> None:
    a = sys.argv[1:]
    out = Path(a[0]); attempt = a[a.index('--attempt') + 1] if '--attempt' in a else 'v407'
    mode = a[a.index('--mode') + 1] if '--mode' in a else 'rev'
    chunks = [int(x) for x in a[1:] if x.isdigit()]
    paths = {int(p.stem.split('.')[1]): p for p in vmx_inventory.corpus(attempt)}
    out.mkdir(parents=True, exist_ok=True)
    total = 0
    for c in chunks:
        src = paths[c]; text = src.read_text()
        new, n = rewrite_chunk(text, mode)
        dst = out / src.name
        if dst.exists():
            raise SystemExit(f'{dst} exists: refusing to overwrite')
        dst.write_text(new, encoding='utf-8', newline='\n')
        total += n
        print(f'chunk {c}: {n} row-0 byte reversals rewritten (--mode {mode}) from {src}; remaining row-0 loads {new.count(ROW0)}')
    print(f'total {total}')


if __name__ == '__main__':
    main()
