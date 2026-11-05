#!/usr/bin/env python3
"""vmx_all.py: package vmx-all (2026-10-02): ONE combined exact drop-in set of the two VMX generated-source passes
  C = vmx_native_arith.py (strict-FP scalarized vector compares / min / max / cmpunord / vctsxs / vctuxs -> native NEON,
      a pure 1:1 rename of the call names + one include line), and
  B = vmx_const_mask.py --mode rev (simde_mm_shuffle_epi8(X, VectorMaskL row 0) -> PPC_VMX_REV128(X) + one define block)
over the EFFECTIVE sources of an attempt (chunk N = attempts/<base>/src/ppc_recomp.N.cpp if present, else the validated-ppc
chunk; never the *-before.cpp backups). Order: C first, then B on C's output (they touch disjoint call names; B's define
block then follows C's include line).
A chunk that the base attempt already rewrote with C (it has C's include line) is taken from the PREVIOUS attempt
(attempts/<prev>/src or validated-ppc) as its pre-C original P, and C(P) must reproduce the base chunk byte for byte.

  inventory [--base v414 --prev v413] OUT.json   per effective chunk: C-eligible calls (inside generated bodies / outside),
        B row-0 mask uses (inside / outside), functions with C / B / both, already-C state.
  build OUT_SRC_DIR [--base v414 --prev v413] [--exclude-b X,Y] [--exclude-c X,Y] [--chunks N,...] [--reuse DIR]
        writes OUT_SRC_DIR/ppc_recomp.N.cpp for every chunk that either pass changes (never overwrites), copies
        vmx_native_arith.h, writes OUT_SRC_DIR/manifest.json (per chunk: rewrite(s), counts, sha256 of P / base / C(P) / out).
        --exclude-b: guest functions whose bodies keep the original mask-load shuffles (B skipped there); --exclude-c X,Y:
        bodies that keep the SIMDe compares (C skipped there; refused in chunks the base already rewrote with C). Names:
        the guest address X of sub_X, else the full body name (e.g. __imp____savevmx_30).
        --reuse DIR: an output whose content equals DIR's file of that name is written as an APFS clone of it (cp -c).
  check OUT_SRC_DIR [--base v414 --prev v413]
        per output chunk, INDEPENDENT inverse checks (no reuse of the forward rewrite code):
          (1) undo B: the define block appears exactly once and equals vmx_const_mask.define_block('rev'); every
              PPC_VMX_REV128(X) maps back to simde_mm_shuffle_epi8(X, <row-0 load>); result = C(P) exactly;
          (2) C's own check --against: C(P) with ppc_native_<op>( mapped back and the include dropped = P exactly
              (vmx_native_arith.check), and no scalarizing op left in any generated body;
          (3) for already-C chunks: C(P) = the base attempt's chunk byte for byte;
          (4) the forward rewrite reproduces the file (B's build-check criterion, with the recorded exclusions).
        Exit 1 on any failure.
Game-derived sources stay under local-analysis/; this tool contains no game code."""
from __future__ import annotations
import argparse, collections, hashlib, json, re, shutil, subprocess, sys
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'own_disc'))
import host_support

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import leaf_locals  # noqa: E402
import vmx_const_mask as vcm  # noqa: E402
import vmx_native_arith as vna  # noqa: E402

MAIN = host_support.package_root(__file__)
VALIDATED = MAIN / 'local-analysis/android-private-trace-window/api26/validated-ppc'
ATTEMPTS = MAIN / 'local-analysis/attempts'
CHUNK_NAME = re.compile(r'^ppc_recomp\.(\d+)\.cpp$')
C_INCLUDE = vna.INCLUDE
B_BLOCK = vcm.define_block('rev')
ROW0 = vcm.ROW0
REV = vcm.MACRO_REV + '('


def sha(text: str) -> str:
    return hashlib.sha256(text.encode()).hexdigest()


