#!/usr/bin/env python3
"""The six small catalogs the runtime reads beside the game data, made from the player's own disc images.

  file-metadata.bin         what the play disc holds: every folder and file with its size (mount "game:")
  installed-content.bin     the installed packages of the title: their names and display names
  installed-package-N.bin   what package N holds (mount "content:"), N = 1 for the first package by name

Names and sizes come from the discs. The four timestamps of each record do not: the catalogs a build is pinned to
were snapshots of files extracted on one machine on one day, and they also list files that are not on the disc (the
decoded images that lay in that folder). To give the same bytes anywhere, a table supplies, per catalog, the
timestamps in record order and the names and sizes of those extra files. The table holds numbers and the names of
our own files; nothing of the game.

    build_catalogs.py --play ISO --inventory DIR --times FILE --title-id N --output DIR

--inventory is the output of `build_shader_pack.py extract` (it copied the title's packages out of the install disc
and lists them). The output folder must not exist. Exit 1 if the table names expected hashes and one differs.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
for directory in ('tools/own_disc', 'tools/host_guest_trace'):
    sys.path.insert(0, str(ROOT / directory))
import content_catalog  # noqa: E402
import file_metadata_catalog as metadata  # noqa: E402

TIMES_SCHEMA = 'catalog-times/1'
DIRECTORY, FILE = 0x11, 1


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def metadata_catalog(mount: str, entries: list[tuple[str, int, bool]], times: list) -> bytes:
    """entries: (path under the mount with '/' separators, size, is a folder), without the root. One row of four
    timestamps per record, the root first, then the entries in the order of their catalog path."""
    root = metadata.canonical_path(mount)        # mount 'game:' -> root 'game:\\'
    if root != mount + '\\':
        raise ValueError('the mount must be a lower-case drive name with its colon')
    rows = {root: (0, True)}
    for path, size, folder in entries:
        guest = metadata.canonical_path(root + path)
        if guest in rows:
            raise ValueError('two entries with one catalog path: ' + guest)
        rows[guest] = (0 if folder else size, folder)
    if len(times) != len(rows):
        raise ValueError(f'the table has {len(times)} rows of timestamps, the catalog has {len(rows)} records')
    records = []
    for (guest, (size, folder)), stamp in zip(sorted(rows.items()), times):
        if not isinstance(stamp, list) or len(stamp) != 4 or not all(type(t) is int and 0 <= t < 1 << 63 for t in stamp):
            raise ValueError('a row of timestamps is not four non-negative integers')
        encoded = guest.encode('ascii')
        allocation = 0 if folder else (size + 2047) // 2048 * 2048
        records.append(struct.pack('>I', len(encoded)) + encoded
                       + struct.pack('>6QI', *stamp, allocation, size, DIRECTORY if folder else FILE))
    data = metadata.MAGIC + struct.pack('>II', len(records), len(mount)) + mount.encode('ascii') + b''.join(records)
    metadata.validate_catalog(data)
    return data


def content_records(title_id: int, packages: list[tuple[str, bytes]]) -> bytes:
    """packages: (name, 256 bytes of display name) in name order. The layout stfs_inventory.py writes."""
    names = [name for name, _ in packages]
    if names != sorted(names):
        raise ValueError('packages must come in name order')
    data = b'GXCONT1\0' + struct.pack('>II', title_id, len(packages)) + b''.join(
        struct.pack('>II', 1, 2) + display + name.encode('ascii').ljust(44, b'\0') for name, display in packages)
    if content_catalog.parse_catalog(data) != (title_id, len(packages)):
        raise ValueError('content catalog refused by its own reader')
    return data


def disc_listing(disc) -> list[tuple[str, int, bool]]:
    """Every folder and file of a disc image, through the builder's reader (names in lower case, as it gives them)."""
    result, pending, seen = [], [('', disc.root_sector, disc.root_size, 0)], set()
    with disc.image.open('rb') as f:
        while pending:
            prefix, sector, size, depth = pending.pop()
            if (sector, size) in seen or depth > 32 or len(seen) > 10000:
                raise ValueError('disc directory recursion refused')
            seen.add((sector, size))
            for name, (start, length, folder) in sorted(disc._read_dir(f, sector, size).items()):
                if name in ('.', '..') or '/' in name or '\\' in name:
                    raise ValueError('unsafe disc component')
                result.append((prefix + name, length, folder))
                if folder:
                    pending.append((prefix + name + '/', start, length, depth + 1))
    return result


