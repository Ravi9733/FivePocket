#!/usr/bin/env python3
"""vmx_native_arith.py: exact generated-source pass for the VMX vector-FP compares / saturating conversions that the NDK clang
scalarizes under -ffp-model=strict (see vmx_native_arith.h for the per-op equivalences and why they are bit-exact).

  rewrite IN.cpp OUT.cpp [--funcs X,Y] [--twin FLAG [--dispatch]]
        In every generated body (PPC_FUNC_IMPL / PPC_FUNC) - or only those of guest addresses X,Y - replace each call
        simde_mm_cmpgt_ps( / cmpge_ps( / cmpeq_ps( / cmplt_ps( / max_ps( / min_ps( / simde_mm_vctsxs( / simde_mm_vctuxs(
        by ppc_native_<op>( (same arguments, same result bits) and include "vmx_native_arith.h" after the chunk's includes.
        --twin FLAG: in-run A/B instead; every function that has such a call keeps its original body and gains a noinline twin
        with the native calls, run while the extern "C" std::atomic<unsigned> FLAG is non-zero (macro_twin.py's scheme);
        --dispatch moves both bodies into noinline functions behind a frame-less dispatcher (fair A/B: both sides pay the same
        entry). Prints the number of rewritten calls per op.
  check OUT.cpp [--funcs X,Y] [--against IN.cpp]
        exit 1 if a selected body (or, with --twin, a twin) still calls a scalarizing SIMDe op, or if the include is missing;
        --against (plain rewrite): OUT with every ppc_native_<op>( mapped back to simde_mm_<op>( and the include line dropped
        must equal IN.cpp exactly (the pass is a pure 1:1 rename and changes nothing else).
  batch TOP.json OUT_DIR [--twin FLAG [--dispatch]]
        rewrite every effective chunk holding a function of a vmx_native_arith.py inventory list (`top`): plain = every
        function of those chunks, --twin = only the listed functions; checks each output and copies vmx_native_arith.h to
        OUT_DIR. Refuses to overwrite existing files.
  inventory OUT.json [--scalarized scalarized-all-v2-topN.json] [--top N]
        corpus-wide use counts of the replaced ops (v407 effective chunks) and the top functions by scalarized-lane samples.
  extract CHUNK.cpp X OUT.cpp [--native]
        a compile-only TU: the chunk's includes and declarations + the body of guest function X (rewritten with --native), for
        exact-flag codegen comparisons (vmx_exact_compile.py).
VMX_NATIVE_MUTANT=swap-max / gt-as-ge (tests only): rewrite max_ps to ppc_native_min_ps / cmpgt_ps to ppc_native_cmpge_ps,
which check accepts (still a rename) but the whole-function differential test (vmx_native_check.py) must catch.
Game-derived sources stay under local-analysis/; this tool contains no game code."""
from __future__ import annotations
import argparse, collections, json, os, re, sys
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'own_disc'))
import host_support

sys.path.insert(0, str(Path(__file__).resolve().parent))
import leaf_locals  # noqa: E402  (functions(): the generated-body scanner)

OPS = {'simde_mm_cmpgt_ps': 'ppc_native_cmpgt_ps', 'simde_mm_cmpge_ps': 'ppc_native_cmpge_ps',
       'simde_mm_cmpeq_ps': 'ppc_native_cmpeq_ps', 'simde_mm_cmplt_ps': 'ppc_native_cmplt_ps',
       'simde_mm_max_ps': 'ppc_native_max_ps', 'simde_mm_min_ps': 'ppc_native_min_ps',
       'simde_mm_cmpunord_ps': 'ppc_native_cmpunord_ps',
       'simde_mm_vctsxs': 'ppc_native_vctsxs', 'simde_mm_vctuxs': 'ppc_native_vctuxs'}