def read(p: Path) -> str:
    b = p.read_bytes()
    if b'\r' in b:
        raise SystemExit(f'{p}: carriage return in a generated chunk (newline translation would hide it)')
    return b.decode()


def effective(attempt: str) -> dict[int, Path]:
    out = {}
    for p in VALIDATED.iterdir():
        m = CHUNK_NAME.match(p.name)
        if not m:
            continue
        o = ATTEMPTS / attempt / 'src' / p.name
        out[int(m.group(1))] = o if o.exists() else p
    return dict(sorted(out.items()))


def has_c_include(text: str) -> bool:
    return any(l == C_INCLUDE for l in text.splitlines(keepends=True)[:400])


def pre_c(n: int, base: dict[int, Path], prev: dict[int, Path]) -> tuple[str, str, bool]:
    """(P = pre-C original text, base text, already_c)"""
    bt = read(base[n])
    if has_c_include(bt):
        return read(prev[n]), bt, True
    return bt, bt, False


def functions(lines: list[str]):
    """leaf_locals.functions() with a usable name for every body: the guest address X for PPC_FUNC_IMPL(__imp__sub_X) /
    PPC_FUNC(sub_X), else the full name (e.g. __imp____savevmx_30 for a save/restore helper body)"""
    for i, j, name in leaf_locals.functions(lines):
        if not name:
            m = leaf_locals.START.match(lines[i])
            name = m.group(2).strip() if m else ''
        yield i, j, name


# ------------------------------------------------------------------------------------------------ forward rewrites

def apply_c(text: str, exclude: set[str] = frozenset()) -> tuple[str, collections.Counter]:
    """exactly vmx_native_arith.rewrite(src, dst, None, '', False), in memory; bodies named in `exclude` are left unchanged"""
    lines = text.splitlines(keepends=True)
    out, prev, counts, nfn = [], 0, collections.Counter(), 0
    for i, j, name in functions(lines):
        out.extend(lines[prev:i]); prev = j + 1
        body = lines[i + 1:j]
        if name in exclude or not vna.has_call(body):
            out.extend(lines[i:j + 1]); continue
        nfn += 1
        out.append(lines[i]); out.extend(vna.rename_line(l, counts) for l in body); out.append(lines[j])
    out.extend(lines[prev:])
    if nfn:
        vna.insert_include(out)
    counts['functions'] = nfn
    return ''.join(out), counts


def apply_b(text: str, exclude: set[str]) -> tuple[str, int, collections.Counter]:
    """vmx_const_mask.rewrite_chunk(text, 'rev') restricted to generated bodies not in `exclude` (row-0 uses outside generated
    bodies are refused: the inventory shows there are none)"""
    lines = text.splitlines(keepends=True)
    out, prev, n, per_fn = [], 0, 0, collections.Counter()
    for i, j, name in functions(lines):
        outside = lines[prev:i]
        if any(ROW0 in l for l in outside):
            raise SystemExit('row-0 mask use outside a generated body')
        out.extend(outside); prev = j + 1
        body = lines[i + 1:j]
        if name in exclude or not any(ROW0 in l for l in body):
            out.extend(lines[i:j + 1]); continue
        new, k = vcm.rewrite_lines(body, 'rev')
        n += k; per_fn[name] += k
        out.append(lines[i]); out.extend(new); out.append(lines[j])
    tail = lines[prev:]
    if any(ROW0 in l for l in tail):
        raise SystemExit('row-0 mask use outside a generated body')
    out.extend(tail)
    if n:
        last = max(i for i, l in enumerate(out[:40]) if l.startswith('#include'))
        out.insert(last + 1, B_BLOCK)
    res = ''.join(out)
    if not exclude:
        ref, k = vcm.rewrite_chunk(text, 'rev')
        if ref != res or k != n:
            raise SystemExit('function-scoped B differs from vmx_const_mask.rewrite_chunk')
    return res, n, per_fn


# ------------------------------------------------------------------------------------------------ inverse checks

