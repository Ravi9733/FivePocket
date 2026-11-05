#!/usr/bin/env python3
"""Aggregate the SHAPES of the differences between two generated-chunk trees (own-disc design study; read-only).

  diff_shapes.py OLD_DIR NEW_DIR OUTPUT.json [--chunks N,M,...]

For every ppc_recomp.N.cpp present in both trees and not byte-identical, runs `diff OLD NEW`, normalises each changed
line (guest addresses, hex and decimal numbers -> placeholders) and counts the normalised added/removed line shapes
overall and per chunk. Only normalised shapes and counts are written (no addresses, constants or code text of the game).
Use it to see which transform families explain a tree-to-tree difference. Runs `diff` one chunk at a time.
"""
from __future__ import annotations

import collections
import json
import re
import subprocess
import sys
from pathlib import Path

CHUNK = re.compile(r'ppc_recomp\.(\d+)\.cpp')
NORM = [(re.compile(r'\b(sub|loc|__imp__sub)_[0-9A-F]{8}\b'), r'\1_X'), (re.compile(r'0x[0-9A-Fa-f]+'), 'N'),
        (re.compile(r'\b\d+\b'), 'N'), (re.compile(r'\s+'), ' ')]


def norm(line: str) -> str:
    for rx, rep in NORM:
        line = rx.sub(rep, line)
    return line.strip()[:160]


def main() -> int:
    old, new, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    only = None
    if '--chunks' in sys.argv:
        only = {int(x) for x in sys.argv[sys.argv.index('--chunks') + 1].split(',') if x}
    # --family NAME=REGEX (repeatable): per chunk, count ADDED lines (raw text) matching REGEX -> chunk families
    families = [tuple(sys.argv[i + 1].split('=', 1)) for i, a in enumerate(sys.argv) if a == '--family']
    if '--default-families' in sys.argv:  # the repair / pass families seen in the GTA V corpus
        families += [('switch', r'ctx\.ctr\.u32 [!=]= 0x'), ('tail', r'Original verified tail|Rebuilt by|stitch'),
                     ('newfunc', r'^PPC_FUNC_IMPL\('), ('hook', r'native|Native|recomp_|RECOMP_'),
                     ('trapfix', r'debugtrap|ERROR'), ('leafret', r'^\s*ctx\.r3\.s64 = -?\d+;'),
                     ('fence', r'__atomic_thread_fence'), ('direct', r'^\s*__imp__sub_[0-9A-F]{8}\(ctx, base\);'),
                     ('vmx', r'ppc_native_|PPC_VMX_REV128'), ('prefetch', r'PPC_MC_DCBT|RECOMP_AP_|RECOMP_RG_'),
                     ('include', r'^#include ')]
    families = [(n, re.compile(rx)) for n, rx in families]
    added, removed = collections.Counter(), collections.Counter()
    per_chunk = {}
    for p in sorted(new.glob('ppc_recomp.*.cpp'), key=lambda q: int(CHUNK.fullmatch(q.name).group(1)) if CHUNK.fullmatch(q.name) else -1):
        m = CHUNK.fullmatch(p.name)
        if not m or (only is not None and int(m.group(1)) not in only):
            continue
        q = old / p.name
        if not q.exists():
            continue
        r = subprocess.run(['cmp', '-s', str(q), str(p)])
        if r.returncode == 0:
            continue
        d = subprocess.run(['diff', str(q), str(p)], capture_output=True, text=True, errors='replace').stdout
        a, rm = collections.Counter(), collections.Counter()
        fam = collections.Counter()
        for line in d.splitlines():
            if line.startswith('> '):
                a[norm(line[2:])] += 1
                for name, rx in families:
                    if rx.search(line[2:]):
                        fam[name] += 1
            elif line.startswith('< '):
                rm[norm(line[2:])] += 1
        added.update(a); removed.update(rm)
        per_chunk[int(m.group(1))] = dict(added=sum(a.values()), removed=sum(rm.values()), families=dict(fam),
                                          top_added=a.most_common(6), top_removed=rm.most_common(4))
    fam_chunks = collections.Counter()
    for v in per_chunk.values():
        fam_chunks[' + '.join(sorted(v['families'])) or '(none of the families)'] += 1
    out.write_text(json.dumps(dict(old=str(old), new=str(new), chunks_differing=len(per_chunk),
                                   family_combinations=fam_chunks.most_common(),
                                   added_total=sum(added.values()), removed_total=sum(removed.values()),
                                   top_added=added.most_common(80), top_removed=removed.most_common(60),
                                   per_chunk=per_chunk), indent=1) + '\n')
    print(len(per_chunk), 'chunks differ;', sum(added.values()), 'lines added,', sum(removed.values()), 'removed')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
