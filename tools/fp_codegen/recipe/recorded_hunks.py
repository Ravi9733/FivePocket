#!/usr/bin/env python3
"""recorded_hunks.py IN.cpp OUT.cpp RECORDS.json KEY: replay recorded one-off edits exactly (design gap G4, last resort).
   recorded_hunks.py publish RECORDS.json AFTER_DIR OUT.json: the same records without any line the generator wrote.

For corpus repairs that no structured pass expresses (hand diagnostics, native hooks, one-of-a-kind case recoveries), the
smallest recorded patch: per chunk a list of hunks, each anchored in one generated function and verified before it is
applied,
    {"function": F | "HEADER", "offset": N, "old": [line, ...], "new": [line, ...]}
F = the guest address X of PPC_FUNC_IMPL(__imp__sub_X); N = line offset of the hunk from that header line ("HEADER":
from the start of the file, for the lines before the first function); old/new = the exact lines without their newline.
Hunks of one function are given in increasing offset order against the text as it is before this pass; the pass refuses
(exit 1) when an old range does not match byte for byte, so it can never apply to drifted input. The records contain
generated code (and the hand edits), so they are private and stay under local-analysis/; this tool contains none.
derive(base_text, target_text) computes the records with difflib per function (used by g4_derive.py).

Records without generated text (schema gta5-recorded-hunks/2): the form that may leave the private folder. A hunk is
    {"function": F | "HEADER", "offset": N, "old": {"count": C, "sha256": H}, "new": [ITEM, ...]}
H = the SHA-256 of the C old lines with their line feeds: the pass still refuses drifted input, without holding the
lines. ITEM is one of
    "text"                                   one line this project wrote (a hook, a guard, a dispatch line built from
                                             addresses, a comment in prose);
    {"copy": [F2, K, COUNT]}                 COUNT lines of the text as it is before this pass, from line offset K of
                                             function F2 ("HEADER": of the file), unchanged;
    {"copy": [F2, K, COUNT], "indent": D}    the same with D tabs added in front of every line (D < 0: taken away);
    {"copy": [F2, K, COUNT], "indent": D, "trim": true}   and the blanks at the end of every line taken away;
    {"copy": [F2, K, COUNT], "lead": "    "} the same with the blanks in front of every line replaced by the given ones
                                             (hand-written repairs indented with spaces);
    {"copy": [F2, K, COUNT], "squeeze": T}   the same with every blank inside the line removed and T tabs in front (the
                                             corpus's condensed spelling of the generator's own statements).
So a line the generator wrote is never in such a file: it is taken from the generated text of the run. publish() makes
these records from the old kind and the chunk text before the pass, and refuses a text line that is a generated line in
another spelling (one that none of the copies gives). `publish RECORDS.json AFTER_DIR OUT.json` does it for every
chunk of RECORDS.json from AFTER_DIR/ppc_recomp.KEY.cpp, the chunk as it is AFTER the pass (the text before it is
recovered with invert()), proves that the new records give that text byte for byte, and prints how many lines are
copied and how many are text. Schema 1 and schema 2 hunks are both replayed by the same call.
"""
from __future__ import annotations

import difflib
import hashlib
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import textpass  # noqa: E402

FUNC = re.compile(r'PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\) \{\n')


def segments(lines: list[str]) -> dict[str, int]:
    """function key -> index of its first line ("HEADER" -> 0); keys in file order"""
    out = {'HEADER': 0}
    for k, line in enumerate(lines):
        m = FUNC.fullmatch(line)
        if m:
            if m.group(1) in out:
                raise SystemExit(f'duplicate function {m.group(1)}')
            out[m.group(1)] = k
    return out


def bounds(lines: list[str]) -> dict[str, tuple[int, int]]:
    seg = segments(lines)
    keys = list(seg)
    return {k: (seg[k], seg[keys[n + 1]] if n + 1 < len(keys) else len(lines)) for n, k in enumerate(keys)}


def name(function) -> str:
    return function if function == 'HEADER' else f'{int(function, 16):08X}'


