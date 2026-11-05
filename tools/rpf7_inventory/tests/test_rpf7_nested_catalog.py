from __future__ import annotations

from pathlib import Path
import sqlite3
import struct
import sys
import tempfile
import unittest


TOOL_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOL_DIRECTORY))

import rpf7_catalog  # noqa: E402
import rpf7_crypto  # noqa: E402
import rpf7_nested_catalog  # noqa: E402
from rpf7_toc import TocHeader, read_bytes_toc  # noqa: E402


def record(
    *, offset: int, stored_size: int, name_offset: int, field5: int, field6: int
) -> bytes:
    return (
        offset.to_bytes(3, "big")
        + stored_size.to_bytes(3, "big")
        + name_offset.to_bytes(2, "big")
        + struct.pack(">II", field5, field6)
    )


def leaf_archive() -> bytes:
    names = b"root\0"
    entries = record(
        offset=0x7FFFFF,
        stored_size=0,
        name_offset=0,
        field5=0,
        field6=0,
    )
    metadata = struct.pack(">4sIII", b"RPF7", 1, 0x80000000 | len(names), 0)
    metadata += entries + names
    return metadata + bytes(512 - len(metadata))


def archive_with_child(
    child: bytes,
    *,
    stored_size: int = 0,
    logical_size: int | None = None,
) -> bytes:
    names = b"root\0nested.rpf\0"
    logical_size = len(child) if logical_size is None else logical_size
    entries = b"".join(
        (
            record(
                offset=0x7FFFFF,
                stored_size=0,
                name_offset=0,
                field5=1,
                field6=1,
            ),
            record(
                offset=1,
                stored_size=stored_size,
                name_offset=5,
                field5=logical_size,
                field6=0,
            ),
        )
    )
    metadata = struct.pack(">4sIII", b"RPF7", 2, 0x80000000 | len(names), 0)
    metadata += entries + names
    return metadata + bytes(512 - len(metadata)) + child


def parsed_root(data: bytes) -> tuple[TocHeader, list[rpf7_catalog.CatalogEntry]]:
    header, entries, names, decrypted = read_bytes_toc(
        data,
        rpf7_crypto.NoKeyProvider(),
        archive_identity=Path("synthetic.rpf"),
    )
    return header, rpf7_catalog.parse_catalog_entries(
        entries, names, header=header, decrypted=decrypted
    )


class NestedCatalogTests(unittest.TestCase):
    def test_catalogs_one_uncompressed_nested_archive(self) -> None:
        outer = archive_with_child(leaf_archive())
        header, entries = parsed_root(outer)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "outer.rpf"
            path.write_bytes(outer)
            nested = rpf7_nested_catalog.scan_nested_archives(
                path, header, entries, rpf7_crypto.NoKeyProvider()
            )
        self.assertEqual(len(nested), 1)
        self.assertEqual(nested[0].depth, 1)
        self.assertEqual(nested[0].header.entry_count, 1)
        self.assertEqual(len(nested[0].entries), 1)

    def test_persists_nested_metadata_in_private_catalog(self) -> None:
        outer = archive_with_child(leaf_archive())
        header, entries = parsed_root(outer)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "outer.rpf"
            output = Path(temporary) / "catalog.sqlite3"
            path.write_bytes(outer)
            nested = rpf7_nested_catalog.scan_nested_archives(
                path, header, entries, rpf7_crypto.NoKeyProvider()
            )
            rpf7_catalog.write_catalog(output, path, header, entries, nested)
            with sqlite3.connect(output) as connection:
                archive_count = connection.execute(
                    "SELECT COUNT(*) FROM archives"
                ).fetchone()[0]
                nested_entry_count = connection.execute(
                    "SELECT COUNT(*) FROM nested_entries"
                ).fetchone()[0]
                schema_version = connection.execute(
                    "SELECT value FROM metadata WHERE key='schema_version'"
                ).fetchone()[0]
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)
        self.assertEqual(archive_count, 2)
        self.assertEqual(nested_entry_count, 1)
        self.assertEqual(schema_version, "2")

    def test_depth_and_size_limits_fail_closed_before_decode(self) -> None:
        outer = archive_with_child(leaf_archive())
        header, entries = parsed_root(outer)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "outer.rpf"
            path.write_bytes(outer)
            with self.assertRaisesRegex(
                rpf7_nested_catalog.NestedCatalogError, "depth"
            ):
                rpf7_nested_catalog.scan_nested_archives(
                    path,
                    header,
                    entries,
                    rpf7_crypto.NoKeyProvider(),
                    limits=rpf7_nested_catalog.NestedCatalogLimits(max_depth=0),
                )
            with self.assertRaisesRegex(
                rpf7_nested_catalog.NestedCatalogError, "per-file"
            ):
                rpf7_nested_catalog.scan_nested_archives(
                    path,
                    header,
                    entries,
                    rpf7_crypto.NoKeyProvider(),
                    limits=rpf7_nested_catalog.NestedCatalogLimits(
                        max_single_archive_size=128
                    ),
                )
            with self.assertRaisesRegex(
                rpf7_nested_catalog.NestedCatalogError, "total-size"
            ):
                rpf7_nested_catalog.scan_nested_archives(
                    path,
                    header,
                    entries,
                    rpf7_crypto.NoKeyProvider(),
                    limits=rpf7_nested_catalog.NestedCatalogLimits(
                        max_total_decoded_size=128
                    ),
                )

    def test_detects_archive_cycle_from_decoder_output(self) -> None:
        outer = archive_with_child(b"x", stored_size=1, logical_size=513)
        self.assertEqual(len(outer), 513)
        header, entries = parsed_root(outer)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "outer.rpf"
            path.write_bytes(outer)
            with self.assertRaisesRegex(
                rpf7_nested_catalog.NestedCatalogError, "cycle"
            ):
                rpf7_nested_catalog.scan_nested_archives(
                    path,
                    header,
                    entries,
                    rpf7_crypto.NoKeyProvider(),
                    xmem_decoder=lambda payload, expected, limit: outer,
                )


if __name__ == "__main__":
    unittest.main()
