#!/usr/bin/env python3
"""dcbt_build.py OUTDIR VARIANT [--chunks N,M,...] [--rounds R]: produce PGO-safe drop-in chunk sources for the dcbt pass.
For every chunk of the v429 corpus with dcbt/dcbtst sites (OUTDIR/dcbt-sites-v429.json, or --chunks): write
OUTDIR/<dir>/ppc_recomp.N.cpp (dir = src-v429 for VARIANT switch, variants/fixed-<v> otherwise) with dcbt_pass.py, compile it
with the chunk's exact device command (PGO chunks: + -Wbackend-plugin -Wprofile-instr-out-of-date, which report every
function whose profile CFG hash no longer matches; a positive control with a branch per site reports 9 in chunk 17), and
when functions mismatch, exclude their sites (left as comments) and rebuild, up to R rounds. The unmodified v429 objects
have 0 such warnings (chunk 17 checked; see the report). Writes OUTDIR/<dir>/dcbt-build.json (per chunk: sites, rewritten,
excluded functions, rounds, compile seconds, final warnings) and keeps the objects under OUTDIR/obj-dcbt-<variant>/ for the
code-identity and disassembly checks. One compile at a time, nice 10."""
import json, os, re, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import dcbt_pass  # noqa: E402

ROOT = Path(os.environ['RECOMP_DATA_ROOT']) if 'RECOMP_DATA_ROOT' in os.environ else Path(__file__).absolute().parents[3]   # the repository (RECOMP_DATA_ROOT: another checkout's data)
TPL = ROOT / 'local-analysis/agents/costmap-v425/obj/v425'
V429 = ROOT / 'local-analysis/attempts/v429'
MISMATCH = re.compile(r'function control flow change detected \(hash mismatch\) (\S+) Hash')
IMPL = re.compile(r'^PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\) \{')


def template(pgo: bool) -> str:
    return (TPL / ('ppc_recomp.17.g.cmd' if pgo else 'ppc_recomp.107.g.cmd')).read_text().strip()


def command(chunk: int, pgo: bool, src: Path, obj: Path):
    t = template(pgo)
    n = '17' if pgo else '107'
    words = t.split(' ')
    for opt, val in (('-c', str(src)), ('-o', str(obj)), ('-MF', str(obj) + '.d'), ('-MT', str(obj))):
        words[words.index(opt) + 1] = val
    words = [w for w in words if not w.startswith('-I/tmp/')]
    assert not any('costmap-v425' in w or 'attempts/v425' in w for w in words), 'template path left'
    return words + ['-iquote', str(V429 / 'src')] + (['-Wprofile-instr-out-of-date', '-Wbackend-plugin'] if pgo else [])


def rewrite(src: Path, dst: Path, variant: str, exclude: set):
    lines = src.read_text(errors='strict').splitlines(keepends=True)
    out, cur, n, skipped = [], None, 0, 0
    for line in lines:
        if line.startswith('PPC_FUNC_IMPL'):
            m = IMPL.match(line)
            cur = ('__imp__sub_' + m.group(1)) if m else cur
        if cur in exclude and dcbt_pass.HINT.match(line.rstrip('\n')):
            out.append(line); skipped += 1
            continue
        new, k = dcbt_pass.rewrite_lines([line])
        out += new; n += k
    k = next(i for i, l in enumerate(out[:5]) if l.strip() == dcbt_pass.INCLUDE)
    out.insert(k + 1, dcbt_pass.header_line(variant))
    dst.write_text(''.join(out), encoding='utf-8', newline='\n')
    return n, skipped


def main():
    a = sys.argv[1:]
    out, variant = Path(a[0]), a[1]
    rounds = int(a[a.index('--rounds') + 1]) if '--rounds' in a else 4
    att = json.loads((V429 / 'attempt.json').read_text())
    pgo_chunks = set(att['ppc_chunk_extra']['chunks'])
    idx = json.loads((out / 'fn-index-v429.json').read_text())
    sites = json.loads((out / 'dcbt-sites-v429.json').read_text())
    chunk_src = {}
    for fn, v in sites.items():
        if any(x[1] in ('dcbt', 'dcbtst') for x in v):
            chunk_src[idx[fn]['chunk']] = Path(idx[fn]['path'])
    if '--chunks' in a:
        want = {int(x) for x in a[a.index('--chunks') + 1].split(',')}
        chunk_src = {k: v for k, v in chunk_src.items() if k in want}
    d = out / ('src-v429' if variant == 'switch' else f'variants/fixed-{variant}')
    d.mkdir(parents=True, exist_ok=True)
    od = out / f'obj-dcbt-{variant}'
    od.mkdir(exist_ok=True)
    hdr = 'mc_dcbt_switch.h' if variant == 'switch' else 'mc_dcbt_fixed.h'
    (d / hdr).write_text((HERE / hdr).read_text(), encoding='utf-8', newline='\n')
    if variant == 'switch':
        (d / 'mc_dcbt_cfg.h').write_text((HERE / 'mc_dcbt_cfg.h').read_text(), encoding='utf-8', newline='\n')
    log_path = d / 'dcbt-build.json'
    log = json.loads(log_path.read_text()) if log_path.exists() else {}
    pv = 'line' if variant == 'line' else variant
    for chunk in sorted(chunk_src):
        if str(chunk) in log and log[str(chunk)].get('final_mismatch') == []:
            continue
        pgo = chunk in pgo_chunks
        exclude, rec = set(), {'pgo': pgo, 'source': str(chunk_src[chunk]), 'rounds': []}
        for r in range(rounds):
            dst = d / f'ppc_recomp.{chunk}.cpp'
            if dst.exists():
                dst.unlink()  # this tool's own output from the previous round
            n, skipped = rewrite(chunk_src[chunk], dst, pv, exclude)
            obj = od / f'ppc_recomp.{chunk}.o'
            if obj.exists():
                obj.unlink()
            t0 = time.monotonic()
            res = subprocess.run(['nice', '-n', '10'] + command(chunk, pgo, dst, obj), capture_output=True, text=True)
            dt = time.monotonic() - t0
            mism = sorted(set(MISMATCH.findall(res.stderr)))
            other = [l for l in res.stderr.splitlines() if 'warning' in l and 'hash mismatch' not in l and 'warnings generated' not in l]
            rec['rounds'].append({'rewritten': n, 'excluded_sites': skipped, 'seconds': round(dt, 1), 'mismatch': mism,
                                  'rc': res.returncode, 'other_warnings': other[:5]})
            print(f'chunk {chunk} round {r}: rc {res.returncode} sites {n} (excluded {skipped}) {dt:.0f} s mismatches {mism}',
                  flush=True)
            if res.returncode:
                print(res.stderr[-2000:]); rec['error'] = res.stderr[-2000:]
                break
            if not mism:
                break
            exclude |= set(mism)
        rec['excluded_functions'] = sorted(exclude)
        rec['final_mismatch'] = rec['rounds'][-1]['mismatch'] if rec['rounds'][-1]['rc'] == 0 else None
        log[str(chunk)] = rec
        log_path.write_text(json.dumps(log, indent=1), encoding='utf-8', newline='\n')
    tot = sum(v['rounds'][-1]['rewritten'] for v in log.values())
    exc = sum(v['rounds'][-1]['excluded_sites'] for v in log.values())
    bad = [k for k, v in log.items() if v.get('final_mismatch') != []]
    print(f'{len(log)} chunks: {tot} sites rewritten, {exc} excluded; not clean: {bad}')


if __name__ == '__main__':
    main()
