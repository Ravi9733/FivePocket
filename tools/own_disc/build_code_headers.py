#!/usr/bin/env python3
"""Make the runtime's hooked copies of game functions from this run's own generated code.

Two headers of the runtime hold a game function as the generator wrote it, with a few lines of this project added
(hooks that watch which shader the game binds). Until 2026-10-08 the compile stage read them from earlier private
work: about 2,300 lines of generated code of an earlier build. This stage writes them again from the run's recipe
output. A recipe (this project's own lines, names of functions, numbers and hashes; no generated code) says, per
header, its parts in order:

  a literal piece of text (declarations, the closing lines; a header may be text only: the one-line header through
  which the runtime reaches a hooked copy in another folder), or
  a function: the game function's address and the chunk it is in; the line that replaces its first line; whether
  calls of "__imp__sub_X" become calls of "sub_X"; and edits against the function's lines as generated:
  {"at": line, "remove": n, "removed_sha256": hash of those n lines, "insert": [this project's lines]}

    build_code_headers.py --chunks DIR --recipes FILE --output DIR

--chunks is the recipe stage's output folder (ppc_recomp.N.cpp). The output folder must not exist. A function is
taken from "PPC_FUNC_IMPL(__imp__sub_X) {" up to the line before the next function. Every removed stretch is
checked against its hash before it is replaced and every header against the size and SHA-256 its recipe names;
exit 1 if a header differs (it is then not written), 2 if an argument or the recipe file is refused.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

SCHEMA = 'code-headers/1'
ADDRESS = re.compile(r'[0-9A-F]{8}')
NEXT = ('PPC_FUNC_IMPL(', 'PPC_WEAK_FUNC(', 'PPC_FUNC(')
IMPORT_CALL = re.compile(r'\b__imp__(sub_[0-9A-F]{8})\b')


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def is_hash(value) -> bool:
    return isinstance(value, str) and len(value) == 64 and all(c in '0123456789abcdef' for c in value)


def check_recipes(table) -> list[dict]:
    if not isinstance(table, dict) or table.get('schema') != SCHEMA or not isinstance(table.get('headers'), list) or not table['headers']:
        raise ValueError('recipe file refused: it needs the schema ' + SCHEMA + ' and a list of headers')
    seen = set()
    for header in table['headers']:
        path = header.get('path') if isinstance(header, dict) else None
        if not isinstance(path, str) or not path or path.startswith('/') or '\\' in path or '..' in path.split('/') \
                or not path.endswith('.h') or path in seen:
            raise ValueError('recipe refused: a header needs its own relative path ending in .h')
        seen.add(path)
        if type(header.get('bytes')) is not int or header['bytes'] <= 0 or not is_hash(header.get('sha256')):
            raise ValueError(f'recipe {path}: the expected size and SHA-256 are missing')
        parts = header.get('parts')
        if not isinstance(parts, list) or not parts:      # a header of text only is one that passes on to a hooked copy
            raise ValueError(f'recipe {path}: it needs parts')
        for part in parts:
            if isinstance(part, str):
                continue
            if not isinstance(part, dict) or not isinstance(part.get('function'), str) or not ADDRESS.fullmatch(part['function']) \
                    or type(part.get('chunk')) is not int or part['chunk'] < 0 or not isinstance(part.get('first_line'), str) \
                    or '\n' in part['first_line'] or type(part.get('direct_calls')) is not bool or not isinstance(part.get('edits'), list):
                raise ValueError(f'recipe {path}: a function part is refused')
            last = 0
            for edit in part['edits']:
                if not isinstance(edit, dict) or type(edit.get('at')) is not int or type(edit.get('remove')) is not int \
                        or edit['at'] < max(last, 1) or edit['remove'] < 0 or not is_hash(edit.get('removed_sha256')) \
                        or not isinstance(edit.get('insert'), list) or not all(isinstance(l, str) and '\n' not in l for l in edit['insert']) \
                        or (edit['remove'] == 0 and not edit['insert']):
                    raise ValueError(f'recipe {path}: an edit is refused (edits come in line order, do not overlap and leave the first line alone)')
                last = edit['at'] + edit['remove']
    return table['headers']


def function_lines(chunk_text: str, address: str) -> list[str]:
    """The lines of one generated function, from its first line up to the line before the next function."""
    lines = chunk_text.split('\n')
    head = f'PPC_FUNC_IMPL(__imp__sub_{address}) {{'
    starts = [i for i, line in enumerate(lines) if line == head]
    if len(starts) != 1:
        raise ValueError(f'function {address} is not once in its chunk')
    end = next((i for i in range(starts[0] + 1, len(lines)) if lines[i].startswith(NEXT)), len(lines))
    return lines[starts[0]:end]


def function_text(part: dict, chunk_text: str) -> list[str]:
    raw = function_lines(chunk_text, part['function'])
    # the hooked copy calls the game's functions by their public names; this project's own lines are left as written
    generated = [IMPORT_CALL.sub(r'\1', line) for line in raw] if part['direct_calls'] else raw
    out, at = [part['first_line']], 1
    for edit in part['edits']:
        if edit['at'] + edit['remove'] > len(raw):
            raise ValueError(f'function {part["function"]}: an edit lies beyond the function')
        if sha('\n'.join(raw[edit['at']:edit['at'] + edit['remove']]).encode('utf-8')) != edit['removed_sha256']:
            raise ValueError(f'function {part["function"]}: the lines an edit replaces are not the lines it was made for')
        out += generated[at:edit['at']] + edit['insert']
        at = edit['at'] + edit['remove']
    return out + generated[at:]


def build_header(header: dict, read_chunk) -> bytes:
    pieces = []
    for part in header['parts']:
        pieces.append(part if isinstance(part, str) else '\n'.join(function_text(part, read_chunk(part['chunk']))))
    data = ''.join(pieces).encode('utf-8')
    if len(data) != header['bytes'] or sha(data) != header['sha256']:
        raise ValueError('the header made from this run\'s code is not the header the recipe names')
    return data


def build(headers: list[dict], chunks: Path, output: Path) -> dict:
    """Writes the headers and report.json (the same form as the native-packs stage's) into a new folder."""
    cache = {}

    def read_chunk(number: int) -> str:
        if number not in cache:
            cache[number] = (chunks / f'ppc_recomp.{number}.cpp').read_text(encoding='utf-8')
        return cache[number]
    output.mkdir(parents=True)
    report = {'schema': 1, 'status': 'complete', 'headers': {}}
    for header in headers:
        try:
            data = build_header(header, read_chunk)
        except (ValueError, OSError) as error:
            report['status'] = 'differs'
            report['headers'][header['path']] = {'status': 'refused', 'why': str(error)}
            continue
        target = output / header['path']
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        report['headers'][header['path']] = {'status': 'written', 'bytes': len(data), 'sha256': sha(data)}
    (output / 'report.json').write_text(json.dumps(report, indent=1, sort_keys=True) + '\n', encoding='utf-8', newline='\n')
    return report


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--chunks', type=Path, required=True)
    ap.add_argument('--recipes', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    a = ap.parse_args()
    headers = check_recipes(json.loads(a.recipes.read_text(encoding='utf-8')))
    if not a.chunks.is_dir():
        raise ValueError('the chunk folder is not there')
    report = build(headers, a.chunks, a.output.absolute())
    for path, h in report['headers'].items():
        if h['status'] != 'written':
            print('CODE_HEADER_REFUSED', path, '-', h['why'], flush=True)
    written = [h for h in report['headers'].values() if h['status'] == 'written']
    print('CODE_HEADERS', report['status'], f'{len(written)} of {len(report["headers"])} headers written', flush=True)
    return 0 if report['status'] == 'complete' else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (ValueError, OSError) as error:
        print('CODE_HEADERS_REFUSED:', error, file=sys.stderr)
        sys.exit(2)
