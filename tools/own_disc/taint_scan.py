#!/usr/bin/env python3
"""Look for bytes of the game in a folder meant to be handed out: 32-byte windows shared with retail-derived files.

    taint_scan.py --retail FILE [...] --scan DIR [--stride N] [--controls N] [--report FILE]

Every window of WINDOW bytes of the retail files (the decoded executable image, the shader pack, the disc's executable)
that starts at a multiple of --stride (default 16) goes into an index; then every window at every offset of every file
below DIR is looked up. With stride s, any shared run of WINDOW + s - 1 bytes or longer is certainly found; a shorter one
may be. A file with hits is listed with its hit count and the first offsets. Windows with fewer than MIN_DISTINCT
different byte values (zero fill, padding, runs of small integers, counter arrays) are left out on both sides: they occur
in any binary and say nothing about where bytes came from (measured 2026-11-05: without this rule a PGO profile's counter
arrays and MoltenVK's index tables "matched" the decoded image by the thousand; with it, none did). Nothing here writes into DIR; the
report lists file names and counts, never the bytes.

Planted controls (--controls, default 256): before the scan, that many real windows of the retail files (high-entropy, at
stride-aligned offsets chosen by a fixed seed, each WINDOW + stride - 1 bytes long) are planted between random filler in a
buffer in memory and scanned by the same code; every one must be found, or the run is refused. So neither the index nor the
low-entropy rule can hide a real copied run. Nothing of them is written anywhere.
Exit 0: no hits; 1: hits (listed); 2: refused (a control missed, or a bad argument).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

WINDOW = 32
MIN_DISTINCT = 10


def plain(chunk: bytes) -> bool:
    return len(set(chunk)) < MIN_DISTINCT


def key(chunk: bytes) -> int:
    return int.from_bytes(hashlib.blake2b(chunk, digest_size=8).digest(), 'little')


def index(paths: list[Path], stride: int) -> set[int]:
    keys: set[int] = set()
    for path in paths:
        data = path.read_bytes()
        for at in range(0, len(data) - WINDOW + 1, stride):
            chunk = data[at:at + WINDOW]
            if not plain(chunk):
                keys.add(key(chunk))
    return keys


def controls(paths: list[Path], keys: set[int], stride: int, count: int, seed: int = 20261105) -> tuple[int, int]:
    """plants `count` real retail runs between random filler in memory and scans them: (planted, found)"""
    import random
    rng = random.Random(seed)
    runs = []
    datas = [p.read_bytes() for p in paths]
    tries = 0
    while len(runs) < count and tries < count * 200:
        tries += 1
        data = rng.choice(datas)
        if len(data) < WINDOW + stride:
            continue
        at = rng.randrange(0, (len(data) - WINDOW - stride) // stride + 1) * stride
        run = data[at:at + WINDOW + stride - 1]
        if not plain(run[:WINDOW]):
            runs.append(run)
    found = 0
    for run in runs:
        filler = bytes(rng.getrandbits(8) for _ in range(97))
        if scan_bytes(filler + run + filler, keys)[0]:
            found += 1
    return len(runs), found


def scan(path: Path, keys: set[int], limit: int = 5) -> tuple[int, list[int]]:
    return scan_bytes(path.read_bytes(), keys, limit)


def scan_bytes(data: bytes, keys: set[int], limit: int = 5) -> tuple[int, list[int]]:
    hits, first = 0, []
    for at in range(0, len(data) - WINDOW + 1):
        chunk = data[at:at + WINDOW]
        if plain(chunk):
            continue
        if key(chunk) in keys:
            hits += 1
            if len(first) < limit:
                first.append(at)
    return hits, first


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--retail', type=Path, action='append', required=True)
    ap.add_argument('--scan', type=Path, required=True)
    ap.add_argument('--stride', type=int, default=16)
    ap.add_argument('--controls', type=int, default=256)
    ap.add_argument('--report', type=Path)
    a = ap.parse_args(argv)
    if not a.scan.is_dir() or a.stride < 1 or not all(p.is_file() for p in a.retail):
        print('TAINT_REFUSED: a retail file is missing, the scan folder is not one, or the stride is below 1', file=sys.stderr)
        return 2
    keys = index(a.retail, a.stride)
    planted, found = controls(a.retail, keys, a.stride, a.controls)
    if found != planted or planted < min(a.controls, 1):
        print(f'TAINT_REFUSED: planted controls {found}/{planted} found: the scan cannot be trusted', file=sys.stderr)
        return 2
    rows = []
    for path in sorted(p for p in a.scan.rglob('*') if p.is_file() and not p.is_symlink()):
        hits, first = scan(path, keys)
        if hits:
            rows.append({'file': path.relative_to(a.scan).as_posix(), 'hits': hits, 'first_offsets': first})
    report = {'window': WINDOW, 'stride': a.stride, 'retail': [str(p) for p in a.retail], 'index_entries': len(keys),
              'controls': {'planted': planted, 'found': found}, 'min_distinct': MIN_DISTINCT,
              'files_with_hits': len(rows), 'files': rows}
    if a.report:
        a.report.write_text(json.dumps(report, indent=1) + '\n', encoding='utf-8', newline='\n')
    for row in sorted(rows, key=lambda r: -r['hits'])[:40]:
        print(f'{row["hits"]:>9}  {row["file"]}')
    print(f'TAINT {len(rows)} file(s) share {WINDOW}-byte windows with the retail files (stride {a.stride}, '
          f'{len(keys)} indexed; planted controls {found}/{planted} found)', flush=True)
    return 1 if rows else 0


if __name__ == '__main__':
    sys.exit(main())
