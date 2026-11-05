#!/usr/bin/env python3
"""vmx_inventory.py OUT_PREFIX [--attempt v407]: read-only inventory of the VMX data-movement patterns in the effective
generated corpus (local-analysis/attempts/<attempt>/src/ppc_recomp.N.cpp when present, else the validated-ppc chunk).
Writes OUT_PREFIX.patterns.tsv (normalised statement pattern -> count over the corpus) and OUT_PREFIX.funcs.tsv (per guest
function: chunk, number of byte-reversing constant-mask loads/stores (VectorMaskL row 0), variable-row VectorMaskL/R and
VectorShiftTable uses, and other vector statements). Also reports any use of the permute tables other than a 16-byte load
(e.g. a write), which would make a constant-table rewrite unsound."""
from __future__ import annotations
import collections, os, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN = Path(os.environ.get('RECOMP_DATA_ROOT', ROOT))   # a worktree without local-analysis: the main checkout's data
if not (ROOT / 'local-analysis/attempts/config.json').exists():
    ROOT = MAIN
VALIDATED = ROOT / 'local-analysis/android-private-trace-window/api26/validated-ppc'
START = re.compile(r'^(?:PPC_FUNC_IMPL|PPC_FUNC)\((?:__imp__)?(sub_[0-9A-F]{8})\)\s*\{\s*$')
TABLE = re.compile(r'\b(VectorMaskL|VectorMaskR|VectorShiftTableL|VectorShiftTableR)\b')
LOAD_CONST = 'simde_mm_load_si128((simde__m128i*)VectorMaskL)'
CONST_SHUFFLE = re.compile(r'simde_mm_shuffle_epi8\([^;]*simde_mm_set1_epi(?:8|16)\(')
LOAD_ROW = re.compile(r'simde_mm_load_si128\(\(simde__m128i\*\)&(VectorMaskL|VectorMaskR|VectorShiftTableL|VectorShiftTableR)\[')


def corpus(attempt: str) -> list[Path]:
    # only the canonical ppc_recomp.N.cpp (validated-ppc also holds backups such as ppc_recomp.13.u64-direct-before.cpp,
    # which the first version of this glob counted as extra chunks)
    out = []
    names = [p for p in VALIDATED.glob('ppc_recomp.*.cpp') if re.fullmatch(r'ppc_recomp\.[0-9]+\.cpp', p.name)]
    for p in sorted(names, key=lambda p: int(p.stem.split('.')[1])):
        o = ROOT / 'local-analysis/attempts' / attempt / 'src' / p.name
        out.append(o if o.exists() else p)
    return out


def norm(s: str) -> str:
    s = re.sub(r'0x[0-9A-Fa-f]+', 'K', s.strip())
    return re.sub(r'\b[0-9]+\b|(?<=[a-z])[0-9]+', 'N', s)


def main() -> None:
    a = sys.argv[1:]
    prefix = Path(a[0]); attempt = a[a.index('--attempt') + 1] if '--attempt' in a else 'v407'
    pats = collections.Counter(); rows = []; bad = []
    for path in corpus(attempt):
        chunk = int(path.stem.split('.')[1]); fn = None; c = collections.Counter()
        for no, line in enumerate(path.open(errors='replace'), 1):
            m = START.match(line)
            if m:
                fn = m.group(1); c = collections.Counter(); continue
            if fn and line.startswith('}'):
                if c: rows.append((fn, chunk, c))
                fn = None; continue
            if 'simde_' not in line and not TABLE.search(line):
                continue
            pats[norm(line)] += 1
            if fn is None: continue
            k_const = line.count(LOAD_CONST); k_row = len(LOAD_ROW.findall(line))
            if len(TABLE.findall(line)) != k_const + k_row:
                bad.append(f'{path.name}:{no}: {line.strip()[:160]}')
            c['bswap_const'] += k_const; c['table_row'] += k_row
            c['perm'] += line.count('simde_mm_perm_epi8_(')
            c['const_shuffle'] += len(CONST_SHUFFLE.findall(line))
            c['vec_stmt'] += 1
    with open(str(prefix) + '.patterns.tsv', 'w', encoding='utf-8', newline='\n') as f:
        for p, n in pats.most_common(): f.write(f'{n}\t{p}\n')
    with open(str(prefix) + '.funcs.tsv', 'w', encoding='utf-8', newline='\n') as f:
        f.write('# func\tchunk\tbswap_const\ttable_row\tvec_stmt\tperm\tconst_shuffle\n')
        for fn, chunk, c in rows:
            f.write(f'{fn}\t{chunk}\t{c["bswap_const"]}\t{c["table_row"]}\t{c["vec_stmt"]}\t{c["perm"]}\t{c["const_shuffle"]}\n')
    tot = collections.Counter()
    for _, _, c in rows: tot.update(c)
    print(f'functions with vector statements: {len(rows)}; totals {dict(tot)}; other table uses: {len(bad)}')
    for b in bad[:20]: print('  ', b)


if __name__ == '__main__':
    main()
