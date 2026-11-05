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
from rpf7_toc import TocHeader  # noqa: E402


def record(
    *, resource: bool, offset: int, size: int, name_offset: int, field5: int, field6: int
) -> bytes:
    first = offset | (0x800000 if resource else 0)
    return (
        first.to_bytes(3, "big")
        + size.to_bytes(3, "big")
        + name_offset.to_bytes(2, "big")
        + struct.pack(">II", field5, field6)
    )


class CatalogTests(unittest.TestCase):
    def test_resource_size_decodes_weighted_pages(self) -> None:
        self.assertEqual(rpf7_catalog.resource_size(1 << 17), 16 * 0x200)
        self.assertEqual(rpf7_catalog.resource_size((1 << 27) | 1), 0x400)

    def test_builds_paths_and_metadata(self) -> None:
        names = b"root\0dir\0file.bin\0thing.ytd\0"
        entries = b"".join(
            (
                record(resource=False, offset=0x7FFFFF, size=0, name_offset=0, field5=1, field6=2),
                record(resource=False, offset=0x7FFFFF, size=0, name_offset=5, field5=3, field6=1),
                record(resource=False, offset=4, size=8, name_offset=9, field5=16, field6=1),
                record(resource=True, offset=5, size=32, name_offset=18, field5=1 << 17, field6=1 << 17),
            )
        )
        header = TocHeader(4, len(names), 0, 0, 4096)
        result = rpf7_catalog.parse_catalog_entries(
            entries, names, header=header, decrypted=False
        )

        self.assertEqual(
            [entry.path for entry in result],
            ["", "dir", "file.bin", "dir/thing.ytd"],
        )
        self.assertEqual(result[2].payload_offset, 4 * 512)
        self.assertEqual(result[2].logical_size, 16)
        self.assertTrue(result[2].compressed)
        self.assertTrue(result[2].encrypted)
        self.assertEqual(result[3].system_size, 16 * 0x200)
        self.assertEqual(result[3].logical_size, 2 * 16 * 0x200)

    def test_rejects_unsafe_name_component(self) -> None:
        names = b"root\0../escape\0"
        entries = b"".join(
            (
                record(resource=False, offset=0x7FFFFF, size=0, name_offset=0, field5=1, field6=1),
                record(resource=False, offset=1, size=4, name_offset=5, field5=4, field6=0),
            )
        )
        header = TocHeader(2, len(names), 0, 0, 1024)
        with self.assertRaisesRegex(rpf7_catalog.Rpf7CatalogError, "unsafe"):
            rpf7_catalog.parse_catalog_entries(
                entries, names, header=header, decrypted=False
            )

    def test_allows_empty_root_name_only(self) -> None:
        names = b"\0file.bin\0"
        entries = b"".join(
            (
                record(resource=False, offset=0x7FFFFF, size=0, name_offset=0, field5=1, field6=1),
                record(resource=False, offset=1, size=4, name_offset=1, field5=4, field6=0),
            )
        )
        header = TocHeader(2, len(names), 0, 0, 1024)
        result = rpf7_catalog.parse_catalog_entries(
            entries, names, header=header, decrypted=False
        )
        self.assertEqual(result[0].name, "")
        self.assertEqual(result[1].path, "file.bin")

    def test_writes_private_sqlite_catalog(self) -> None:
        entry = rpf7_catalog.CatalogEntry(
            0, None, "directory", "root", "", 0, None, None, None, None,
            None, 1, 0, None, None, None, None
        )
        header = TocHeader(1, 5, 0, 0, 512)
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "catalog.sqlite3"
            rpf7_catalog.write_catalog(output, Path("sample.rpf"), header, [entry])
            mode = output.stat().st_mode & 0o777
            with sqlite3.connect(output) as connection:
                row = connection.execute(
                    "SELECT kind, path FROM entries WHERE entry_index = 0"
                ).fetchone()
                archive_row = connection.execute(
                    "SELECT archive_id, depth, entry_count FROM archives"
                ).fetchone()
            self.assertEqual(mode, 0o600)
            self.assertEqual(row, ("directory", ""))
            self.assertEqual(archive_row, (0, 0, 1))
            with self.assertRaises(FileExistsError):
                rpf7_catalog.write_catalog(output, Path("sample.rpf"), header, [entry])


if __name__ == "__main__":
    unittest.main()
