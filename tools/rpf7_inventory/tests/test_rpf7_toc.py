from __future__ import annotations

import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


TOOL_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOL_DIRECTORY))

import rpf7_crypto  # noqa: E402
import rpf7_toc  # noqa: E402


def record(
    *,
    resource: bool,
    offset_blocks: int,
    stored_size: int,
    name_offset: int,
    field5: int,
    field6: int,
) -> bytes:
    resource_and_offset = offset_blocks | (0x800000 if resource else 0)
    return (
        resource_and_offset.to_bytes(3, "big")
        + stored_size.to_bytes(3, "big")
        + name_offset.to_bytes(2, "big")
        + struct.pack(">II", field5, field6)
    )


def synthetic_archive(*, encryption_tag: int = 0, names: bytes = b"root\0item.bin\0") -> bytes:
    entries = b"".join(
        (
            record(
                resource=False,
                offset_blocks=0x7FFFFF,
                stored_size=0,
                name_offset=0,
                field5=1,
                field6=1,
            ),
            record(
                resource=False,
                offset_blocks=1,
                stored_size=4,
                name_offset=5,
                field5=8,
                field6=1,
            ),
        )
    )
    names_info = 0x80000000 | len(names)
    header = struct.pack(">4sIII", b"RPF7", 2, names_info, encryption_tag)
    metadata = header + entries + names
    return metadata + bytes(512 - len(metadata)) + b"data"


def synthetic_aes_encrypted_archive() -> bytes:
    """Two synthetic records encrypted with AES-256-ECB under an all-zero key."""

    entries = bytes.fromhex(
        "9d61f7acfbed7a93d8e3011641767228"
        "fd5d906c845cb2ab6ca3479ad84854a1"
    )
    names = bytes.fromhex("5d901f687bd4b45231692ec55f6cc6c8")
    header = struct.pack(">4sIII", b"RPF7", 2, 0x80000010, 0x0FFFFFF7)
    metadata = header + entries + names
    return metadata + bytes(512 - len(metadata)) + b"data"


class StaticKeyProvider:
    def __init__(self, key: bytes | None):
        self.key = key
        self.calls = 0

    def aes256_key_for(self, archive: Path, encryption_tag: int) -> bytes | None:
        del archive, encryption_tag
        self.calls += 1
        return self.key


class HeaderLimitTests(unittest.TestCase):
    def test_rejects_entry_count_before_allocation(self) -> None:
        header = struct.pack(">4sIII", b"RPF7", 101, 0x80000001, 0)
        limits = rpf7_toc.TocLimits(max_entries=100)
        with self.assertRaisesRegex(rpf7_toc.Rpf7TocError, "entry count"):
            rpf7_toc.parse_toc_header(header, 10_000, limits)


class TocParserTests(unittest.TestCase):
    def test_parses_plaintext_to_aggregate_summary_only(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            archive = Path(temporary_directory) / "synthetic.rpf"
            archive.write_bytes(synthetic_archive())
            result = rpf7_toc.summarize_file_toc(archive, rpf7_crypto.NoKeyProvider())

        self.assertEqual(result["directory_count"], 1)
        self.assertEqual(result["binary_file_count"], 1)
        self.assertEqual(result["compressed_binary_count"], 1)
        self.assertEqual(result["encrypted_file_flag_count"], 1)
        self.assertFalse(result["table_decrypted"])
        self.assertFalse(result["names_emitted"])
        self.assertNotIn("entries", result)
        self.assertNotIn("names", result)

    def test_encrypted_path_obtains_key_and_decrypts_each_table_separately(self) -> None:
        calls: list[int] = []

        def identity_decryptor(data: bytes, key: bytes) -> bytes:
            self.assertEqual(key, bytes(32))
            calls.append(len(data))
            return data

        provider = StaticKeyProvider(bytes(32))
        with tempfile.TemporaryDirectory() as temporary_directory:
            archive = Path(temporary_directory) / "synthetic.rpf"
            archive.write_bytes(synthetic_archive(encryption_tag=0x0FFFFFF7))
            result = rpf7_toc.summarize_file_toc(
                archive,
                provider,
                decryptor=identity_decryptor,
            )

        self.assertEqual(provider.calls, 1)
        self.assertEqual(calls, [32, len(b"root\0item.bin\0")])
        self.assertTrue(result["table_decrypted"])
        self.assertFalse(result["key_material_emitted"])

    def test_aes_decryptor_and_parser_integrate_on_synthetic_tables(self) -> None:
        provider = StaticKeyProvider(bytes(32))
        with tempfile.TemporaryDirectory() as temporary_directory:
            archive = Path(temporary_directory) / "synthetic.rpf"
            archive.write_bytes(synthetic_aes_encrypted_archive())
            result = rpf7_toc.summarize_file_toc(archive, provider)

        self.assertTrue(result["table_decrypted"])
        self.assertEqual(result["directory_count"], 1)
        self.assertEqual(result["binary_file_count"], 1)
        self.assertEqual(result["names_validated"], 2)

    def test_encrypted_path_fails_closed_without_key(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            archive = Path(temporary_directory) / "synthetic.rpf"
            archive.write_bytes(synthetic_archive(encryption_tag=0x0FFFFFF7))
            with self.assertRaisesRegex(rpf7_crypto.KeyMaterialError, "no AES-256"):
                rpf7_toc.summarize_file_toc(archive, rpf7_crypto.NoKeyProvider())

    def test_rejects_unbounded_name(self) -> None:
        payload = synthetic_archive(names=b"root\0unterminated")
        with tempfile.TemporaryDirectory() as temporary_directory:
            archive = Path(temporary_directory) / "synthetic.rpf"
            archive.write_bytes(payload)
            with self.assertRaisesRegex(rpf7_toc.Rpf7TocError, "null-terminated"):
                rpf7_toc.summarize_file_toc(archive, rpf7_crypto.NoKeyProvider())

    def test_ng_family_remains_explicitly_unsupported(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            archive = Path(temporary_directory) / "synthetic.rpf"
            archive.write_bytes(synthetic_archive(encryption_tag=0x0FEFFFFF))
            with self.assertRaisesRegex(rpf7_toc.UnsupportedEncryptionError, "NG/TFIT"):
                rpf7_toc.summarize_file_toc(archive, rpf7_crypto.NoKeyProvider())

    def test_cli_reports_missing_key_without_names_or_key_data(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            archive = Path(temporary_directory) / "synthetic.rpf"
            archive.write_bytes(synthetic_archive(encryption_tag=0x0FFFFFF7))
            completed = subprocess.run(
                [
                    sys.executable,
                    str(TOOL_DIRECTORY / "rpf7_inventory.py"),
                    str(archive),
                    "--toc-summary",
                ],
                check=False,
                capture_output=True,
                text=True,
            )

        self.assertEqual(completed.returncode, 1, completed.stderr)
        result = json.loads(completed.stdout)
        self.assertEqual(result["toc_summaries_blocked"], 1)
        summary = result["archives"][0]["toc_summary"]
        self.assertEqual(summary["status"], "blocked")
        self.assertFalse(summary["names_emitted"])
        self.assertFalse(summary["key_material_emitted"])


if __name__ == "__main__":
    unittest.main()
