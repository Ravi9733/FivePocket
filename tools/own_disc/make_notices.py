#!/usr/bin/env python3
"""Write the notices of a builder package: every third-party part inside it, where it sits, under which terms, and the
terms' text copied from the part's own source.

    make_notices.py --components TABLE.json --root NAME=PATH [--root NAME=PATH ...] --out DIR
                    [--package-list FILE] [--allow-pending]
    make_notices.py --components TABLE.json --root NAME=PATH ... --check [--package-list FILE] [--allow-pending]

TABLE.json (tools/own_disc/notices/components.json) names the parts. Each part has an id, a name, where it comes from,
its terms in a word, the places in the package that hold it (patterns with * and ?, forward slashes), and where its
terms' text is read from: a file below a named root, lines of such a file (a notice in a source file's head), or a
member of a tar archive that is itself a root. Every text is pinned by the SHA-256 of the file it is read from: a
tree that holds another text is refused, so the notices cannot drift from the sources they describe. No text is typed
into this tool or its table.

A part may instead say that its text travels inside the package already (`travels`: the place of that file in the
package) or that the package does not carry the part at all (`fetched`: the player's builder fetches it and shows its
terms). A part whose text could not be collected yet says so (`pending`: the reason). With a pending part nothing is
written unless --allow-pending is given; the notices then say INCOMPLETE in their first line and name the parts.

--package-list FILE: the package's files, one per line (a tab and more columns after the name are ignored). Every
program in it (.exe, .dll, .pyd) must lie at a place some part names; a program that no part covers stops the run.
A part marked `general` (a runtime linked into every program) does not count for this: a new program has to be named
by the part that describes what it is.

Written into DIR (which must not exist): NOTICES.txt (a contents list, then per part its facts and its text),
licences/<id>/<n>-<name> (each text byte for byte as read), and notices.json (each part, each text's source hash and
output hash, and the hash of NOTICES.txt). The output is the same for the same table and roots.
Exit 0: written or checked; 2: refused, nothing written. Nothing here reads the game.
"""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import sys
import tarfile
from pathlib import Path

PROGRAM_SUFFIXES = ('.exe', '.dll', '.pyd')
SCHEMA = 'builder-notices/1'


class Refusal(Exception):
    pass


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_text_source(text: dict, roots: dict) -> tuple[bytes, str]:
    """The bytes of one terms text and the name it is filed under. Refuses a missing file, another file than the
    pinned one, and an extract that is not inside the file."""
    root = text.get('root')
    if root not in roots:
        raise Refusal(f'no --root was given for "{root}"')
    base = roots[root]
    if 'member' in text:
        if not base.is_file():
            raise Refusal(f'root "{root}" is not an archive file: {base}')
        with tarfile.open(base) as tar:
            try:
                handle = tar.extractfile(tar.getmember(text['member']))
            except KeyError:
                raise Refusal(f'{base.name} has no member {text["member"]}') from None
            if handle is None:
                raise Refusal(f'{text["member"]} in {base.name} is not a file')
            whole = handle.read()
        name = Path(text['member']).name
    else:
        path = base / text['path']
        if not path.is_file():
            raise Refusal(f'the terms file is not there: {root}:{text["path"]}')
        whole = path.read_bytes()
        name = Path(text['path']).name
    if sha256(whole) != text.get('sha256'):
        raise Refusal(f'{root}:{text.get("path", text.get("member"))} is not the pinned file (found {sha256(whole)[:16]}, pinned {str(text.get("sha256"))[:16]})')
    if 'lines' in text:
        first, last = text['lines']
        lines = whole.split(b'\n')
        if not (1 <= first <= last <= len(lines)):
            raise Refusal(f'{root}:{text["path"]} has no lines {first} to {last}')
        whole = b'\n'.join(lines[first - 1:last]) + b'\n'
        if not whole.strip():
            raise Refusal(f'{root}:{text["path"]} lines {first} to {last} are empty')
        name = f'{name}.lines-{first}-{last}.txt'
    return whole, name


def load_table(path: Path) -> list:
    table = json.loads(path.read_text(encoding='utf-8'))
    if table.get('schema') != SCHEMA:
        raise Refusal(f'{path.name} is not a {SCHEMA} table')
    parts = table.get('components')
    if not isinstance(parts, list) or not parts:
        raise Refusal('the table names no part')
    seen = set()
    for part in parts:
        for key in ('id', 'name', 'origin', 'terms', 'in_package'):
            if not part.get(key):
                raise Refusal(f'a part lacks "{key}": {part.get("id", part.get("name", "?"))}')
        if part['id'] in seen:
            raise Refusal(f'two parts share the id {part["id"]}')
        seen.add(part['id'])
        ways = [key for key in ('texts', 'travels', 'fetched', 'pending') if part.get(key)]
        if not ways:
            raise Refusal(f'{part["id"]} names no terms text and no reason why not')
    return parts


def uncovered_programs(parts: list, listed: list) -> list:
    patterns = [pattern for part in parts if not part.get('general') for pattern in part['in_package']]
    return [name for name in listed
            if Path(name).suffix.lower() in PROGRAM_SUFFIXES and not any(fnmatch.fnmatchcase(name, pattern) for pattern in patterns)]


def read_package_list(path: Path) -> list:
    names = []
    for line in path.read_text(encoding='utf-8').splitlines():
        name = line.split('\t')[0].strip().replace('\\', '/')
        if name and not name.startswith('#'):
            names.append(name)
    return names