def digest(lines: list[str]) -> str:
    return hashlib.sha256(''.join(lines).encode('utf-8')).hexdigest()


def squeezed(line: str) -> str:
    """a line without its blanks (and without its line feed)"""
    return re.sub(r'[ \t]+', '', line.rstrip('\n'))


def copied(lines: list[str], b: dict, item: dict, where: str) -> list[str]:
    """the lines one copy item stands for, taken from the text before the pass"""
    extra = set(item) - {'copy', 'indent', 'squeeze', 'trim', 'lead'}
    if extra or sum(k in item for k in ('indent', 'squeeze', 'lead')) > 1 or ('trim' in item and ('squeeze' in item or 'lead' in item)) \
            or item.get('trim', True) is not True or not isinstance(item.get('copy'), list) or len(item['copy']) != 3:
        raise SystemExit(f'{where}: not a copy item')
    source, k, count = item['copy']
    key = name(source)
    if key not in b:
        raise SystemExit(f'{where}: copies from {key}, which is not in this chunk')
    s, e = b[key]
    if type(k) is not int or type(count) is not int or count < 1 or not s <= s + k <= s + k + count <= e:
        raise SystemExit(f'{where}: copies lines outside {key}')
    got = lines[s + k:s + k + count]
    if 'lead' in item:
        if not isinstance(item['lead'], str) or item['lead'].strip(' \t'):
            raise SystemExit(f'{where}: not a copy item')
        return [item['lead'] + x.lstrip(' \t') for x in got]
    if 'squeeze' in item:
        tabs = item['squeeze']
        if type(tabs) is not int or tabs < 0:
            raise SystemExit(f'{where}: not a copy item')
        return ['\t' * tabs + squeezed(x) + '\n' for x in got]
    d = item.get('indent', 0)
    if type(d) is not int:
        raise SystemExit(f'{where}: not a copy item')
    if d < 0 and any(not x.startswith('\t' * -d) for x in got):
        raise SystemExit(f'{where}: a copied line has fewer tabs than are taken away')
    got = ['\t' * d + x for x in got] if d >= 0 else [x[-d:] for x in got]
    return [x.rstrip(' \t\n') + '\n' for x in got] if item.get('trim') else got


def apply(text: str, hunks: list[dict]) -> tuple[str, dict]:
    lines = text.splitlines(keepends=True)
    b = bounds(lines)
    edits = []
    for h in hunks:
        key = name(h['function'])
        if key not in b:
            raise SystemExit(f'{key}: not in this chunk')
        s, e = b[key]
        a = s + int(h['offset'])
        where = f'{key}+{h["offset"]}'
        if isinstance(h['old'], dict):              # schema 2: the old lines by their number and hash
            n = h['old'].get('count')
            if type(n) is not int or not s <= a <= a + n <= e or digest(lines[a:a + n]) != h['old'].get('sha256'):
                raise SystemExit(f'{where}: recorded old lines do not match')
        else:
            old = [x + '\n' for x in h['old']]
            n = len(old)
            if not s <= a <= a + n <= e or lines[a:a + n] != old:
                raise SystemExit(f'{where}: recorded old lines do not match')
        new = []
        for item in h['new']:
            new += [item + '\n'] if isinstance(item, str) else copied(lines, b, item, where)
        edits.append((a, a + n, new))
    edits.sort()
    for (a0, a1, _), (b0, _b1, _) in zip(edits, edits[1:]):
        if b0 < a1 or (b0 == a0):
            raise SystemExit('overlapping recorded hunks')
    out, idx = [], 0
    for a0, a1, new in edits:
        out += lines[idx:a0] + new
        idx = a1
    out += lines[idx:]
    return ''.join(out), {'hunks': len(edits)}