def undo_b(text: str) -> tuple[str | None, str]:
    """independent inverse of B: drop the one define block, PPC_VMX_REV128(X) -> simde_mm_shuffle_epi8(X, row-0 load)"""
    k = text.count(B_BLOCK)
    if k == 0:
        if REV in text or 'ppc_vmx_rev128_' in text:
            return None, 'rev macro used without its define block'
        return text, 'no B'
    if k != 1:
        return None, f'define block appears {k} times'
    text = text.replace(B_BLOCK, '', 1)
    if 'ppc_vmx_rev128_' in text or '#define PPC_VMX_REV128' in text:
        return None, 'stray rev helper outside the define block'
    out, i = [], 0
    while True:
        k = text.find(REV, i)
        if k < 0:
            out.append(text[i:]); break
        if k > 0 and (text[k - 1].isalnum() or text[k - 1] == '_'):
            return None, 'PPC_VMX_REV128 as part of a longer name'
        p, depth = k + len(REV), 0
        while p < len(text):
            c = text[p]
            if c == '(':
                depth += 1
            elif c == ')':
                if depth == 0: break
                depth -= 1
            elif c == '\n':
                return None, 'unterminated PPC_VMX_REV128('
            p += 1
        out.append(text[i:k] + vcm.SHUF + text[k + len(REV):p] + ', ' + ROW0 + ')')
        i = p + 1
    return ''.join(out), 'ok'


def c_check(c_text: str, pre: str, exclude: set[str] = frozenset()) -> list[str]:
    """vmx_native_arith.check(path, None, against) in memory: include present when natives are used, no scalarizing op left
    in a generated body (other than the excluded ones), and the rename mapped back + include dropped == P"""
    lines = c_text.splitlines(keepends=True)
    bad = []
    if any(vna.NATIVE.search(l.partition('//')[0]) for l in lines) and not any(l.startswith(C_INCLUDE.split(' //')[0]) for l in lines[:400]):
        bad.append('include missing')
    for i, j, name in functions(lines):
        if name not in exclude and vna.has_call(lines[i + 1:j]):
            bad.append(f'{name}: scalarizing op left')
    back = {v + '(': k + '(' for k, v in vna.OPS.items()}
    undone = [l for l in lines if l != C_INCLUDE]
    undone = [re.sub(r'(?<![A-Za-z0-9_])ppc_native_[a-z0-9_]+\(', lambda m: back.get(m.group(0), m.group(0)), l) for l in undone]
    if ''.join(undone) != pre:
        bad.append('not a pure rename of P')
    return bad


# ------------------------------------------------------------------------------------------------ commands

def inventory(base_name: str, prev_name: str, out: Path) -> None:
    base, prev = effective(base_name), effective(prev_name)
    rows, tot = [], collections.Counter()
    for n in base:
        pre, bt, already = pre_c(n, base, prev)
        lines = pre.splitlines(keepends=True)
        c_in = c_out = b_in = b_out = 0; fc = fb = fboth = 0; inside = [False] * len(lines)
        for i, j, name in leaf_locals.functions(lines):
            for q in range(i, j + 1): inside[q] = True
            body = lines[i + 1:j]
            kc = sum(len(vna.CALL.findall(l.partition('//')[0])) for l in body)
            kb = sum(l.count(ROW0) for l in body)
            c_in += kc; b_in += kb; fc += kc > 0; fb += kb > 0; fboth += kc > 0 and kb > 0
        for q, l in enumerate(lines):
            if not inside[q]:
                c_out += len(vna.CALL.findall(l.partition('//')[0])); b_out += l.count(ROW0)
        r = dict(chunk=n, src=str(base[n].relative_to(MAIN)), already_c=already, c_calls=c_in, c_calls_outside=c_out,
                 b_row0=b_in, b_row0_outside=b_out, fn_c=fc, fn_b=fb, fn_both=fboth)
        rows.append(r)
        for k in ('c_calls', 'c_calls_outside', 'b_row0', 'b_row0_outside', 'fn_c', 'fn_b', 'fn_both'):
            tot[k] += r[k]
        tot['chunks_c'] += r['c_calls'] > 0; tot['chunks_b'] += r['b_row0'] > 0
        tot['chunks_both'] += r['c_calls'] > 0 and r['b_row0'] > 0; tot['chunks_any'] += r['c_calls'] > 0 or r['b_row0'] > 0
        tot['already_c'] += already
    out.write_text(json.dumps(dict(base=base_name, prev=prev_name, totals=dict(tot), chunks=rows), indent=1), encoding='utf-8', newline='\n')
    print(json.dumps(dict(tot)))