CALL = re.compile(r'(?<![A-Za-z0-9_])(' + '|'.join(OPS) + r')\(')
NATIVE = re.compile(r'(?<![A-Za-z0-9_])ppc_native_[a-z0-9_]+\(')
INCLUDE = '#include "vmx_native_arith.h" // vmx_native_arith.py\n'
MAIN = host_support.package_root(__file__)
VALIDATED = MAIN / 'local-analysis/android-private-trace-window/api26/validated-ppc'
CHUNK_NAME = re.compile(r'^ppc_recomp\.(\d+)\.cpp$')   # the corpus dir also holds *-before.cpp backups: not chunks


def effective_chunks(attempt: str = 'v407') -> list[Path]:
    """chunk N = attempts/<attempt>/src/ppc_recomp.N.cpp if present, else the validated corpus's (backups excluded)"""
    out = []
    for p in sorted((p for p in VALIDATED.iterdir() if CHUNK_NAME.match(p.name)), key=lambda p: int(CHUNK_NAME.match(p.name).group(1))):
        o = MAIN / 'local-analysis/attempts' / attempt / 'src' / p.name
        out.append(o if o.exists() else p)
    return out


def inventory(out: Path, scalarized: Path | None, top: int) -> None:
    """corpus-wide calls of the scalarizing ops per function (v407 effective chunks) and, with a vmx_scalarized_cost.py v2
    result, the top functions by scalarized-compare samples with their chunk"""
    ops = collections.Counter(); where = {}; fn_ops = collections.defaultdict(collections.Counter); cur = None
    for q in effective_chunks():
        with q.open(errors='replace') as f:
            for line in f:
                if line.startswith('PPC_FUNC_IMPL(__imp__sub_') and line.rstrip().endswith('{'):
                    cur = line[25:33]; where[cur] = str(q); continue
                if 'simde_mm_' in line:
                    for m in CALL.finditer(line.partition('//')[0]):
                        ops[m.group(1)] += 1; fn_ops[cur][m.group(1)] += 1
    print('corpus calls (v407 effective chunks):', ', '.join(f'{k[9:]} {v}' for k, v in ops.most_common()),
          f'in {sum(1 for f in fn_ops if f)} functions')
    data = dict(corpus_ops=dict(ops), functions_with_ops=sum(1 for f in fn_ops if f))
    if scalarized:
        d = json.loads(scalarized.read_text(encoding='utf-8'))
        agg = collections.Counter(); samp = collections.Counter()
        for r in d['functions']:
            a = re.search(r'sub_([0-9A-F]{8})', r['fn']).group(1); agg[a] += r['at_pc']; samp[a] += r['samples']
        rows = [dict(addr=a, scalarized_samples=v, fn_samples=samp[a], chunk=where.get(a), ops=dict(fn_ops[a]))
                for a, v in agg.most_common(top)]
        data.update(scalarized_total=sum(agg.values()), top=rows)
        chunks = collections.Counter(r['chunk'] for r in rows)
        print(f'top {top}: {sum(r["scalarized_samples"] for r in rows)} of {sum(agg.values())} scalarized samples, in {len(chunks)} chunks')
        for r in rows[:20]:
            print(f'  {r["addr"]} {r["scalarized_samples"]:4d}/{r["fn_samples"]:4d} {Path(r["chunk"]).parent.parent.name}/{Path(r["chunk"]).name} '
                  + ' '.join(f'{k[9:]}:{v}' for k, v in r['ops'].items()))
    out.write_text(json.dumps(data, indent=1), encoding='utf-8', newline='\n')


def rename_line(line: str, counts: collections.Counter) -> str:
    code, sep, comment = line.partition('//')
    mutant = os.environ.get('VMX_NATIVE_MUTANT', '')

    def sub(m):
        counts[m.group(1)] += 1
        new = OPS[m.group(1)]
        if mutant == 'swap-max' and m.group(1) == 'simde_mm_max_ps':
            new = 'ppc_native_min_ps'
        if mutant == 'gt-as-ge' and m.group(1) == 'simde_mm_cmpgt_ps':
            new = 'ppc_native_cmpge_ps'
        return new + '('
    return CALL.sub(sub, code) + sep + comment


def has_call(lines) -> bool:
    return any(CALL.search(l.partition('//')[0]) for l in lines)


def insert_include(out: list[str], extra: str = '') -> None:
    k = max(n for n, l in enumerate(out[:200]) if l.startswith('#include')) + 1
    out.insert(k, INCLUDE + extra)


