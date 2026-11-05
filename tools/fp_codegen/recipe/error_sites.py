#!/usr/bin/env python3
"""error_sites.py: the corpus's leaf/tail repairs of the generator's `// ERROR X` branch sites (design gap G4).

  error_sites.py IN.cpp OUT.cpp RECORDS.json KEY      apply the records of RECORDS.json["chunks"][KEY]
  error_sites.py derive GEN_DIR CODE_DIR OUT.json [--chunks N,M,... | @FILE]
  error_sites.py publish RECORDS.json BEFORE_DIR OUT.json    the same records without any line the generator wrote

XenonRecomp translates a branch to an address outside the function that is no known function entry as
`// ERROR X` + `return;` (inside the `if (cond) {` block for a conditional branch). The validated corpus replaced such a
line, at chosen sites, by an inline copy of the original code at X, keeping the generator's `return;`:
    <indent>// Original constant leaf at 0xX: li rN,V; blr.      + the generator's translation of that li
    <indent>// Original verified leaf at 0xX, followed by blr.  + hand-condensed translation lines
    <indent>// Original verified tail at 0xX, ending with blr.  (or "... ending with guarded bctr.")
    <indent>// Original register/store leaf at 0xX, followed by blr.
Records (private, under local-analysis/):
    {"bodies": {X: {"comment": C, "lines": [L, ...]} | {"constant": true}},
     "sites": [{"function": F, "branch": A, "target": X}]}
A = the guest address of the branch whose translation holds the ERROR line. A "constant" body stores nothing: the pass
takes `// li rN,V` and its one translation line from the generator output of the same chunk (the function whose
instruction comments contain X) and renders the comment from it. Other bodies are recorded text (the corpus's own
condensed translation, which differs from the generator's form), indentation-free, and are re-indented to the ERROR line.
Exit 1 when a site does not hold exactly `// ERROR X`. Contains no game code or game-derived data.

Records without generated text (schema gta5-error-sites/2), the form that may leave the private folder: a body line is
either text this project wrote, or a copy item of recorded_hunks.py ({"copy": [F, K, COUNT], "lead": BLANKS}: COUNT
lines of the text as it is before this pass, from line offset K of function F, with the blanks in front replaced by
BLANKS; the pass then indents them to the ERROR line like every body line). publish makes them for every chunk of
RECORDS.json from BEFORE_DIR/ppc_recomp.KEY.cpp (the chunk as it is before this pass): a body line the text before the
pass holds becomes a copy; one that is such a line in another spelling is refused; the rest is this project's text
(the corpus's hand-written condensed lines). It proves that old and new records give the same text and prints the
counts.
"""
from __future__ import annotations

import argparse
import difflib
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import recorded_hunks  # noqa: E402
import textpass  # noqa: E402
from g4text import Index, positions  # noqa: E402

ERROR = re.compile(r'(\t+)// ERROR ([0-9A-F]+)\n')
LI = re.compile(r'\t// li r(\d+),(-?\d+)\n')
ORIGINAL = re.compile(r'(\t+)// Original (constant leaf|verified leaf|verified tail|register/store leaf) at 0x([0-9A-F]+)'
                      r'(: li r\d+,-?\d+; blr\.|, followed by blr\.|, ending with blr\.|, ending with guarded bctr\.)\n')


def constant_body(idx: Index, x: int) -> tuple[str, list[str]]:
    name, pos = idx.find(x)
    k = pos[x]
    m = LI.fullmatch(idx.lines[k])
    if not m or idx.lines[k + 2] != '\t// blr \n' or pos.get(x + 4) != k + 2:
        raise SystemExit(f'{x:08X}: the generator does not translate li + blr there')
    return f'// Original constant leaf at 0x{x:X}: li r{m.group(1)},{m.group(2)}; blr.', [idx.lines[k + 1][1:-1]]


def body_lines(lines: list[str], bounds: dict, body: dict, where: str) -> list[str]:
    """the lines of a recorded body, without indentation and with their line feeds: text as it is, copies taken from
    the text before the pass"""
    out = []
    for item in body['lines']:
        out += [item + '\n'] if isinstance(item, str) else recorded_hunks.copied(lines, bounds, item, where)
    return out


