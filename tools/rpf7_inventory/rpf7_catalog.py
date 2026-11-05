#!/usr/bin/env python3
"""Create a private SQLite catalog from one validated RPF7 archive."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path, PurePosixPath
import sqlite3
import struct
import subprocess
import sys
from typing import Sequence, TYPE_CHECKING

from rpf7_crypto import ExplicitKeyFileProvider, KeyMaterialError
from rpf7_toc import (
    ENTRY_SIZE,
    Rpf7TocError,
    TocHeader,
    TocLimits,
    parse_toc_buffers,
    read_file_toc,
)

if TYPE_CHECKING:
    from rpf7_nested_catalog import NestedArchive


BLOCK_SIZE = 512


class Rpf7CatalogError(ValueError):
    """Raised when a safe, unambiguous private catalog cannot be created."""


@dataclass(frozen=True)
class CatalogEntry:
    index: int
    parent_index: int | None
    kind: str
    name: str
    path: str
    name_offset: int
    payload_offset: int | None
    stored_size: int | None
    logical_size: int | None
    compressed: bool | None
    encrypted: bool | None
    first_child: int | None
    child_count: int | None
    system_flags: int | None
    graphics_flags: int | None
    system_size: int | None
    graphics_size: int | None


def resource_size(flags: int) -> int:
    """Decode an RPF7 virtual/physical page-size flag word."""

    base_size = 0x200 << (flags & 0xF)
    weighted_pages = (
        (((flags >> 27) & 0x1) << 0)
        + (((flags >> 26) & 0x1) << 1)
        + (((flags >> 25) & 0x1) << 2)
        + (((flags >> 24) & 0x1) << 3)
        + (((flags >> 17) & 0x7F) << 4)
        + (((flags >> 11) & 0x3F) << 5)
        + (((flags >> 7) & 0xF) << 6)
        + (((flags >> 5) & 0x3) << 7)
        + (((flags >> 4) & 0x1) << 8)
    )
    return base_size * weighted_pages


def _decode_name(names: bytes, offset: int, limit: int, index: int) -> str:
    end = names.find(b"\0", offset, min(len(names), offset + limit + 1))
    if end < 0:
        raise Rpf7CatalogError(f"entry {index} has no bounded name terminator")
    raw = names[offset:end]
    try:
        name = raw.decode("utf-8")
    except UnicodeDecodeError as error:
        raise Rpf7CatalogError(f"entry {index} name is not UTF-8") from error
    if (
        (not name and index != 0)
        or name in {".", ".."}
        or "/" in name
        or "\\" in name
    ):
        raise Rpf7CatalogError(f"entry {index} has an unsafe path component")
    return name


def parse_catalog_entries(
    entries: bytes,
    names: bytes,
    *,
    header: TocHeader,
    limits: TocLimits = TocLimits(),
    decrypted: bool,
) -> list[CatalogEntry]:
    """Build validated paths and metadata; caller keeps the result private."""

    parse_toc_buffers(
        entries,
        names,
        header=header,
        limits=limits,
        decrypted=decrypted,
    )

    raw_records: list[dict[str, object]] = []
    for index in range(header.entry_count):
        record = entries[index * ENTRY_SIZE : (index + 1) * ENTRY_SIZE]
        resource_and_offset = int.from_bytes(record[0:3], "big")
        stored_size = int.from_bytes(record[3:6], "big")
        name_offset = int.from_bytes(record[6:8], "big") << header.name_shift
        field5, field6 = struct.unpack(">II", record[8:16])
        offset_blocks = resource_and_offset & 0x7FFFFF
        name = _decode_name(names, name_offset, limits.max_name_bytes, index)
        if offset_blocks == 0x7FFFFF:
            kind = "directory"
        elif resource_and_offset & 0x800000:
            kind = "resource"
        else:
            kind = "binary"
        raw_records.append(
            {
                "kind": kind,
                "name": name,
                "name_offset": name_offset,
                "offset_blocks": offset_blocks,
                "stored_size": stored_size,
                "field5": field5,
                "field6": field6,
            }
        )

    if raw_records[0]["kind"] != "directory":
        raise Rpf7CatalogError("first entry is not a directory")
    parents: list[int | None] = [None] * header.entry_count
    paths: list[str | None] = [None] * header.entry_count
    paths[0] = ""
    pending = [0]
    while pending:
        directory_index = pending.pop()
        directory = raw_records[directory_index]
        first = int(directory["field5"])
        count = int(directory["field6"])
        parent_path = paths[directory_index]
        assert parent_path is not None
        for child_index in range(first, first + count):
            if child_index == 0 or parents[child_index] is not None:
                raise Rpf7CatalogError(
                    f"entry {child_index} has multiple parents or forms a cycle"
                )
            parents[child_index] = directory_index
            child_name = str(raw_records[child_index]["name"])
            paths[child_index] = str(PurePosixPath(parent_path) / child_name)
            if raw_records[child_index]["kind"] == "directory":
                pending.append(child_index)

    unreachable = [index for index, path in enumerate(paths) if path is None]
    if unreachable:
        raise Rpf7CatalogError(
            f"archive contains {len(unreachable)} entries unreachable from the root"
        )

    result: list[CatalogEntry] = []
    for index, raw in enumerate(raw_records):
        kind = str(raw["kind"])
        stored_size = int(raw["stored_size"])
        field5 = int(raw["field5"])
        field6 = int(raw["field6"])
        common = {
            "index": index,
            "parent_index": parents[index],
            "kind": kind,
            "name": str(raw["name"]),
            "path": str(paths[index]),
            "name_offset": int(raw["name_offset"]),
        }
        if kind == "directory":
            result.append(
                CatalogEntry(
                    **common,
                    payload_offset=None,
                    stored_size=None,
                    logical_size=None,
                    compressed=None,
                    encrypted=None,
                    first_child=field5,
                    child_count=field6,
                    system_flags=None,
                    graphics_flags=None,
                    system_size=None,
                    graphics_size=None,
                )
            )
            continue

        system_flags = field5 if kind == "resource" else None
        graphics_flags = field6 if kind == "resource" else None
        system_size = resource_size(field5) if kind == "resource" else None
        graphics_size = resource_size(field6) if kind == "resource" else None
        logical_size = (
            system_size + graphics_size if kind == "resource" else field5
        )
        result.append(
            CatalogEntry(
                **common,
                payload_offset=int(raw["offset_blocks"]) * BLOCK_SIZE,
                stored_size=(stored_size if stored_size else logical_size),
                logical_size=logical_size,
                compressed=(stored_size != 0),
                encrypted=(field6 == 1 if kind == "binary" else None),
                first_child=None,
                child_count=None,
                system_flags=system_flags,
                graphics_flags=graphics_flags,
                system_size=system_size,
                graphics_size=graphics_size,
            )
        )
    return result


def write_catalog(
    output: Path,
    archive: Path,
    header: TocHeader,
    entries: Sequence[CatalogEntry],
    nested_archives: Sequence["NestedArchive"] = (),
) -> None:
    """Create a mode-0600 SQLite catalog without overwriting an existing file."""

    output.parent.mkdir(parents=True, exist_ok=True)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(output, flags, 0o600)
    os.close(descriptor)
    connection: sqlite3.Connection | None = None
    try:
        connection = sqlite3.connect(output)
        with connection:
            connection.executescript(
                """
                PRAGMA journal_mode=DELETE;
                CREATE TABLE metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL);
                CREATE TABLE entries (
                    entry_index INTEGER PRIMARY KEY,
                    parent_index INTEGER,
                    kind TEXT NOT NULL,
                    name TEXT NOT NULL,
                    path TEXT NOT NULL UNIQUE,
                    name_offset INTEGER NOT NULL,
                    payload_offset INTEGER,
                    stored_size INTEGER,
                    logical_size INTEGER,
                    compressed INTEGER,
                    encrypted INTEGER,
                    first_child INTEGER,
                    child_count INTEGER,
                    system_flags INTEGER,
                    graphics_flags INTEGER,
                    system_size INTEGER,
                    graphics_size INTEGER
                );
                CREATE INDEX entries_parent_index ON entries(parent_index);
                CREATE INDEX entries_kind ON entries(kind);
                CREATE TABLE archives (
                    archive_id INTEGER PRIMARY KEY,
                    parent_archive_id INTEGER,
                    source_entry_index INTEGER,
                    depth INTEGER NOT NULL,
                    path TEXT NOT NULL UNIQUE,
                    archive_size INTEGER NOT NULL,
                    entry_count INTEGER NOT NULL,
                    encryption_tag TEXT NOT NULL
                );
                CREATE TABLE nested_entries (
                    archive_id INTEGER NOT NULL,
                    entry_index INTEGER NOT NULL,
                    parent_index INTEGER,
                    kind TEXT NOT NULL,
                    name TEXT NOT NULL,
                    path TEXT NOT NULL,
                    name_offset INTEGER NOT NULL,
                    payload_offset INTEGER,
                    stored_size INTEGER,
                    logical_size INTEGER,
                    compressed INTEGER,
                    encrypted INTEGER,
                    first_child INTEGER,
                    child_count INTEGER,
                    system_flags INTEGER,
                    graphics_flags INTEGER,
                    system_size INTEGER,
                    graphics_size INTEGER,
                    PRIMARY KEY (archive_id, entry_index),
                    FOREIGN KEY (archive_id) REFERENCES archives(archive_id)
                );
                CREATE INDEX nested_entries_kind ON nested_entries(kind);
                """
            )
            connection.executemany(
                "INSERT INTO metadata(key, value) VALUES (?, ?)",
                (
                    ("schema_version", "2"),
                    ("archive_filename", archive.name),
                    ("archive_size", str(header.file_size)),
                    ("encryption_tag", f"0x{header.encryption_tag:08X}"),
                ),
            )
            connection.executemany(
                "INSERT INTO entries VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                [
                    (
                        entry.index,
                        entry.parent_index,
                        entry.kind,
                        entry.name,
                        entry.path,
                        entry.name_offset,
                        entry.payload_offset,
                        entry.stored_size,
                        entry.logical_size,
                        entry.compressed,
                        entry.encrypted,
                        entry.first_child,
                        entry.child_count,
                        entry.system_flags,
                        entry.graphics_flags,
                        entry.system_size,
                        entry.graphics_size,
                    )
                    for entry in entries
                ],
            )
            connection.execute(
                "INSERT INTO archives VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                (
                    0,
                    None,
                    None,
                    0,
                    "",
                    header.file_size,
                    header.entry_count,
                    f"0x{header.encryption_tag:08X}",
                ),
            )
            for nested in nested_archives:
                nested_header = nested.header
                connection.execute(
                    "INSERT INTO archives VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                    (
                        nested.archive_id,
                        nested.parent_archive_id,
                        nested.source_entry_index,
                        nested.depth,
                        nested.path,
                        nested_header.file_size,
                        nested_header.entry_count,
                        f"0x{nested_header.encryption_tag:08X}",
                    ),
                )
                connection.executemany(
                    "INSERT INTO nested_entries VALUES "
                    "(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                    [
                        (
                            nested.archive_id,
                            entry.index,
                            entry.parent_index,
                            entry.kind,
                            entry.name,
                            entry.path,
                            entry.name_offset,
                            entry.payload_offset,
                            entry.stored_size,
                            entry.logical_size,
                            entry.compressed,
                            entry.encrypted,
                            entry.first_child,
                            entry.child_count,
                            entry.system_flags,
                            entry.graphics_flags,
                            entry.system_size,
                            entry.graphics_size,
                        )
                        for entry in nested.entries
                    ],
                )
        connection.close()
        connection = None
    except BaseException:
        if connection is not None:
            connection.close()
        output.unlink(missing_ok=True)
        raise


def _require_private_output(path: Path, repository: Path) -> Path:
    output = path.expanduser().resolve()
    private_root = (repository / "local-analysis").resolve()
    try:
        output.relative_to(private_root)
    except ValueError as error:
        raise Rpf7CatalogError(f"catalog must remain below {private_root}") from error
    ignored = subprocess.run(
        ["git", "check-ignore", "--quiet", "--no-index", str(output)],
        cwd=repository,
        check=False,
    )
    if ignored.returncode != 0:
        raise Rpf7CatalogError("catalog path is not covered by Git ignore rules")
    return output


def repository_root() -> Path:
    return Path(__file__).resolve().parents[2]


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("private_key_file", type=Path)
    parser.add_argument("private_catalog", type=Path)
    parser.add_argument(
        "--recursive", action="store_true", help="catalog binary .rpf entries recursively"
    )
    parser.add_argument("--max-depth", type=int, default=4)
    parser.add_argument("--max-nested-archives", type=int, default=64)
    parser.add_argument("--max-single-nested-bytes", type=int, default=256 * 1024 * 1024)
    parser.add_argument("--max-total-nested-bytes", type=int, default=512 * 1024 * 1024)
    parser.add_argument(
        "--xmem-decoder",
        type=Path,
        help="built tools/xmem_lzx executable for compressed nested archives",
    )
    args = parser.parse_args(argv)

    try:
        output = _require_private_output(args.private_catalog, repository_root())
        key_provider = ExplicitKeyFileProvider(args.private_key_file)
        header, entry_buffer, name_buffer, decrypted = read_file_toc(
            args.archive, key_provider
        )
        entries = parse_catalog_entries(
            entry_buffer,
            name_buffer,
            header=header,
            decrypted=decrypted,
        )
        nested_archives: Sequence["NestedArchive"] = ()
        if args.recursive:
            from rpf7_extract import decode_xmem_with_tool
            from rpf7_nested_catalog import NestedCatalogLimits, scan_nested_archives

            if args.max_depth < 0 or min(
                args.max_nested_archives,
                args.max_single_nested_bytes,
                args.max_total_nested_bytes,
            ) < 1:
                raise Rpf7CatalogError("recursive limits must be positive")
            decoder = (
                args.xmem_decoder.expanduser().absolute()
                if args.xmem_decoder is not None
                else repository_root() / "out/xmem-lzx/xmem-lzx"
            )
            nested_archives = scan_nested_archives(
                args.archive,
                header,
                entries,
                key_provider,
                limits=NestedCatalogLimits(
                    max_depth=args.max_depth,
                    max_archives=args.max_nested_archives,
                    max_single_archive_size=args.max_single_nested_bytes,
                    max_total_decoded_size=args.max_total_nested_bytes,
                ),
                xmem_decoder=lambda data, expected, limit: decode_xmem_with_tool(
                    data, expected, limit, executable=decoder
                ),
            )
        write_catalog(
            output, args.archive.absolute(), header, entries, nested_archives
        )
        counts: dict[str, int] = {}
        for entry in entries:
            counts[entry.kind] = counts.get(entry.kind, 0) + 1
        print(
            json.dumps(
                {
                    "status": "cataloged",
                    "archive_filename": args.archive.name,
                    "entry_count": len(entries),
                    "kind_counts": dict(sorted(counts.items())),
                    "nested_archive_count": len(nested_archives),
                    "nested_entry_count": sum(
                        len(nested.entries) for nested in nested_archives
                    ),
                    "maximum_nested_depth": max(
                        (nested.depth for nested in nested_archives), default=0
                    ),
                    "catalog_filename": output.name,
                    "names_emitted_to_stdout": False,
                    "payload_read": bool(args.recursive),
                    "payload_emitted": False,
                    "key_material_emitted": False,
                },
                indent=2,
            )
        )
        return 0
    except (KeyMaterialError, OSError, Rpf7CatalogError, Rpf7TocError, sqlite3.Error, ValueError) as error:
        print(f"rpf7_catalog: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