def rewrite(src: Path, dst: Path, funcs: set[str] | None, twin: str, dispatch: bool) -> collections.Counter:
    lines = src.read_text(encoding='utf-8').splitlines(keepends=True)
    out, prev, counts, nfn = [], 0, collections.Counter(), 0
    for i, j, name in leaf_locals.functions(lines):
        out.extend(lines[prev:i]); prev = j + 1
        body = lines[i + 1:j]
        if (funcs is not None and name not in funcs) or not has_call(body):
            out.extend(lines[i:j + 1]); continue
        nfn += 1
        new = [rename_line(l, counts) for l in body]
        if not twin:
            out.append(lines[i]); out.extend(new); out.append(lines[j]); continue
        out.append(f'static __attribute__((noinline)) void vna_tw_sub_{name}(PPCContext& __restrict ctx, uint8_t* base) {{\n')
        out.extend(new); out.append('}\n')
        if dispatch:
            out.append(f'static __attribute__((noinline)) void vna_or_sub_{name}(PPCContext& __restrict ctx, uint8_t* base) {{\n')
            out.extend(body); out.append('}\n')
            out.append(lines[i])
            out.append(f'\tif ({twin}.load(std::memory_order_relaxed)) vna_tw_sub_{name}(ctx, base); else vna_or_sub_{name}(ctx, base); // vmx native twin dispatch\n')
            out.append(lines[j])
        else:
            out.append(lines[i])
            out.append(f'\tif ({twin}.load(std::memory_order_relaxed)) {{ vna_tw_sub_{name}(ctx, base); return; }} // vmx native twin\n')
            out.extend(body); out.append(lines[j])
    out.extend(lines[prev:])
    if nfn:
        insert_include(out, f'#include <atomic>\nextern "C" std::atomic<unsigned> {twin}; // vmx native twin switch\n' if twin else '')
    dst.write_text(''.join(out), encoding='utf-8', newline='\n')
    counts['functions'] = nfn
    return counts


def check(path: Path, funcs: set[str] | None, against: Path | None = None) -> int:
    lines = path.read_text(encoding='utf-8').splitlines(keepends=True)
    bad = []
    twin_mode = any('vmx native twin' in l for l in lines[:400]) or any(l.startswith('static __attribute__((noinline)) void vna_tw_sub_') for l in lines)
    if any(NATIVE.search(l.partition('//')[0]) for l in lines) and not any(l.startswith(INCLUDE.split(' //')[0]) for l in lines[:400]):
        bad.append('include missing')
    # twins are not PPC_FUNC bodies: scan them by their header line
    starts = [(n, l) for n, l in enumerate(lines) if l.startswith('static __attribute__((noinline)) void vna_tw_sub_')]
    for n, l in starts:
        j = n + 1
        while not lines[j].startswith('}'): j += 1
        if has_call(lines[n + 1:j]): bad.append(f'twin {l.split("(")[0].split()[-1]} still calls a scalarizing op')
    if not twin_mode:
        for i, j, name in leaf_locals.functions(lines):
            if funcs is not None and name not in funcs: continue
            if has_call(lines[i + 1:j]): bad.append(f'{name}: scalarizing op left')
    if against is not None and not twin_mode:
        # the plain rewrite must be exactly a rename: OUT with every ppc_native_<op>( mapped back is ORIG plus the include line
        back = {v + '(': k + '(' for k, v in OPS.items()}
        undone = [l for l in lines if l != INCLUDE]
        undone = [re.sub(r'(?<![A-Za-z0-9_])ppc_native_[a-z0-9_]+\(', lambda m: back.get(m.group(0), m.group(0)), l) for l in undone]
        if undone != against.read_text(encoding='utf-8').splitlines(keepends=True):
            bad.append(f'not a pure rename of {against.name}')
    for b in bad[:20]: print('CHECK:', b)
    print(f'check {path.name}: {"FAIL" if bad else "ok"}')
    return 1 if bad else 0


