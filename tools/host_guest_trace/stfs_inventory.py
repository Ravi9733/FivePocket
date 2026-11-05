#!/usr/bin/env python3
"""Inspect/extract explicitly supplied read-only PIRS STFS packages, privately.

Format reference: Xenia 95a5c3ee250f80c3b9d139658649d9ffb6db3eec,
src/xenia/vfs/devices/stfs_xbox.h and stfs_container_device.cc (BSD-3-Clause).
Hash consistency is checked; Microsoft signatures/licenses are NOT authenticated.
No decryption, licensing changes, package writes, CON, LIVE, SVOD, or dual tables.
"""
from dataclasses import dataclass
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import stat
import struct
import subprocess
import threading

BLOCK = 4096
# Flags a host does not have are left out there: Windows has no O_NOFOLLOW and no O_CLOEXEC (a link is then refused
# by name, below), and needs O_BINARY, or a raw read would translate line ends and stop at a 0x1A byte.
NO_FOLLOW = getattr(os, 'O_NOFOLLOW', 0)
READ_FLAGS = os.O_RDONLY | NO_FOLLOW | getattr(os, 'O_CLOEXEC', 0) | getattr(os, 'O_BINARY', 0)
NEW_FILE_FLAGS = os.O_WRONLY | os.O_CREAT | os.O_EXCL | NO_FOLLOW | getattr(os, 'O_BINARY', 0)
_SEEK_LOCK = threading.Lock()


def pread_by_seek(fd: int, size: int, offset: int) -> bytes:
    """A positioned read where the system has none (Windows): moved and read under one lock."""
    with _SEEK_LOCK:
        os.lseek(fd, offset, os.SEEK_SET)
        chunks, left = [], size
        while left > 0:
            chunk = os.read(fd, left)
            if not chunk:
                break
            chunks.append(chunk)
            left -= len(chunk)
        return b''.join(chunks)


pread = os.pread if hasattr(os, 'pread') else pread_by_seek
FANOUT = 170
END = 0xFFFFFF
MAX_PACKAGE = 8 * 1024**3
MAX_ENTRIES = 4096
MAX_CACHE = 32 * 1024**2


def component(name: str) -> str:
    if not name or not name.isascii() or len(name) > 40 or name in {'.', '..'} \
            or name.endswith(('.', ' ')) or any(ord(c) < 32 or c in '/\\:*?"<>|' for c in name):
        raise ValueError('unsupported or unsafe package component')
    return name


def data_offset(block: int, base: int) -> int:
    physical = block
    for coverage in (170, 28900, 4913000):
        physical += (block + coverage) // coverage
        if block < coverage:
            break
    return base + physical * BLOCK