def gather(parts: list, roots: dict, allow_pending: bool, listed: list | None) -> tuple[list, list]:
    """Every part with its texts read and checked: ([(part, [(bytes, name), ...])], [pending ids])."""
    pending = [part['id'] for part in parts if part.get('pending')]
    if pending and not allow_pending:
        raise Refusal('terms texts still to collect for: ' + ', '.join(pending))
    if listed is not None:
        missing = uncovered_programs(parts, listed)
        if missing:
            raise Refusal('programs in the package that no part covers: ' + ', '.join(missing[:8]) + (f' and {len(missing) - 8} more' if len(missing) > 8 else ''))
    gathered = []
    for part in parts:
        texts = [read_text_source(text, roots) for text in part.get('texts', [])]
        gathered.append((part, texts))
    return gathered, pending


def render(gathered: list, pending: list) -> str:
    out = []
    if pending:
        out.append('INCOMPLETE: the terms texts of these parts are still to be collected: ' + ', '.join(pending) + '.')
        out.append('')
    out += ['Notices for third-party parts inside this package', '=' * 49, '',
            'This file lists every part of this package that other people wrote, where it sits in the package, and',
            'the terms it comes under. The texts below are copies of the files named beside them, as they stand in',
            'the sources this package was built from; the same files lie in the folder licences/. Nothing here is',
            'the game: the game\'s code and data come from the player\'s own discs.', '', 'Contents']
    for number, (part, _) in enumerate(gathered, 1):
        out.append(f'  {number}. {part["name"]} ({part["terms"]})')
    for number, (part, texts) in enumerate(gathered, 1):
        title = f'{number}. {part["name"]}'
        out += ['', '', title, '-' * len(title), '', f'Terms: {part["terms"]}', f'From: {part["origin"]}' + (f' at {part["revision"]}' if part.get('revision') else ''),
                'In this package: ' + ', '.join(part['in_package'])]
        if part.get('note'):
            out.append(part['note'])
        if part.get('travels'):
            out.append(f'Its terms travel with it: {part["travels"]}')
        if part.get('fetched'):
            out.append(f'Not carried by this package: {part["fetched"]}')
        if part.get('pending'):
            out.append(f'TEXT STILL TO BE COLLECTED: {part["pending"]}')
        for index, (data, name) in enumerate(texts, 1):
            try:
                body = data.decode('utf-8')
            except UnicodeDecodeError:
                raise Refusal(f'{part["id"]}: {name} is not UTF-8 text') from None
            out += ['', f'--- licences/{part["id"]}/{index}-{name} ---', '', body.replace('\r\n', '\n').rstrip('\n')]
    return '\n'.join(out) + '\n'


def write(gathered: list, pending: list, out: Path) -> dict:
    if out.exists():
        raise Refusal(f'the output folder exists already: {out}')
    notices = render(gathered, pending).encode('utf-8')
    record = {'schema': SCHEMA, 'complete': not pending, 'pending': pending, 'notices_sha256': sha256(notices), 'components': []}
    files = {'NOTICES.txt': notices}
    for part, texts in gathered:
        entry = {key: part[key] for key in ('id', 'name', 'origin', 'terms', 'in_package')}
        entry['texts'] = []
        for index, ((data, name), source) in enumerate(zip(texts, part.get('texts', [])), 1):
            place = f'licences/{part["id"]}/{index}-{name}'
            files[place] = data
            entry['texts'].append({'file': place, 'sha256': sha256(data), 'source_sha256': source['sha256']})
        record['components'].append(entry)
    files['notices.json'] = (json.dumps(record, indent=1, sort_keys=True) + '\n').encode('utf-8')
    out.mkdir(parents=True)
    for place, data in files.items():
        target = out / place
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    return record


def parse_roots(pairs: list) -> dict:
    roots = {}
    for pair in pairs or []:
        name, sep, path = pair.partition('=')
        if not sep or not name or not path:
            raise Refusal(f'--root wants NAME=PATH, not {pair}')
        if name in roots:
            raise Refusal(f'--root {name} is given twice')
        roots[name] = Path(path)
    return roots


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description='Write the notices of a builder package from the parts\' own terms files.')
    parser.add_argument('--components', required=True, type=Path)
    parser.add_argument('--root', action='append')
    parser.add_argument('--out', type=Path)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--package-list', type=Path)
    parser.add_argument('--allow-pending', action='store_true')
    args = parser.parse_args(argv)
    try:
        if args.check == (args.out is not None):
            raise Refusal('give either --out DIR or --check')
        parts = load_table(args.components)
        listed = read_package_list(args.package_list) if args.package_list else None
        gathered, pending = gather(parts, parse_roots(args.root), args.allow_pending, listed)
        texts = sum(len(t) for _, t in gathered)
        if args.check:
            render(gathered, pending)
            print(f'notices: {len(parts)} parts, {texts} terms texts, every pinned file found' + (f'; pending: {", ".join(pending)}' if pending else ''))
            return 0
        record = write(gathered, pending, args.out)
        print(f'notices: {len(parts)} parts, {texts} terms texts written to {args.out} (NOTICES.txt {record["notices_sha256"][:16]})'
              + (f'; INCOMPLETE, pending: {", ".join(pending)}' if pending else ''))
        return 0
    except (Refusal, OSError, json.JSONDecodeError, tarfile.TarError) as error:
        print(f'notices: refused: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
