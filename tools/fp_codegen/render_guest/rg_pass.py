#!/usr/bin/env python3
"""rg_pass.py IN.cpp OUT.cpp --sites-file SITES.json [--sites A,B,...]: render-guest insertion pass for one generated chunk
(v441 base sources).
Inserts one RECOMP_RG_* statement (rg_prefetch.h) per site of SITES into its guest function, after an anchor that must
occur exactly once in that function: either the generated statement right after a given PPC comment (e.g. the
`ctx.r29.u64 = ctx.r4.u64;` of `// mr r29,r4`), or a loop-head label line; and `#include "rg_prefetch.h" // rg_pass.py`
after the chunk's first line (`#include "ppc_recomp_shared.h"`). Every other line is copied unchanged. Never overwrites OUT.
Also importable: load_sites(path), then insert(lines_of_one_function_body, func, sites) for the host differential test.
No game code here. Promoted 2026-10-04 (package repro-recipe) from the render-guest agent's worktree; the only change: the
site table and its groups (guest functions, anchors, statements: game-derived) moved out of this source into SITES.json
{"sites": {id: [function, kind, anchor, statement]}, "groups": {name: [ids]}}, kept under the ignored local-analysis/."""
import json, re, sys
from pathlib import Path

# site id -> (guest function, anchor kind, anchor, statement); filled by load_sites() from the private sites file
#   kind 'after-stmt': the anchor is a PPC comment line; the statement goes after the next (generated) line
#   kind 'after-label': the anchor is a label line
SITES = {}
GROUPS = {}  # group name -> site ids (each site has its own run-time word: rg_cfg.h)


def load_sites(path):
    data = json.loads(Path(path).read_text())
    SITES.clear(); GROUPS.clear()
    SITES.update({k: tuple(v) for k, v in data['sites'].items()})
    GROUPS.update({k: tuple(v) for k, v in data.get('groups', {}).items()})
    return SITES
INCLUDE = '#include "ppc_recomp_shared.h"'
HEADER = '#include "rg_prefetch.h" // rg_pass.py\n'
IMPL = re.compile(r'^PPC_FUNC_IMPL\((__imp__sub_[0-9A-F]{8})\) \{')


def expand(sites):
    if sites is None:
        return set(SITES)
    out = set()
    for s in sites:
        out |= set(GROUPS.get(s, (s,)))
    return out


def insert(body, func, sites=None):
    """body: the lines of one function. Returns (new lines, inserted site ids)."""
    want = {k: v for k, v in SITES.items() if v[0] == func and k in expand(sites)}
    out, done = [], []
    pending = None
    for line in body:
        out.append(line)
        if pending is not None:
            if line.lstrip().startswith('//') or line.strip().endswith(':'):
                raise SystemExit(f'{func}: no generated statement after the anchor of {pending}')
            out.append('\t' + want[pending][3] + ' // rg_pass.py\n'); done.append(pending); pending = None
            continue
        for k, (f, kind, anchor, stmt) in want.items():
            if line.rstrip('\n') == anchor:
                if k in done or pending == k:
                    raise SystemExit(f'{func}: anchor {anchor!r} twice')
                if kind == 'after-label':
                    out.append('\t' + stmt + ' // rg_pass.py\n'); done.append(k)
                else:
                    pending = k
    return out, done


POSCTL = 'if (__atomic_load_n(&recomp_rg_cfg.ps, __ATOMIC_RELAXED) == 7u) { __asm__ __volatile__("" ::: "memory"); }'


def main():
    a = sys.argv[1:]
    if '--sites-file' not in a:
        raise SystemExit('--sites-file SITES.json is required (the site table is private game-derived data)')
    load_sites(a[a.index('--sites-file') + 1])
    del a[a.index('--sites-file'):a.index('--sites-file') + 2]
    if '--positive-control' in a:  # PGO check's positive control: a real branch per site must be reported as a mismatch
        a.remove('--positive-control')
        for k, v in list(SITES.items()):
            SITES[k] = v[:3] + (POSCTL,)
    sites = a[a.index('--sites') + 1].split(',') if '--sites' in a else None
    src, dst = Path(a[0]), Path(a[1])
    if dst.exists():
        raise SystemExit(f'{dst} exists')
    lines = src.read_text(errors='strict').splitlines(keepends=True)
    if lines[0].strip() != INCLUDE:
        raise SystemExit('unexpected first line')
    want = expand(sites)
    funcs = {v[0] for k, v in SITES.items() if k in want}
    out, done = [lines[0], HEADER], []
    i = 1
    while i < len(lines):
        m = IMPL.match(lines[i])
        if m and m.group(1) in funcs:
            j = i + 1
            while j < len(lines) and not lines[j].startswith('PPC_FUNC_IMPL') and not lines[j].startswith('PPC_WEAK_FUNC'):
                j += 1
            new, d = insert(lines[i:j], m.group(1), sites)
            out += new; done += d; i = j
            continue
        out.append(lines[i]); i += 1
    missing = sorted(want - set(done))
    if missing:
        raise SystemExit(f'sites not inserted: {missing}')
    dst.write_text(''.join(out), encoding='utf-8', newline='\n')
    print(f'{src.name}: inserted {",".join(sorted(done))} -> {dst}')


if __name__ == '__main__':
    main()
