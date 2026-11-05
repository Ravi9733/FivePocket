#!/usr/bin/env python3
"""anchored_insert.py: record an insert-only hand edit of a generated chunk as a replayable pass, and replay it.

  extract BEFORE.cpp AFTER.cpp INSERTS.json KEY [--tag T]
        AFTER must equal BEFORE plus inserted whole lines (no line removed or changed). Writes (or adds KEY to)
        INSERTS.json {"chunks": {KEY: {"tag": T, "inserts": [record, ...]}}}, one record per inserted block:
          {"scope": F | "chunk", "anchor": LINE, "occurrence": K, "count": C, "text": TEXT}
        = insert TEXT after the K-th (1-based) line equal to LINE inside the generated function F (guest address, or the
        full body name), or inside the whole chunk when the anchor line is outside every function; C is the number of
        such lines in the scope (checked on replay). The records are verified by replaying them on BEFORE. INSERTS.json
        holds generated-code context lines and the inserted text: it is game-derived and stays under local-analysis/.
  apply IN.cpp OUT.cpp INSERTS.json KEY
        replays the records of KEY (positions resolved on IN; several blocks after one anchor keep their order).
        Refused when a scope or anchor is missing or its count differs.

Function scope makes the records independent of line numbers, so earlier recipe passes that add or remove lines in other
functions do not move them. Contains no game code.
"""
from __future__ import annotations

import difflib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import textpass  # noqa: E402


def scopes(lines: list[str]) -> list[str]:
    """per line: the name of the generated function whose header..closing brace contains it, else 'chunk'"""
    owner = ['chunk'] * len(lines)
    for i, j, name in textpass.functions(lines):
        for k in range(i, min(j, len(lines) - 1) + 1):
            owner[k] = name
    return owner


def scope_lines(lines: list[str], owner: list[str], scope: str) -> list[int]:
    idx = [k for k, o in enumerate(owner) if o == scope]
    if scope != 'chunk':
        names = {o for o in owner}
        if scope not in names:
            raise SystemExit(f'scope {scope}: no such function')
        # a function name could repeat (PPC_FUNC_IMPL + PPC_FUNC wrappers); demand one contiguous body
        if idx and idx[-1] - idx[0] + 1 != len(idx):
            raise SystemExit(f'scope {scope}: not one contiguous body')
    return idx


def apply(text: str, records: list[dict]) -> str:
    lines = text.splitlines(keepends=True)
    owner = scopes(lines)
    after: dict[int, list[str]] = {}
    for r in records:
        idx = [k for k in scope_lines(lines, owner, r['scope']) if lines[k] == r['anchor']]
        if len(idx) != r['count']:
            raise SystemExit(f'scope {r["scope"]}: anchor {r["anchor"]!r} occurs {len(idx)} times, recorded {r["count"]}')
        k = idx[r['occurrence'] - 1]
        after.setdefault(k, []).append(r['text'])
    out = []
    for k, line in enumerate(lines):
        out.append(line)
        out += after.get(k, [])
    return ''.join(out)


def extract(before: str, after_text: str) -> list[dict]:
    a, b = before.splitlines(keepends=True), after_text.splitlines(keepends=True)
    owner = scopes(a)
    records = []
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, a, b, autojunk=False).get_opcodes():
        if tag == 'equal':
            continue
        if tag != 'insert':
            raise SystemExit(f'not insert-only: {tag} at line {i1 + 1}')
        if i1 == 0:
            raise SystemExit('insertion before the first line is not supported')
        anchor, scope = a[i1 - 1], owner[i1 - 1]
        same = [k for k in scope_lines(a, owner, scope) if a[k] == anchor]
        records.append(dict(scope=scope, anchor=anchor, occurrence=same.index(i1 - 1) + 1, count=len(same),
                            text=''.join(b[j1:j2])))
    if apply(before, records) != after_text:
        raise SystemExit('extracted records do not reproduce AFTER')
    return records


def main() -> int:
    a = sys.argv[1:]
    if len(a) >= 5 and a[0] == 'extract':
        before, after_text = textpass.read_text(Path(a[1])), textpass.read_text(Path(a[2]))
        out, key = Path(a[3]), a[4]
        tag = a[a.index('--tag') + 1] if '--tag' in a else ''
        records = extract(before, after_text)
        data = json.loads(out.read_text()) if out.exists() else {'chunks': {}}
        if key in data['chunks']:
            raise SystemExit(f'{out}: key {key} exists')
        data['chunks'][key] = {'tag': tag, 'inserts': records}
        out.write_text(json.dumps(data, indent=1) + '\n', encoding='utf-8', newline='\n')
        print(f'{key}: {len(records)} inserted blocks, {sum(r["text"].count(chr(10)) for r in records)} lines')
        return 0
    if len(a) == 5 and a[0] == 'apply':
        records = json.loads(Path(a[3]).read_text())['chunks'][a[4]]['inserts']
        textpass.write_new(Path(a[2]), apply(textpass.read_text(Path(a[1])), records))
        print(f'{Path(a[1]).name}: {len(records)} blocks inserted')
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    raise SystemExit(main())