def derive(base: str, target: str) -> list[dict]:
    """the hunks that turn base into target, function by function (both must define the same functions in order)"""
    bl, tl = base.splitlines(keepends=True), target.splitlines(keepends=True)
    bb, tb = bounds(bl), bounds(tl)
    if list(bb) != list(tb):
        raise SystemExit('the two texts define different functions')
    hunks = []
    for key, (s, e) in bb.items():
        ts, te = tb[key]
        if bl[s:e] == tl[ts:te]:
            continue
        sm = difflib.SequenceMatcher(None, bl[s:e], tl[ts:te], autojunk=False)
        for op, i1, i2, j1, j2 in sm.get_opcodes():
            if op == 'equal':
                continue
            if any(not x.endswith('\n') for x in bl[s + i1:s + i2] + tl[ts + j1:ts + j2]):
                raise SystemExit(f'{key}: a hunk touches a line without newline (end of file)')
            hunks.append({'function': key, 'offset': i1, 'old': [x[:-1] for x in bl[s + i1:s + i2]],
                          'new': [x[:-1] for x in tl[ts + j1:ts + j2]]})
    return hunks


def invert(after: str, hunks: list[dict]) -> str:
    """the text before the pass, from the text after it and schema 1 hunks (the inverse of apply for those)"""
    lines = after.splitlines(keepends=True)
    b = bounds(lines)
    edits, grown = [], {}
    for h in hunks:
        key = name(h['function'])
        if key not in b or isinstance(h['old'], dict) or any(not isinstance(x, str) for x in h['new']):
            raise SystemExit(f'{key}: not in this chunk, or not a schema 1 hunk')
        s, e = b[key]
        a = s + int(h['offset']) + grown.get(key, 0)      # hunks of one function come in increasing offset order
        new = [x + '\n' for x in h['new']]
        if not s <= a <= a + len(new) <= e or lines[a:a + len(new)] != new:
            raise SystemExit(f'{key}+{h["offset"]}: recorded new lines are not in the text after the pass')
        edits.append((a, a + len(new), [x + '\n' for x in h['old']]))
        grown[key] = grown.get(key, 0) + len(new) - len(h['old'])
    edits.sort()
    out, idx = [], 0
    for a0, a1, old in edits:
        out += lines[idx:a0] + old
        idx = a1
    return ''.join(out + lines[idx:])