def apply(text: str, rec: dict) -> tuple[str, dict]:
    lines = text.splitlines(keepends=True)
    idx = Index(lines)
    bounds = recorded_hunks.bounds(lines)
    repl: dict[int, list[str]] = {}
    stats = {'sites': 0, 'constant': 0, 'recorded': 0}
    for s in rec['sites']:
        name = f'{int(s["function"], 16):08X}'
        if name not in idx.fn:
            raise SystemExit(f'sub_{name}: not in this chunk')
        i, j = idx.fn[name]
        pos = idx.positions(name)
        a, x = int(s['branch'], 16), int(s['target'], 16)
        k = pos.get(a)
        if k is None:
            raise SystemExit(f'sub_{name}: no instruction at {a:08X}')
        e = k + 1
        while e < j and not ERROR.fullmatch(lines[e]) and not lines[e].startswith('\t// '):
            e += 1
        m = ERROR.fullmatch(lines[e]) if e < j else None
        if not m or int(m.group(2), 16) != x or e in repl:
            raise SystemExit(f'sub_{name}: no free `// ERROR {x:X}` in the translation of {a:08X}')
        body = rec['bodies'][f'{x:08X}']
        if body.get('constant'):
            comment, blines = constant_body(idx, x)
            blines = [ln + '\n' for ln in blines]
            stats['constant'] += 1
        else:
            comment, blines = body['comment'], body_lines(lines, bounds, body, f'{x:08X}')
            stats['recorded'] += 1
        ind = m.group(1)
        repl[e] = [ind + comment + '\n'] + [ind + ln for ln in blines]
        stats['sites'] += 1
    out = []
    for n, line in enumerate(lines):
        out += repl.get(n, [line])
    return ''.join(out), stats


# ------------------------------------------------------------------------------------------------ publish

def publish(text: str, rec: dict) -> tuple[dict, dict]:
    """the records of one chunk without generated text, for the old records and the text before the pass"""
    lines = text.splitlines(keepends=True)
    b = recorded_hunks.bounds(lines)
    owner = {k: key for key, (s, e) in b.items() for k in range(s, e)}
    flush, tight = {}, set()
    for k, line in enumerate(lines):
        flush.setdefault(line.lstrip(' \t'), []).append(k)
        tight.add(recorded_hunks.squeezed(line))
    stats = {'bodies': 0, 'copied': 0, 'text': 0}
    bodies = {}
    for x, body in rec['bodies'].items():
        if 'lines' not in body:
            bodies[x] = dict(body)
            continue
        items, run = [], None                       # run = (lead, function, last position) of the open copy
        for ln in body['lines']:
            if not isinstance(ln, str):
                raise SystemExit(f'{x}: publish reads records whose body lines are text')
            line = ln + '\n'
            lead = line[:len(line) - len(line.lstrip(' \t'))]
            found = flush.get(line.lstrip(' \t'), [])
            if not found:
                if recorded_hunks.squeezed(line) in tight:
                    raise SystemExit(f'{x}: a body line is a line of the text before the pass in a spelling no copy gives')
                items.append(ln); run = None; stats['text'] += 1
                continue
            go_on = [k for k in found if run and (lead, owner[k], k) == (run[0], run[1], run[2] + 1)]
            k = go_on[0] if go_on else found[0]
            if go_on:
                items[-1]['copy'][2] += 1
            else:
                items.append({'copy': [owner[k], k - b[owner[k]][0], 1], 'lead': lead})
            run = (lead, owner[k], k); stats['copied'] += 1
        bodies[x] = {'comment': body['comment'], 'lines': items}
        stats['bodies'] += 1
    return {'bodies': bodies, 'sites': rec['sites']}, stats


def publish_file(records: Path, before_dir: Path, out: Path) -> int:
    rec = json.loads(records.read_text(encoding='utf-8'))
    if rec.get('schema') != 'gta5-error-sites/1':
        raise SystemExit('publish reads schema gta5-error-sites/1')
    chunks, total = {}, {}
    for key, old in rec['chunks'].items():
        text = textpass.read_text(before_dir / f'ppc_recomp.{key}.cpp')
        chunks[key], stats = publish(text, old)
        if apply(text, chunks[key]) != apply(text, old):
            raise SystemExit(f'chunk {key}: the new records do not give the text the old ones give')
        for k, v in stats.items():
            total[k] = total.get(k, 0) + v
    textpass.write_new(out, json.dumps({'schema': 'gta5-error-sites/2',
                                        'note': 'guest addresses of original leaves and tails; their lines are copies from the run\'s own text or this project\'s hand-written text (error_sites.py publish)',
                                        'chunks': chunks, 'unparsed': rec.get('unparsed', [])}, indent=1) + '\n')
    print(f'{out.name}: {len(chunks)} chunks {total}')
    return 0


