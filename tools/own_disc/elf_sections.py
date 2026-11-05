#!/usr/bin/env python3
"""Section-by-section identity of an ELF64 little-endian file (objects and shared libraries), with no dependencies.

Two builds of one source on two hosts can differ in bytes that are not code or data: the compiler's own name in
`.comment`, and paths in the debug sections. `code_identity` hashes everything else, section by section, in file
order: names, types, flags, sizes and contents, including symbol, string and relocation tables.

    elf_sections.py FILE...            one line per file: sha256, code identity, sections left out
    elf_sections.py --diff A B         the sections that differ between two files
    elf_sections.py --archive A...     one line per `ar` archive: sha256, archive identity, number of members

`archive_identity` is the same for a static library (`ar`, GNU format): its object members in order, each by name
and code identity. The NDK's compiler names itself differently on macOS and on Windows, so a library built from one
source on two hosts differs in bytes and has one identity.
"""
from __future__ import annotations

import hashlib
import json
import re
import struct
import sys

LEFT_OUT = re.compile(r'^\.comment$|^\.(rela\.)?debug_')
SHT_NOBITS = 8


def sections(path) -> list[dict]:
    with open(path, 'rb') as f:
        return sections_of(f.read(), path)


def sections_of(data: bytes, path='data') -> list[dict]:
    if data[:6] != b'\x7fELF\x02\x01':
        raise ValueError(f'not an ELF64 little-endian file: {path}')
    try:
        shoff, = struct.unpack_from('<Q', data, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x3A)
        if shnum == 0 or shstrndx == 0xFFFF:  # counts moved into section 0: files with more than 65,279 sections
            first = struct.unpack_from('<IIQQQQIIQQ', data, shoff)
            shnum = shnum or first[5]
            shstrndx = first[6] if shstrndx == 0xFFFF else shstrndx
        heads = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize) for i in range(shnum)]
    except struct.error:
        raise ValueError(f'truncated section table in {path}') from None
    strings = heads[shstrndx]
    table = data[strings[4]:strings[4] + strings[5]]
    result = []
    for name, kind, flags, _addr, offset, size, link, info, align, entsize in heads:
        end = table.index(b'\0', name)
        body = b'' if kind == SHT_NOBITS else data[offset:offset + size]
        if len(body) != (0 if kind == SHT_NOBITS else size):
            raise ValueError(f'truncated section in {path}')
        result.append(dict(name=table[name:end].decode(), type=kind, flags=flags, size=size, link=link, info=info,
                           align=align, entsize=entsize, sha256=hashlib.sha256(body).hexdigest()))
    return result


def code_identity(path) -> tuple[str, list[str]]:
    """(hash over every section that is not `.comment` or debug, names of the sections left out)."""
    return identity_of(sections(path))


def identity_of(section_list: list[dict]) -> tuple[str, list[str]]:
    kept, left = [], []
    for s in section_list:
        (left if LEFT_OUT.search(s['name']) else kept).append(s)
    # link/info are indices into the section table; a left-out section shifts them, so they are not part of identity.
    plain = [(s['name'], s['type'], s['flags'], s['size'], s['align'], s['entsize'], s['sha256']) for s in kept]
    return hashlib.sha256(json.dumps(plain).encode()).hexdigest(), sorted({s['name'] for s in left})


AR_MAGIC = b'!<arch>\n'


def archive_members(path) -> list[tuple[str, bytes]]:
    """The object members of a GNU-format `ar` archive, in file order, as (name, bytes). The symbol table and the
    table of long names are the archive's own records, not members."""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:8] != AR_MAGIC:
        raise ValueError(f'not an ar archive: {path}')
    at, names, members = 8, None, []
    while at < len(data):
        head = data[at:at + 60]
        if len(head) != 60 or head[58:60] != b'`\n' or not head[48:58].strip().isdigit():
            raise ValueError(f'damaged member header in {path}')
        size = int(head[48:58])
        body = data[at + 60:at + 60 + size]
        if len(body) != size:
            raise ValueError(f'truncated member in {path}')
        at += 60 + size + (size & 1)             # members start at even offsets
        raw = head[:16].rstrip(b' ')
        if raw in (b'/', b'/SYM64/'):            # the symbol table
            continue
        if raw == b'//':                         # the table of names longer than the header's field
            names = body
            continue
        if raw.startswith(b'/') and raw[1:].isdigit():
            start = int(raw[1:])
            end = -1 if names is None else names.find(b'/\n', start)
            if end < 0 or start >= len(names):
                raise ValueError(f'member name outside the name table in {path}')
            name = names[start:end]
        elif raw.endswith(b'/') and len(raw) > 1:
            name = raw[:-1]
        else:
            raise ValueError(f'member name of another archive format in {path}')
        members.append((name.decode(), body))
    return members


def archive_identity(path) -> tuple[str, int]:
    """(hash over the archive's object members in order, each by name and code identity; number of members)."""
    members = archive_members(path)
    if not members:
        raise ValueError(f'no members in {path}')
    plain = [[name, identity_of(sections_of(body, f'{path}({name})'))[0]] for name, body in members]
    return hashlib.sha256(json.dumps(plain).encode()).hexdigest(), len(plain)


def file_sha256(path) -> str:
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def diff(a, b) -> list[str]:
    """Names of sections whose presence, size or content differs between two files (order kept)."""
    def keyed(path):
        seen, out = {}, {}
        for s in sections(path):
            n = seen.get(s['name'], 0)
            seen[s['name']] = n + 1
            out[(s['name'], n)] = (s['type'], s['flags'], s['size'], s['sha256'])
        return out
    x, y = keyed(a), keyed(b)
    names = []
    for key in list(x) + [k for k in y if k not in x]:
        if x.get(key) != y.get(key) and key[0] not in names:
            names.append(key[0])
    return names


def main() -> None:
    if len(sys.argv) == 4 and sys.argv[1] == '--diff':
        for name in diff(sys.argv[2], sys.argv[3]):
            print(name)
        return
    if len(sys.argv) > 2 and sys.argv[1] == '--archive':
        for path in sys.argv[2:]:
            identity, count = archive_identity(path)
            print(file_sha256(path), identity, count, path)
        return
    for path in sys.argv[1:]:
        identity, left = code_identity(path)
        print(file_sha256(path), identity, ','.join(left) or '-', path)


if __name__ == '__main__':
    main()