def publish(base: str, hunks: list[dict]) -> tuple[list[dict], dict]:
    """schema 2 hunks for schema 1 hunks and the text before the pass: every new line that the text before the pass
    holds (as it is, with other tabs in front, or without its blanks) becomes a copy; what is left is this project's
    text. Refuses a text line that is a line of the text before the pass in yet another spelling."""
    lines = base.splitlines(keepends=True)
    b = bounds(lines)
    owner = {}
    for key, (s, e) in b.items():
        for k in range(s, e):
            owner[k] = key
    exact, bare, flush, cut, tight = {}, {}, {}, {}, {}
    for k, line in enumerate(lines):
        exact.setdefault(line, []).append(k)
        bare.setdefault(line.lstrip('\t'), []).append(k)
        flush.setdefault(line.lstrip(' \t'), []).append(k)
        if line.rstrip(' \t\n') + '\n' != line:
            cut.setdefault(line.strip(' \t\n'), []).append(k)
        tight.setdefault(squeezed(line), []).append(k)
    stats = {'hunks': 0, 'old lines': 0, 'copied': 0, 'copied with other tabs': 0, 'copied with other blanks in front': 0,
             'copied without end blanks': 0, 'copied without blanks': 0, 'text': 0}
    out = []
    for h in hunks:
        key = name(h['function'])
        own = b.get(key, (0, 0))
        items, run = [], None                       # run = (mode, parameter, function, last position) of the open copy
        for text in h['new']:
            line = text + '\n'
            tabs = len(line) - len(line.lstrip('\t'))
            found = [('copied', 0, k) for k in exact.get(line, [])]
            found += [('copied with other tabs', tabs - (len(lines[k]) - len(lines[k].lstrip('\t'))), k) for k in bare.get(line.lstrip('\t'), []) if lines[k] != line]
            lead = line[:len(line) - len(line.lstrip(' \t'))]
            if lead.strip('\t'):                    # blanks in front that are not tabs only
                found += [('copied with other blanks in front', lead, k) for k in flush.get(line.lstrip(' \t'), []) if lines[k] != line]
            if line.rstrip(' \t\n') + '\n' == line:
                found += [('copied without end blanks', tabs - (len(lines[k]) - len(lines[k].lstrip('\t'))), k) for k in cut.get(line.strip('\t\n'), [])]
            if line == '\t' * tabs + squeezed(line) + '\n':
                found += [('copied without blanks', tabs, k) for k in tight.get(squeezed(line), [])
                          if lines[k].lstrip('\t') != line.lstrip('\t') and lines[k].strip(' \t\n') != line.strip('\t\n')]
            if not found:
                if squeezed(line) in tight:
                    raise SystemExit(f'{key}+{h["offset"]}: a new line is a line of the text before the pass in a spelling no copy gives')
                items.append(text); run = None; stats['text'] += 1
                continue
            order = {'copied': 0, 'copied with other tabs': 1, 'copied with other blanks in front': 2, 'copied without end blanks': 3, 'copied without blanks': 4}
            go_on = [f for f in found if run and (f[0], f[1], owner[f[2]], f[2]) == (run[0], run[1], run[2], run[3] + 1)]
            mode, parameter, k = go_on[0] if go_on else min(found, key=lambda f: (order[f[0]], not own[0] <= f[2] < own[1], f[2]))
            stats[mode] += 1
            if go_on:
                items[-1]['copy'][2] += 1
            else:
                item = {'copy': [owner[k], k - b[owner[k]][0], 1]}
                if mode == 'copied with other tabs':
                    item['indent'] = parameter
                elif mode == 'copied with other blanks in front':
                    item['lead'] = parameter
                elif mode == 'copied without end blanks':
                    item.update(indent=parameter, trim=True)
                elif mode == 'copied without blanks':
                    item['squeeze'] = parameter
                items.append(item)
            run = (mode, parameter, owner[k], k)
        old = [x + '\n' for x in h['old']]
        out.append({'function': h['function'], 'offset': h['offset'], 'old': {'count': len(old), 'sha256': digest(old)}, 'new': items})
        stats['hunks'] += 1; stats['old lines'] += len(old)
    return out, stats


def publish_file(records: Path, after_dir: Path, out: Path) -> int:
    rec = json.loads(records.read_text(encoding='utf-8'))
    if rec.get('schema') != 'gta5-recorded-hunks/1':
        raise SystemExit('publish reads schema gta5-recorded-hunks/1')
    chunks, total = {}, {}
    for key, hunks in rec['chunks'].items():
        after = textpass.read_text(after_dir / f'ppc_recomp.{key}.cpp')
        base = invert(after, hunks)
        if apply(base, hunks)[0] != after:
            raise SystemExit(f'chunk {key}: the old records do not give the text after the pass')
        chunks[key], stats = publish(base, hunks)
        if apply(base, chunks[key])[0] != after:
            raise SystemExit(f'chunk {key}: the new records do not give the text after the pass')
        for k, v in stats.items():
            total[k] = total.get(k, 0) + v
    textpass.write_new(out, json.dumps({'schema': 'gta5-recorded-hunks/2',
                                        'note': 'exact one-off edits of generated code without any generated line: copies name places in the run\'s own text (recorded_hunks.py)',
                                        'chunks': chunks}, indent=1) + '\n')
    print(f'{out.name}: {len(chunks)} chunks {total}')
    return 0


def main() -> int:
    if len(sys.argv) == 5 and sys.argv[1] == 'publish':
        return publish_file(Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4]))
    if len(sys.argv) != 5:
        print(__doc__)
        return 2
    src, dst, params, key = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4]
    hunks = json.loads(params.read_text())['chunks'][key]
    text, stats = apply(textpass.read_text(src), hunks)
    textpass.write_new(dst, text)
    print(f'{src.name}: {stats}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