def build(out: Path, base_name: str, prev_name: str, exclude: set[str], only: set[int] | None, reuse: Path | None = None,
          exclude_c: set[str] = frozenset()) -> None:
    base, prev = effective(base_name), effective(prev_name)
    out.mkdir(parents=True, exist_ok=True)
    man_path = out / 'manifest.json'
    manifest = json.loads(man_path.read_text(encoding='utf-8')) if man_path.exists() else dict(base=base_name, prev=prev_name, chunks={})
    if manifest['base'] != base_name or manifest['prev'] != prev_name:
        raise SystemExit('manifest is for another base')
    for n in base:
        if only is not None and n not in only:
            continue
        pre, bt, already = pre_c(n, base, prev)
        names = {name for _, _, name in functions(pre.splitlines(keepends=True))}
        if already and exclude_c & names:
            raise SystemExit(f'chunk {n}: C is already in the base there; a C exclusion would change the base')
        c_text, cc = apply_c(pre, exclude_c)
        if already and c_text != bt:
            raise SystemExit(f'chunk {n}: C(P) from {prev[n]} does not reproduce {base[n]}')
        final, nb, per_fn = apply_b(c_text, exclude)
        if final == bt:
            continue
        dst = out / f'ppc_recomp.{n}.cpp'
        if dst.exists():
            raise SystemExit(f'{dst} exists: refusing to overwrite')
        old = reuse / dst.name if reuse else None
        if old is not None and old.exists() and old.read_bytes() == final.encode():
            host_support.clone_or_copy(old, dst)   # identical content: an APFS clone, no extra disk
        else:
            dst.write_text(final, encoding='utf-8', newline='\n')
        nfn = cc.pop('functions')
        excluded_here = sorted(exclude & names)
        rewrites = (['C (in base)'] if already else (['C'] if nfn else [])) + (['B'] if nb else [])
        manifest['chunks'][str(n)] = dict(
            chunk=n, rewrites=rewrites, base_src=str(base[n].relative_to(MAIN)),
            pre_c_src=str((prev[n] if already else base[n]).relative_to(MAIN)), c_functions=nfn, c_calls=dict(cc),
            c_calls_total=sum(cc.values()), b_rewrites=nb, b_functions=len(per_fn), b_excluded=excluded_here,
            c_excluded=sorted(exclude_c & names),
            sha256=dict(pre_c=sha(pre), base=sha(bt), c=sha(c_text), out=sha(final)))
        print(f'chunk {n}: {"+".join(rewrites)}: C {nfn} functions {sum(cc.values())} calls, B {nb} in {len(per_fn)} functions',
              flush=True)
    hdr = out / 'vmx_native_arith.h'
    if not hdr.exists():
        shutil.copyfile(HERE / 'vmx_native_arith.h', hdr)
    manifest['exclude_b'] = sorted(set(manifest.get('exclude_b', [])) | exclude)
    manifest['exclude_c'] = sorted(set(manifest.get('exclude_c', [])) | set(exclude_c))
    man_path.write_text(json.dumps(manifest, indent=1), encoding='utf-8', newline='\n')