def batch(top_json: Path, out: Path, twin: str, dispatch: bool) -> int:
    """rewrite every chunk that holds one of the listed functions: plain mode = every function of those chunks, twin mode =
    only the listed functions (in-run A/B); check each output; copy vmx_native_arith.h next to them"""
    rows = json.loads(top_json.read_text(encoding='utf-8'))['top']
    by_chunk = collections.defaultdict(set)
    for r in rows:
        by_chunk[r['chunk']].add(r['addr'])
    out.mkdir(parents=True, exist_ok=True)
    total = collections.Counter(); fails = 0
    for chunk, addrs in sorted(by_chunk.items(), key=lambda kv: int(Path(kv[0]).name.split('.')[1])):
        src = Path(chunk); dst = out / src.name
        if dst.exists():
            raise SystemExit(f'{dst} exists: refusing to overwrite')
        c = rewrite(src, dst, addrs if twin else None, twin, dispatch)
        total.update(c)
        fails += check(dst, addrs if twin else None, None if twin else src)
    import shutil
    shutil.copyfile(Path(__file__).resolve().parent / 'vmx_native_arith.h', out / 'vmx_native_arith.h')
    nfn = total.pop('functions')
    print(f'{len(by_chunk)} chunks, {nfn} functions rewritten: ' + ', '.join(f'{k[9:]} {v}' for k, v in total.most_common()))
    return 1 if fails else 0


def extract(chunk: Path, name: str, dst: Path, native: bool) -> None:
    lines = chunk.read_text(encoding='utf-8').splitlines(keepends=True)
    head = [l for l in lines[:20000] if l.startswith('#include') or (l.startswith('PPC_FUNC_IMPL(') and l.rstrip().endswith(';'))
            or l.startswith('PPC_EXTERN_FUNC(') or l.startswith('PPC_EXTERN_IMPORT(')]
    for i, j, n in leaf_locals.functions(lines):
        if n == name:
            body = lines[i + 1:j]
            if native:
                c = collections.Counter(); body = [rename_line(l, c) for l in body]
            out = head + ([INCLUDE] if native else []) + [lines[i]] + body + [lines[j]]
            dst.write_text(''.join(out), encoding='utf-8', newline='\n'); return
    raise SystemExit(f'{name} not in {chunk}')


def main() -> None:
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest='cmd', required=True)
    r = sp.add_parser('rewrite'); r.add_argument('src'); r.add_argument('dst'); r.add_argument('--funcs')
    r.add_argument('--twin', default=''); r.add_argument('--dispatch', action='store_true')
    c = sp.add_parser('check'); c.add_argument('path'); c.add_argument('--funcs'); c.add_argument('--against')
    b = sp.add_parser('batch'); b.add_argument('top_json'); b.add_argument('out')
    b.add_argument('--twin', default=''); b.add_argument('--dispatch', action='store_true')
    v = sp.add_parser('inventory'); v.add_argument('out'); v.add_argument('--scalarized'); v.add_argument('--top', type=int, default=50)
    e = sp.add_parser('extract'); e.add_argument('chunk'); e.add_argument('name'); e.add_argument('dst'); e.add_argument('--native', action='store_true')
    a = ap.parse_args()
    if a.cmd == 'rewrite':
        funcs = set(a.funcs.split(',')) if a.funcs else None
        counts = rewrite(Path(a.src), Path(a.dst), funcs, a.twin, a.dispatch)
        print(f'{Path(a.src).name}: {counts.pop("functions")} functions, ' + ', '.join(f'{k[9:]} {v}' for k, v in counts.most_common()))
    elif a.cmd == 'check':
        raise SystemExit(check(Path(a.path), set(a.funcs.split(',')) if a.funcs else None, Path(a.against) if a.against else None))
    elif a.cmd == 'batch':
        raise SystemExit(batch(Path(a.top_json), Path(a.out), a.twin, a.dispatch))
    elif a.cmd == 'inventory':
        inventory(Path(a.out), Path(a.scalarized) if a.scalarized else None, a.top)
    else:
        extract(Path(a.chunk), a.name, Path(a.dst), a.native)


if __name__ == '__main__':
    main()
