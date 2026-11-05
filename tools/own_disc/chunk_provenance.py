#!/usr/bin/env python3
"""Provenance of every generated PPC chunk in an attempt (own-disc design study; read-only).

For attempt vN this classifies each of the 374 chunks by comparing three SHA-256 digests:
  F = the raw generator output (local-analysis/regen/flags-off/ppc: XenonRecomp + configs, no repairs)
  V = the validated corpus (android-private-trace-window/api26/validated-ppc: F + post-generation repairs)
  E = the chunk the attempt actually compiles (iterate.py rules: attempt.json "source" pin, else vN/src copy,
      else, with ppc_chunks == "all", the validated corpus)
and walks the attempt lineage ("from" chain) to find the attempts in which the effective chunk changed,
using (size, mtime) of attempt src copies (iterate.py and the dedupe tools preserve mtimes; an edit changes it).

Output: JSON with chunk numbers, class, digests (prefixes) and the attempts that changed the chunk, plus
their goal lines. No source text is copied. Usage:
  chunk_provenance.py v447 OUTPUT.json
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]            # the checkout this tool lies in
LOCAL = ROOT / 'local-analysis'
ATTEMPTS = LOCAL / 'attempts'
CORPUS = LOCAL / 'android-private-trace-window/api26/validated-ppc'
RAW = LOCAL / 'regen/flags-off/ppc'
CHUNK = re.compile(r'ppc_recomp\.(\d+)\.cpp')


def sha(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def effective(name: str) -> dict[int, tuple[str, Path]]:
    """chunk -> (kind, path) where kind is 'V' (validated corpus) or 'S' (attempt src copy)."""
    a = ATTEMPTS / name
    spec = json.loads((a / 'attempt.json').read_text())
    out: dict[int, tuple[str, Path]] = {}
    for f in spec['files']:
        if 'ppc_chunk' not in f:
            continue
        n = int(f['ppc_chunk'])
        if f.get('source'):
            p = ROOT / f['source']
            out[n] = ('V' if p.parent.resolve() == CORPUS.resolve() else 'X', p)
        else:
            out[n] = ('S', a / 'src' / f['name'])
    if spec.get('ppc_chunks') == 'all':
        for p in CORPUS.glob('ppc_recomp.*.cpp'):
            m = CHUNK.fullmatch(p.name)
            if m and int(m.group(1)) not in out:
                out[int(m.group(1))] = ('V', p)
    else:  # unlisted chunks come from the base link (the strict corpus objects built from validated-ppc)
        for p in CORPUS.glob('ppc_recomp.*.cpp'):
            m = CHUNK.fullmatch(p.name)
            if m and int(m.group(1)) not in out:
                out[int(m.group(1))] = ('V', p)
    return out


def state(kind: str, path: Path):
    if kind == 'V':
        return 'V'
    if not path.exists():
        return ('missing', str(path))
    st = path.stat()
    return (st.st_size, st.st_mtime_ns)


def main() -> int:
    name, output = sys.argv[1], Path(sys.argv[2])
    chain = []
    n = name
    while n:
        p = ATTEMPTS / n / 'attempt.json'
        if not p.exists():
            break
        spec = json.loads(p.read_text())
        chain.append((n, spec.get('goal', '')))
        n = spec.get('from')
    chain.reverse()  # oldest first
    goals = dict(chain)
    eff = effective(name)
    raw_digest = {int(CHUNK.fullmatch(p.name).group(1)): sha(p) for p in RAW.glob('ppc_recomp.*.cpp') if CHUNK.fullmatch(p.name)}
    val_digest = {int(CHUNK.fullmatch(p.name).group(1)): sha(p) for p in CORPUS.glob('ppc_recomp.*.cpp') if CHUNK.fullmatch(p.name)}
    # lineage: effective state per attempt
    history: dict[int, list[str]] = {c: [] for c in eff}
    previous: dict[int, object] = {}
    for attempt, _ in chain:
        try:
            e = effective(attempt)
        except (KeyError, json.JSONDecodeError, FileNotFoundError):
            continue
        for c in eff:
            kind, path = e.get(c, ('V', CORPUS / f'ppc_recomp.{c}.cpp'))
            s = state(kind, path)
            if c in previous and previous[c] != s:
                history[c].append(attempt)
            elif c not in previous and s != 'V':
                history[c].append(attempt)
            previous[c] = s
    rows = []
    for c in sorted(eff):
        kind, path = eff[c]
        e = sha(path) if kind != 'V' else val_digest[c]
        v, f = val_digest.get(c), raw_digest.get(c)
        if e == f:
            cls = 'generated'          # byte-identical to the raw XenonRecomp output
        elif e == v:
            cls = 'corpus-repair'      # validated corpus = generated + post-generation repairs
        else:
            cls = 'attempt-override'   # differs from the validated corpus
        rows.append(dict(chunk=c, cls=cls, kind=kind, effective=e[:12], validated=(v or '')[:12], raw=(f or '')[:12],
                         validated_is_raw=(v == f), changed_in=history[c]))
    summary = {}
    for r in rows:
        summary[r['cls']] = summary.get(r['cls'], 0) + 1
    changing = sorted({a for r in rows for a in r['changed_in']}, key=lambda s: int(s[1:]))
    output.write_text(json.dumps(dict(attempt=name, chain_length=len(chain), summary=summary,
                                      validated_vs_raw=dict(same=sum(1 for r in rows if r['validated_is_raw']),
                                                            differ=sum(1 for r in rows if not r['validated_is_raw'])),
                                      rows=rows,
                                      changing_attempts={a: goals.get(a, '')[:400] for a in changing}), indent=1) + '\n')
    print(json.dumps(summary), 'changing attempts:', len(changing))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