# ------------------------------------------------------------------------------------------------ derive

def derive(a) -> int:
    from g4text import segments  # noqa: E402
    gen, code = Path(a.gen), Path(a.code)
    spec = Path(a.chunks[1:]).read_text() if a.chunks and a.chunks.startswith('@') else (a.chunks or '')
    chunks = [int(x) for x in re.split(r'[\s,]+', spec) if x]
    out, counts, odd = {}, {'sites': 0, 'constant': 0, 'recorded bodies': 0}, []
    for n in chunks:
        gt, ct = textpass.read_text(gen / f'ppc_recomp.{n}.cpp'), textpass.read_text(code / f'ppc_recomp.{n}.cpp')
        g, c = gt.splitlines(keepends=True), ct.splitlines(keepends=True)
        gs, cs = segments(g), segments(c)
        idx = Index(g)
        bodies, sites = {}, []
        for name, (gi, gj) in gs.items():
            ci, cj = cs[name]
            if g[gi:gj] == c[ci:cj] or not any(ERROR.fullmatch(x) for x in g[gi:gj]):
                continue
            pos = None
            sm = difflib.SequenceMatcher(None, g[gi:gj], c[ci:cj], autojunk=False)
            for op, i1, i2, j1, j2 in sm.get_opcodes():
                if op != 'replace' or i2 - i1 != 1:
                    continue
                em, om = ERROR.fullmatch(g[gi + i1]), ORIGINAL.fullmatch(c[ci + j1])
                if not em or not om or om.group(1) != em.group(1) or om.group(3) != em.group(2):
                    continue
                ind = em.group(1)
                blines = [x[len(ind):-1] for x in c[ci + j1 + 1:ci + j2]]
                if not all(x.startswith(ind) for x in c[ci + j1 + 1:ci + j2]):
                    odd.append(f'{n}:{name}:{em.group(2)}')
                    continue
                if pos is None:
                    pos = positions(g, gi, gj, int(name, 16))
                back = {v: k for k, v in pos.items()}
                k = gi + i1
                while k > gi and k not in back:
                    k -= 1
                x = em.group(2)
                comment = c[ci + j1][len(ind):-1]
                body = {'comment': comment, 'lines': blines}
                if om.group(2) == 'constant leaf':
                    try:
                        if constant_body(idx, int(x, 16)) == (comment, blines):
                            body = {'constant': True}
                    except SystemExit:
                        pass
                if bodies.get(x, body) != body:
                    odd.append(f'{n}:{name}:{x} (two bodies for one target)')
                    continue
                bodies[x] = body
                sites.append({'function': name, 'branch': f'{back[k]:08X}', 'target': x})
                counts['sites'] += 1
        if sites:
            out[str(n)] = {'bodies': bodies, 'sites': sites}
            counts['constant'] += sum(1 for b in bodies.values() if b.get('constant'))
            counts['recorded bodies'] += sum(1 for b in bodies.values() if not b.get('constant'))
    if Path(a.out).exists():
        raise SystemExit(f'{a.out} exists')
    Path(a.out).write_text(json.dumps({'schema': 'gta5-error-sites/1', 'note': 'private: guest addresses and recorded '
                                       'translation text of original leaves/tails (error_sites.py derive)',
                                       'chunks': out, 'unparsed': odd}, indent=1) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps(counts), f'{len(out)} chunks; unparsed {odd}')
    return 0


def main() -> int:
    if len(sys.argv) > 1 and sys.argv[1] == 'derive':
        ap = argparse.ArgumentParser()
        ap.add_argument('cmd'); ap.add_argument('gen'); ap.add_argument('code'); ap.add_argument('out')
        ap.add_argument('--chunks')
        return derive(ap.parse_args())
    if len(sys.argv) == 5 and sys.argv[1] == 'publish':
        return publish_file(Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4]))
    if len(sys.argv) != 5:
        print(__doc__)
        return 2
    src, dst, params, key = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4]
    rec = json.loads(params.read_text())['chunks'][key]
    text, stats = apply(textpass.read_text(src), rec)
    textpass.write_new(dst, text)
    print(f'{src.name}: {stats}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
