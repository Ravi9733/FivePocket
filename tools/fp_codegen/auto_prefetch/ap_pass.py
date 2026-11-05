#!/usr/bin/env python3
"""ap_pass.py IN.cpp OUT.cpp --sites-file SITES.json [--sites A,B,...]: auto-prefetch insertion pass for one generated chunk
(v436 base sources).
Inserts one RECOMP_AP_* statement (ap_prefetch.h) right after the loop-head label of each site of SITES that lives in the
chunk (a label line `loc_XXXXXXXX:` inside the named function, which must occur exactly once there), and
`#include "ap_prefetch.h" // ap_pass.py` after the chunk's first line (`#include "ppc_recomp_shared.h"`). Every other line
is copied unchanged. Prints the sites inserted. Never overwrites OUT.
Also importable: load_sites(path), then insert(lines_of_one_function_body, func) for the host differential test.
Promoted 2026-10-04 (package repro-recipe) from the auto-prefetch agent's worktree; the only change: the site table (guest
functions, labels and statements: game-derived) moved out of this source into SITES.json {"sites": {id: [function, label,
statement]}}, kept under the ignored local-analysis/."""
import json, re, sys
from pathlib import Path

# site id -> (guest function, loop-head label, statement); filled by load_sites() from the private sites file
SITES = {}


def load_sites(path):
    SITES.clear()
    SITES.update({k: tuple(v) for k, v in json.loads(Path(path).read_text())['sites'].items()})
    return SITES
INCLUDE = '#include "ppc_recomp_shared.h"'
HEADER = '#include "ap_prefetch.h" // ap_pass.py\n'
IMPL = re.compile(r'^PPC_FUNC_IMPL\((__imp__sub_[0-9A-F]{8})\) \{')


def insert(body, func, sites=None):
    """body: the lines of one function (any slice that contains the label lines). Returns (new lines, inserted ids)."""
    want = {k: v for k, v in SITES.items() if v[0] == func and (sites is None or k in sites)}
    out, done = [], []
    for line in body:
        out.append(line)
        for k, (f, label, stmt) in want.items():
            if line.rstrip('\n') == label + ':':
                if k in done:
                    raise SystemExit(f'{func}: label {label} twice')
                out.append('\t' + stmt + ' // ap_pass.py\n'); done.append(k)
    return out, done


def main():
    a = sys.argv[1:]
    if '--sites-file' not in a:
        raise SystemExit('--sites-file SITES.json is required (the site table is private game-derived data)')
    load_sites(a[a.index('--sites-file') + 1])
    del a[a.index('--sites-file'):a.index('--sites-file') + 2]
    sites = set(a[a.index('--sites') + 1].split(',')) if '--sites' in a else None
    src, dst = Path(a[0]), Path(a[1])
    if dst.exists():
        raise SystemExit(f'{dst} exists')
    lines = src.read_text(errors='strict').splitlines(keepends=True)
    if lines[0].strip() != INCLUDE:
        raise SystemExit('unexpected first line')
    funcs = {v[0] for k, v in SITES.items() if sites is None or k in sites}
    out, cur, done = [lines[0], HEADER], None, []
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
    want = [k for k, v in SITES.items() if v[0] in funcs]
    missing = [k for k in want if k not in done]
    if not done:
        raise SystemExit('no site in this chunk')
    dst.write_text(''.join(out), encoding='utf-8', newline='\n')
    print(f'{src.name}: inserted {",".join(done)}' + (f'; not in this chunk: {",".join(missing)}' if missing else '') + f' -> {dst}')


if __name__ == '__main__':
    main()
