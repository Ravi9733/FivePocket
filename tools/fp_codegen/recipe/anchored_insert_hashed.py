#!/usr/bin/env python3
"""anchored_insert_hashed.py: anchored_insert.py's records without the generated anchor lines (the form that may leave the
private folder), and their replay.

  publish INSERTS.json OUT.json
        writes OUT.json = INSERTS.json with every record's anchor LINE replaced by {"sha256": H}, H = the SHA-256 of the
        line with its line feed (UTF-8). scope, occurrence, count and text are kept: the scope is a guest address (or
        "chunk") and the text is the line(s) this project inserts (hooks, diagnostics), never a generated line.
  publish INSERTS.json OUT.json --check BEFORE.cpp KEY [BEFORE.cpp KEY ...]
        and proves, per chunk KEY, that the published records give the same text as the original ones on BEFORE (the
        chunk as it is before the pass).
  apply IN.cpp OUT.cpp INSERTS.json KEY
        replays the records of KEY; an anchor is either a line (anchored_insert.py's form) or {"sha256": H}. Refused when
        a scope is missing or the number of matching lines in it differs from the recorded count, as in anchored_insert.py.

The recipe's anchored-insert adapter calls apply(text, records), so a pass switches to this form by naming this tool and the
published records; anchored_insert.py and the recipes that pin it are unchanged. Contains no game code.
"""
from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import anchored_insert  # noqa: E402
import textpass  # noqa: E402


def line_sha(line: str) -> str:
    return hashlib.sha256(line.encode('utf-8')).hexdigest()


def matches(record: dict, line: str) -> bool:
    anchor = record['anchor']
    return line == anchor if isinstance(anchor, str) else line_sha(line) == anchor['sha256']


def apply(text: str, records: list[dict]) -> str:
    lines = text.splitlines(keepends=True)
    owner = anchored_insert.scopes(lines)
    after: dict[int, list[str]] = {}
    for r in records:
        idx = [k for k in anchored_insert.scope_lines(lines, owner, r['scope']) if matches(r, lines[k])]
        if len(idx) != r['count']:
            raise SystemExit(f'scope {r["scope"]}: anchor occurs {len(idx)} times, recorded {r["count"]}')
        after.setdefault(idx[r['occurrence'] - 1], []).append(r['text'])
    out = []
    for k, line in enumerate(lines):
        out.append(line)
        out += after.get(k, [])
    return ''.join(out)


def publish(data: dict) -> dict:
    out = {k: v for k, v in data.items() if k != 'chunks'}
    out['schema'] = 'gta5-anchored-insert/2'
    out['chunks'] = {}
    for key, chunk in data['chunks'].items():
        records = []
        for r in chunk['inserts']:
            anchor = r['anchor'] if isinstance(r['anchor'], dict) else {'sha256': line_sha(r['anchor'])}
            records.append({**r, 'anchor': anchor})
        out['chunks'][key] = {**chunk, 'inserts': records}
    return out


def main() -> int:
    a = sys.argv[1:]
    if len(a) >= 3 and a[0] == 'publish':
        data = json.loads(Path(a[1]).read_text(encoding='utf-8'))
        out = publish(data)
        rest = a[3:]
        if rest:
            if rest[0] != '--check' or len(rest) < 3 or len(rest) % 2 != 1:
                print(__doc__)
                return 2
            for before, key in zip(rest[1::2], rest[2::2]):
                text = textpass.read_text(Path(before))
                old = anchored_insert.apply(text, data['chunks'][key]['inserts'])
                if apply(text, out['chunks'][key]['inserts']) != old:
                    raise SystemExit(f'{key}: the published records give another text')
                print(f'{key}: published records give the same text ({len(out["chunks"][key]["inserts"])} blocks)')
        Path(a[2]).write_text(json.dumps(out, indent=1) + '\n', encoding='utf-8', newline='\n')
        print(f'{a[2]}: {sum(len(c["inserts"]) for c in out["chunks"].values())} records, every anchor as its SHA-256')
        return 0
    if len(a) == 5 and a[0] == 'apply':
        records = json.loads(Path(a[3]).read_text(encoding='utf-8'))['chunks'][a[4]]['inserts']
        textpass.write_new(Path(a[2]), apply(textpass.read_text(Path(a[1])), records))
        print(f'{Path(a[1]).name}: {len(records)} blocks inserted')
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    raise SystemExit(main())