def check(out: Path, base_name: str, prev_name: str) -> int:
    base, prev = effective(base_name), effective(prev_name)
    manifest = json.loads((out / 'manifest.json').read_text(encoding='utf-8'))
    exclude = set(manifest.get('exclude_b', []))
    exclude_c = set(manifest.get('exclude_c', []))
    fails = 0; ok = 0
    files = sorted((p for p in out.glob('ppc_recomp.*.cpp') if CHUNK_NAME.match(p.name)), key=lambda p: int(p.name.split('.')[1]))
    if {int(p.name.split('.')[1]) for p in files} != {int(k) for k in manifest['chunks']}:
        print('CHECK: files and manifest disagree'); fails += 1
    for f in files:
        n = int(f.name.split('.')[1]); text = read(f)
        pre, bt, already = pre_c(n, base, prev)
        bad = []
        c_text, why = undo_b(text)
        if c_text is None:
            bad.append('undo B: ' + why)
        else:
            bad += c_check(c_text, pre, exclude_c)
            if already and c_text != bt:
                bad.append('undo B != base chunk (already-C chunk)')
            # forward reproduction (B's build-check criterion), with the recorded exclusions
            fwd, _ = apply_c(pre, set() if already else exclude_c)
            if fwd != c_text:
                bad.append('C(P) != undo B(out)')
            if apply_b(fwd, exclude)[0] != text:
                bad.append('B(C(P)) != out')
        rec = manifest['chunks'].get(str(n), {})
        if rec.get('sha256', {}).get('out') != sha(text) or rec.get('sha256', {}).get('pre_c') != sha(pre) \
                or rec.get('sha256', {}).get('base') != sha(bt):
            bad.append('manifest hashes differ')
        for b in bad[:5]:
            print(f'CHECK chunk {n}: {b}')
        fails += bool(bad); ok += not bad
    print(f'check {out}: {ok} chunks ok, {fails} failed')
    return 1 if fails else 0


def crosscheck(out: Path, base_name: str, prev_name: str, tmp: Path) -> int:
    """the packages' OWN check entry points on every output chunk: vmx_native_arith.check(undo_B(out), against=P's file)
    (C's `check --against`) and B's build-check equality out == vmx_const_mask.rewrite_chunk(C(P)) (no exclusions only)"""
    import tempfile
    base, prev = effective(base_name), effective(prev_name)
    manifest = json.loads((out / 'manifest.json').read_text(encoding='utf-8'))
    exclude = set(manifest.get('exclude_b', []))
    exclude_c = set(manifest.get('exclude_c', []))
    tmp.mkdir(parents=True, exist_ok=True)
    fails = 0; n_ok = 0; b_skipped = []
    for f in sorted((p for p in out.glob('ppc_recomp.*.cpp') if CHUNK_NAME.match(p.name)), key=lambda p: int(p.name.split('.')[1])):
        n = int(f.name.split('.')[1]); text = read(f)
        bt = read(base[n]); already = has_c_include(bt)
        p_file = prev[n] if already else base[n]
        c_text, why = undo_b(text)
        if c_text is None:
            print(f'chunk {n}: undo B failed: {why}'); fails += 1; continue
        c_names = {name for _, _, name in leaf_locals.functions(c_text.splitlines(keepends=True))}
        # vmx_native_arith.check's own scan of left-over scalarizing ops, minus the C-excluded bodies (by its own names)
        funcs = None if not (exclude_c & c_names) else c_names - exclude_c
        with tempfile.NamedTemporaryFile('w', dir=tmp, suffix=f'.{n}.cpp', delete=True, encoding='utf-8', newline='\n') as t:
            t.write(c_text); t.flush()
            import contextlib, io
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = vna.check(Path(t.name), funcs, p_file)
        if rc:
            print(f'chunk {n}: vmx_native_arith.check --against {p_file.name}: FAIL\n{buf.getvalue()}'); fails += 1; continue
        names = {name for _, _, name in functions(c_text.splitlines(keepends=True))}
        if exclude & names:
            b_skipped.append(n)
        elif vcm.rewrite_chunk(c_text, 'rev')[0] != text:
            print(f'chunk {n}: out != vmx_const_mask.rewrite_chunk(C(P))'); fails += 1; continue
        n_ok += 1
    print(f'crosscheck: {n_ok} ok, {fails} failed; B equality via function-scoped exclusions (vmx_all.check) for chunks {b_skipped}')
    return 1 if fails else 0