def table_offset(block: int, level: int, base: int) -> int:
    if level == 0:
        physical = 0 if block < 170 else (block // 170) * 171 + block // 28900 + 1 + (block >= 28900)
    elif level == 1:
        physical = 171 if block < 28900 else (block // 28900) * 29071 + 1
    elif level == 2:
        physical = 29071
    else:
        raise ValueError('unsupported hash level')
    return base + physical * BLOCK


@dataclass(frozen=True)
class Entry:
    index: int
    path: str
    size: int
    blocks: tuple[int, ...]
    directory: bool
    created: int
    modified: int


class Package:
    def __init__(self, path: Path, title_id: int):
        self.fd = -1
        self.cache = {}
        self.path = path
        try:
            if not NO_FOLLOW and Path(path).is_symlink():
                raise ValueError('package path is a link')
            self.fd = os.open(path, READ_FLAGS)
            self.initial = os.fstat(self.fd)
            self.size = self.initial.st_size
            if not stat.S_ISREG(self.initial.st_mode) or not 0x971A <= self.size <= MAX_PACKAGE:
                raise ValueError('package size/type rejected')
            header = self.read(0, 0x971A)
            if header[:4] != b'PIRS' or struct.unpack_from('>I', header, 0x344)[0] != 2 \
                    or struct.unpack_from('>I', header, 0x360)[0] != title_id \
                    or struct.unpack_from('>I', header, 0x348)[0] not in (1, 2):
                raise ValueError('package type/title/version rejected')
            header_size = struct.unpack_from('>I', header, 0x340)[0]
            if not 0x971A <= header_size <= min(self.size, 1024 * 1024):
                raise ValueError('package header bounds rejected')
            self.base = (header_size + BLOCK - 1) & -BLOCK
            volume = header[0x379:0x39D]
            if volume[:3] != b'\x24\0\x01' or any(header[0x39D:0x3AD]):
                raise ValueError('only single-file read-only STFS version zero is supported')
            self.total = int.from_bytes(volume[28:32], 'big')
            free = int.from_bytes(volume[32:36], 'big')
            self.directory_count = int.from_bytes(volume[3:5], 'little')
            self.first_directory = int.from_bytes(volume[5:8], 'little')
            if not 1 <= self.total <= min(170**3, self.size // BLOCK) or free != 0 \
                    or not 1 <= self.directory_count <= MAX_ENTRIES // 64 \
                    or data_offset(self.total - 1, self.base) + BLOCK > self.size:
                raise ValueError('package allocation bounds rejected')
            self.root_level = 0 if self.total <= 170 else 1 if self.total <= 28900 else 2
            self.root_hash = volume[8:28]
            self.display_bytes = header[0x411:0x511]
            # UTF-16BE validity only; do not publish private display text.
            display = self.display_bytes.decode('utf-16-be').split('\0', 1)[0]
            self.display_bytes = display.encode('utf-16-be').ljust(256, b'\0')
            self.title_id = title_id
            self.claimed = bytearray(self.total)
            self.entries = self.directory()
            self.check_unchanged()
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def check_unchanged(self):
        current = os.fstat(self.fd)
        fields = ('st_dev', 'st_ino', 'st_size', 'st_mtime_ns', 'st_ctime_ns')
        if any(getattr(current, field) != getattr(self.initial, field) for field in fields):
            raise ValueError('package changed during inspection')

    def read(self, offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or size > 1024 * 1024 or offset + size > self.size:
            raise ValueError('package read outside validated bounds')
        data = pread(self.fd, size, offset)
        if len(data) != size:
            raise ValueError('short package read')
        return data

    def table(self, block: int, level: int) -> bytes:
        offset = table_offset(block, level, self.base)
        if offset in self.cache:
            return self.cache[offset]
        if level == self.root_level:
            expected = self.root_hash
        else:
            parent = self.table(block, level + 1)
            slot = (block // (170 ** (level + 1))) % 170
            expected = parent[slot * 24:slot * 24 + 20]
        data = self.read(offset, BLOCK)
        if hashlib.sha1(data).digest() != expected:
            raise ValueError('STFS hash-table consistency failure')
        if (len(self.cache) + 1) * BLOCK > MAX_CACHE:
            raise ValueError('hash table cache cap exceeded')
        self.cache[offset] = data
        return data

    def record(self, block: int) -> tuple[bytes, int]:
        if not 0 <= block < self.total:
            raise ValueError('STFS block outside volume')
        table = self.table(block, 0)
        offset = block % 170 * 24
        info = struct.unpack_from('>I', table, offset + 20)[0]
        if info >> 30 != 2:
            raise ValueError('STFS block is not allocated')
        return table[offset:offset + 20], info & END

    def chain(self, start: int, count: int) -> tuple[int, ...]:
        if not 0 <= count <= self.total:
            raise ValueError('STFS chain count outside volume')
        result = []
        current = start
        for _ in range(count):
            _, following = self.record(current)
            if self.claimed[current]:
                raise ValueError('STFS block overlap or chain cycle')
            self.claimed[current] = 1
            result.append(current)
            current = following
        if count and current != END:
            raise ValueError('STFS chain exceeds declared allocation')
        return tuple(result)

    def block(self, number: int) -> bytes:
        expected, _ = self.record(number)
        data = self.read(data_offset(number, self.base), BLOCK)
        if hashlib.sha1(data).digest() != expected:
            raise ValueError('STFS data hash consistency failure')
        return data

    def directory(self) -> tuple[Entry, ...]:
        chain = self.chain(self.first_directory, self.directory_count)
        entries, by_index, seen = [], {}, set()
        for block_index, number in enumerate(chain):
            data = self.block(number)
            for slot in range(64):
                index = block_index * 64 + slot
                raw = data[slot * 64:(slot + 1) * 64]
                if not any(raw):
                    continue
                length, is_directory = raw[40] & 63, bool(raw[40] & 128)
                if not 1 <= length <= 40 or any(raw[length:40]):
                    raise ValueError('malformed STFS name storage')
                name = component(raw[:length].decode('ascii'))
                parent = int.from_bytes(raw[50:52], 'big')
                if parent != 0xFFFF:
                    if parent not in by_index or not by_index[parent].directory:
                        raise ValueError('STFS parent must precede child and be a directory')
                    name = by_index[parent].path + '/' + name
                if len(name) > 1024 or name.count('/') > 32 or name.lower() in seen:
                    raise ValueError('STFS duplicate or deeply nested path')
                seen.add(name.lower())
                valid = int.from_bytes(raw[41:44], 'little')
                allocated = int.from_bytes(raw[44:47], 'little')
                start = int.from_bytes(raw[47:50], 'little')
                size = int.from_bytes(raw[52:56], 'big')
                if allocated != valid or allocated != (size + BLOCK - 1) // BLOCK \
                        or (is_directory and (allocated or size)):
                    raise ValueError('STFS file size/allocation mismatch')
                blocks = self.chain(start, allocated)
                if raw[40] & 64 and any(b != start + i for i, b in enumerate(blocks)):
                    raise ValueError('STFS contiguous flag disagrees with chain')
                entry = Entry(index, name, size, blocks, is_directory,
                              int.from_bytes(raw[56:60], 'big'), int.from_bytes(raw[60:64], 'big'))
                by_index[index] = entry
                entries.append(entry)
        return tuple(entries)

    def extract(self, entry: Entry, output: Path) -> str:
        if entry.directory or entry not in self.entries:
            raise ValueError('extract requires an owned file entry')
        # Parent created exclusively by this CLI; never a guest-selected path.
        fd = os.open(output, NEW_FILE_FLAGS, 0o600)
        digest = hashlib.sha256()
        remaining = entry.size
        with os.fdopen(fd, 'wb', buffering=1024 * 1024) as stream:
            for number in entry.blocks:
                data = self.block(number)[:min(remaining, BLOCK)]
                stream.write(data)
                digest.update(data)
                remaining -= len(data)
        self.check_unchanged()
        if remaining:
            raise ValueError('incomplete STFS extraction')
        return digest.hexdigest()


def require_private(path: Path) -> Path:
    root = Path(__file__).resolve().parents[2]
    lexical = path.absolute()
    if lexical != lexical.resolve() or not lexical.is_relative_to(root / 'local-analysis') \
            or subprocess.run(['git', 'check-ignore', '--quiet', '--', str(lexical)], cwd=root).returncode:
        raise ValueError('inputs/output must be ignored private paths without links')
    return lexical


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('packages', nargs='+', type=Path)
    parser.add_argument('--title-id', required=True, type=lambda v: int(v, 0))
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--extract', action='store_true')
    args = parser.parse_args()
    if not 0 < args.title_id <= 0xFFFFFFFF or not 1 <= len(args.packages) <= 16:
        raise ValueError('title or package count rejected')
    paths = [require_private(p) for p in args.packages]
    if len({p.name.lower() for p in paths}) != len(paths) or any(not re.fullmatch('[A-Za-z0-9]{1,40}', p.name) for p in paths):
        raise ValueError('package identities invalid or duplicated')
    output = require_private(args.output_dir)
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    results = []
    records = []
    for path in paths:
        with Package(path, args.title_id) as package:
            files = []
            destination = output / path.name
            if args.extract:
                destination.mkdir(mode=0o700)
            for entry in package.entries:
                report = {'path': entry.path, 'bytes': entry.size, 'blocks': len(entry.blocks), 'directory': entry.directory}
                if args.extract:
                    target = destination / entry.path
                    if entry.directory:
                        target.mkdir(mode=0o700)
                    else:
                        report['sha256'] = package.extract(entry, target)
                files.append(report)
            package.check_unchanged()
            results.append({'package': path.name, 'package_bytes': package.size,
                            'data_blocks': package.total, 'files': files})
            # Device 1 is an explicitly declared virtual read-only content source,
            # not a copied console identifier or a claimed physical Xbox HDD.
            records.append(struct.pack('>II', 1, 2) + package.display_bytes + path.name.encode().ljust(44, b'\0'))
            print(f'Validated package {len(results)}/{len(paths)}: {len(files)} entries; payload hashes ' + ('checked' if args.extract else 'not all read'), flush=True)
    manifest = {'schema_version': 1, 'title_id': args.title_id, 'packages': results,
                'validation': 'hash-tree and directory/chain consistency; no Microsoft signature authentication',
                'all_referenced_payload_blocks_verified': args.extract, 'virtual_device_id': 1}
    with (output / 'inventory.json').open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(manifest, stream, indent=2); stream.write('\n')
    with (output / 'content-catalog.bin').open('xb') as stream:
        stream.write(b'GXCONT1\0' + struct.pack('>II', args.title_id, len(records)) + b''.join(records))
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, UnicodeError):
        raise SystemExit('STFS validation failed; private diagnostics suppressed; incomplete output must not be mounted')
