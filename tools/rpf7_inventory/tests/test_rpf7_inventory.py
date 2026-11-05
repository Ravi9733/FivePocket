from __future__ import annotations

import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


TOOL_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOL_DIRECTORY))

import rpf7_inventory  # noqa: E402


def synthetic_rpf7(
    entry_count: int = 2,
    name_table_size: int = 32,
    *,
    platform_bit: int = 1,
    name_shift: int = 0,
    encryption_tag: int = 0,
) -> bytes:
    name_info = (
        ((platform_bit & 1) << 31)
        | ((name_shift & 0x7) << 28)
        | (name_table_size & 0x0FFFFFFF)
    )
    header = struct.pack(">4sIII", b"RPF7", entry_count, name_info, encryption_tag)
    return header + bytes(entry_count * 16 + name_table_size)


class HeaderTests(unittest.TestCase):
    def test_parses_big_endian_console_header(self) -> None:
        payload = synthetic_rpf7(
            entry_count=0x783,
            name_table_size=0x8980,
            name_shift=0,
            encryption_tag=0x0FFFFFF7,
        )

        result = rpf7_inventory.parse_header(payload[:16], len(payload))

        self.assertEqual(result["entry_count"], 0x783)
        self.assertEqual(result["platform_hint"], "xbox-360")
        self.assertEqual(result["name_table_size"], 0x8980)
        self.assertEqual(result["encryption"], "console-aes")
        self.assertFalse(result["toc_plaintext"])
        self.assertFalse(result["decryption_attempted"])
        self.assertFalse(result["entry_records_parsed"])
        self.assertTrue(result["structurally_valid"])

    def test_decodes_name_offset_shift(self) -> None:
        payload = synthetic_rpf7(name_shift=1)
        result = rpf7_inventory.parse_header(payload[:16], len(payload))
        self.assertEqual(result["name_offset_shift"], 1)
        self.assertEqual(result["name_offset_alignment"], 2)

    def test_reports_declared_metadata_outside_file(self) -> None:
        header = synthetic_rpf7(entry_count=10, name_table_size=100)[:16]
        result = rpf7_inventory.parse_header(header, 16)
        self.assertFalse(result["metadata_layout"]["within_file"])
        self.assertFalse(result["structurally_valid"])

    def test_rejects_truncated_and_wrong_magic(self) -> None:
        with self.assertRaisesRegex(rpf7_inventory.Rpf7FormatError, "shorter"):
            rpf7_inventory.parse_header(b"RPF7", 4)
        with self.assertRaisesRegex(rpf7_inventory.Rpf7FormatError, "not a big-endian"):
            rpf7_inventory.parse_header(b"not-rpf7-format!", 16)


class InventoryTests(unittest.TestCase):
    def test_inventory_is_sorted_header_only_and_reports_bad_extension(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            (root / "nested").mkdir()
            (root / "z.rpf").write_bytes(synthetic_rpf7(entry_count=3))
            (root / "nested" / "a.bin").write_bytes(synthetic_rpf7(entry_count=1))
            (root / "not-an-archive.rpf").write_bytes(b"ordinary data")
            result = rpf7_inventory.inventory_path(root)

        self.assertEqual(result["archives_found"], 2)
        self.assertEqual(
            [archive["path"] for archive in result["archives"]],
            ["nested/a.bin", "z.rpf"],
        )
        self.assertEqual(result["errors"][0]["path"], "not-an-archive.rpf")
        self.assertNotIn("sha256", result["archives"][0])

    @unittest.skipUnless(hasattr(os, "symlink"), "symbolic links are unavailable")
    def test_skips_symlinks(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            base = Path(temporary_directory)
            root = base / "dump"
            outside = base / "outside.rpf"
            root.mkdir()
            outside.write_bytes(synthetic_rpf7())
            try:
                (root / "linked.rpf").symlink_to(outside)
            except OSError as error:
                self.skipTest(f"cannot create symbolic link: {error}")
            result = rpf7_inventory.inventory_path(root)

        self.assertEqual(result["archives_found"], 0)
        self.assertEqual(result["skipped_symlinks"], ["linked.rpf"])

    def test_cli_emits_json_for_one_file(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            archive = Path(temporary_directory) / "test.rpf"
            archive.write_bytes(synthetic_rpf7(entry_count=4))
            completed = subprocess.run(
                [sys.executable, str(TOOL_DIRECTORY / "rpf7_inventory.py"), str(archive)],
                check=False,
                capture_output=True,
                text=True,
            )

        self.assertEqual(completed.returncode, 0, completed.stderr)
        result = json.loads(completed.stdout)
        self.assertEqual(result["archives"][0]["path"], "test.rpf")
        self.assertEqual(result["archives"][0]["entry_count"], 4)

    def test_rejects_symlink_input(self) -> None:
        if not hasattr(os, "symlink"):
            self.skipTest("symbolic links are unavailable")
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            actual = root / "actual.rpf"
            link = root / "link.rpf"
            actual.write_bytes(synthetic_rpf7())
            try:
                link.symlink_to(actual)
            except OSError as error:
                self.skipTest(f"cannot create symbolic link: {error}")
            with self.assertRaisesRegex(ValueError, "symbolic link"):
                rpf7_inventory.inventory_path(link)


if __name__ == "__main__":
    unittest.main()