def mutants(out: Path, base_name: str, prev_name: str, tmp: Path) -> int:
    """check must reject deliberately wrong outputs: (1) B mutant define block (two bytes swapped), (2) C rename mutant
    (one ppc_native_max_ps -> ppc_native_min_ps), (3) one PPC_VMX_REV128 wrapper dropped (that byte reversal lost),
    (4) one unrelated line deleted"""
    import tempfile
    manifest = json.loads((out / 'manifest.json').read_text(encoding='utf-8'))
    both = [int(k) for k, v in manifest['chunks'].items() if 'B' in v['rewrites'] and len(v['rewrites']) == 2]
    n = sorted(both)[0]
    text = read(out / f'ppc_recomp.{n}.cpp')
    cases = {
        'b-define-swap-bytes': text.replace(B_BLOCK, vcm.define_block('rev', 'swap-bytes'), 1),
        'c-max-as-min': text.replace('ppc_native_max_ps(', 'ppc_native_min_ps(', 1) if 'ppc_native_max_ps(' in text else None,
        'b-unwrapped': text[:text.index(B_BLOCK) + len(B_BLOCK)] +   # a call in a body, not the define block
            text[text.index(B_BLOCK) + len(B_BLOCK):].replace(REV, '(', 1),
        'line-deleted': ''.join(l for i, l in enumerate(text.splitlines(keepends=True)) if i != 200),
    }
    sub = tmp / 'mutants'
    detected = 0
    for name, mtext in cases.items():
        if mtext is None or mtext == text:
            print(f'mutant {name}: not applicable to chunk {n}'); continue
        d = sub / name; d.mkdir(parents=True, exist_ok=True)
        (d / f'ppc_recomp.{n}.cpp').write_text(mtext, encoding='utf-8', newline='\n')
        m = dict(manifest); m['chunks'] = {str(n): manifest['chunks'][str(n)]}
        (d / 'manifest.json').write_text(json.dumps(m), encoding='utf-8', newline='\n')
        import contextlib, io
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = check(d, base_name, prev_name)
        print(f'mutant {name} (chunk {n}): {"detected" if rc else "NOT DETECTED"}: ' + buf.getvalue().strip().splitlines()[0])
        detected += bool(rc)
    return 0 if detected == sum(1 for v in cases.values() if v is not None and v != text) else 1


def main() -> None:
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest='cmd', required=True)
    for name in ('crosscheck', 'mutants'):
        p = sp.add_parser(name); p.add_argument('path'); p.add_argument('tmp')
        p.add_argument('--base', default='v414'); p.add_argument('--prev', default='v413')
    for name in ('inventory', 'build', 'check'):
        p = sp.add_parser(name); p.add_argument('path')
        p.add_argument('--base', default='v414'); p.add_argument('--prev', default='v413')
        if name == 'build':
            p.add_argument('--exclude-b', default=''); p.add_argument('--chunks', default=''); p.add_argument("--reuse", default="")
            p.add_argument('--exclude-c', default='')
    a = ap.parse_args()
    if a.cmd == 'inventory':
        inventory(a.base, a.prev, Path(a.path))
    elif a.cmd == 'crosscheck':
        raise SystemExit(crosscheck(Path(a.path), a.base, a.prev, Path(a.tmp)))
    elif a.cmd == 'mutants':
        raise SystemExit(mutants(Path(a.path), a.base, a.prev, Path(a.tmp)))
    elif a.cmd == 'build':
        build(Path(a.path), a.base, a.prev, set(x for x in a.exclude_b.split(',') if x),
              set(int(x) for x in a.chunks.split(',')) if a.chunks else None, Path(a.reuse) if a.reuse else None,
              set(x for x in a.exclude_c.split(',') if x))
    else:
        raise SystemExit(check(Path(a.path), a.base, a.prev))


if __name__ == '__main__':
    main()