def load_times(path: Path) -> dict:
    table = json.loads(path.read_text())
    if not isinstance(table, dict) or table.get('schema') != TIMES_SCHEMA or not isinstance(table.get('play'), dict) \
            or not isinstance(table.get('packages'), list) or not table['packages']:
        raise ValueError('timestamp table refused')
    for extra in table['play'].get('extras', []):
        if not isinstance(extra, dict) or not isinstance(extra.get('name'), str) or type(extra.get('bytes')) is not int \
                or '/' in extra['name'] or '\\' in extra['name']:
            raise ValueError('timestamp table: an extra file needs a plain name and a size')
    return table


def build(play_entries, packages, table: dict, title_id: int) -> dict[str, bytes]:
    """packages: (name on the disc, display bytes, entries) per package of the title."""
    extras = [(e['name'], e['bytes'], False) for e in table['play'].get('extras', [])]
    outputs = {'file-metadata.bin': metadata_catalog('game:', list(play_entries) + extras, table['play']['times'])}
    ordered = sorted(((name.upper(), display, entries) for name, display, entries in packages), key=lambda p: p[0])
    if len(ordered) != len(table['packages']):
        raise ValueError(f'the table describes {len(table["packages"])} packages, the disc holds {len(ordered)}')
    outputs['installed-content.bin'] = content_records(title_id, [(name, display) for name, display, _ in ordered])
    for number, ((_, _, entries), described) in enumerate(zip(ordered, table['packages']), 1):
        outputs[f'installed-package-{number}.bin'] = metadata_catalog('content:', entries, described['times'])
    return outputs


def write_outputs(output: Path, outputs: dict[str, bytes], expected: dict, times_sha256: str) -> dict:
    """Writes the catalogs and report.json into a new folder. status is 'differs' if an expected hash is not met."""
    output.mkdir(parents=True)
    report = {'schema': 1, 'status': 'complete', 'times_sha256': times_sha256, 'outputs': {}}
    for name, data in outputs.items():
        (output / name).write_bytes(data)
        report['outputs'][name] = {'bytes': len(data), 'sha256': sha(data)}
        if name in expected and expected[name] != sha(data):
            report['status'] = 'differs'
            report['outputs'][name]['expected_sha256'] = expected[name]
    if set(expected) - set(outputs):
        report['status'] = 'differs'
        report['missing'] = sorted(set(expected) - set(outputs))
    (output / 'report.json').write_text(json.dumps(report, indent=1, sort_keys=True) + '\n', encoding='utf-8', newline='\n')
    return report


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--play', type=Path, required=True)
    ap.add_argument('--inventory', type=Path, required=True)
    ap.add_argument('--times', type=Path, required=True)
    ap.add_argument('--title-id', type=lambda s: int(s, 0), required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    from disc_builder import XisoDisc, check_private_path
    from stfs_inventory import Package
    output = args.output.absolute()
    check_private_path(output, 'catalog stage output')
    table = load_times(args.times)
    inventory = json.loads((args.inventory / 'inventory.json').read_text())
    packages = []
    for item in inventory.get('packages') or []:
        path = (args.inventory / item['file']).resolve(strict=True)
        if not path.is_relative_to(args.inventory.resolve()):
            raise ValueError('package path refused')
        with Package(path, args.title_id) as package:
            entries = [(e.path, e.size, e.directory) for e in package.entries]
            package.check_unchanged()
            packages.append((item['origin'].rsplit('/', 1)[-1], package.display_bytes, entries))
    outputs = build(disc_listing(XisoDisc(args.play)), packages, table, args.title_id)
    report = write_outputs(output, outputs, table.get('expected') or {}, sha(args.times.read_bytes()))
    print('CATALOG_RESULT', report['status'], ' '.join(f'{n}={v["sha256"][:12]}' for n, v in report['outputs'].items()), flush=True)
    return 0 if report['status'] == 'complete' else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (ValueError, OSError) as error:
        print('CATALOG_STAGE_REFUSED:', error, file=sys.stderr)
        sys.exit(2)
