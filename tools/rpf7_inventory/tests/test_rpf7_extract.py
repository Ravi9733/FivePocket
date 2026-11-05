from __future__ import annotations

from pathlib import Path
import sys
import tempfile
import unittest


TOOL_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOL_DIRECTORY))

import rpf7_extract  # noqa: E402


class ExtractionTests(unittest.TestCase):
    def test_decodes_bounded_xmem_callback(self) -> None:
        expected = b"metadata" * 40
        calls: list[tuple[bytes, int, int]] = []

        def xmem(payload: bytes, size: int, limit: int) -> bytes:
            calls.append((payload, size, limit))
            return expected

        result = rpf7_extract.decode_binary_payload(
            b"framed-lzx",
            encrypted=False,
            compressed=True,
            logical_size=len(expected),
            key=None,
            xmem_decoder=xmem,
        )
        self.assertEqual(result, expected)
        self.assertEqual(calls, [(b"framed-lzx", len(expected), 512 * 1024 * 1024)])

    def test_decrypts_before_decompression(self) -> None:
        expected = b"shader" * 20
        calls: list[tuple[bytes, bytes]] = []

        def decryptor(payload: bytes, key: bytes) -> bytes:
            calls.append((payload, key))
            return b"decrypted-xmem"

        result = rpf7_extract.decode_binary_payload(
            b"ciphertext",
            encrypted=True,
            compressed=True,
            logical_size=len(expected),
            key=bytes(32),
            decryptor=decryptor,
            xmem_decoder=lambda payload, size, limit: (
                expected
                if payload == b"decrypted-xmem" and size == len(expected) and limit > size
                else b""
            ),
        )
        self.assertEqual(result, expected)
        self.assertEqual(calls, [(b"ciphertext", bytes(32))])

    def test_compressed_entry_requires_xmem_decoder(self) -> None:
        with self.assertRaisesRegex(rpf7_extract.Rpf7ExtractionError, "XMem/LZX"):
            rpf7_extract.decode_binary_payload(
                b"compressed",
                encrypted=False,
                compressed=True,
                logical_size=20,
                key=None,
            )

    def test_rejects_size_mismatch_and_output_limit(self) -> None:
        with self.assertRaisesRegex(rpf7_extract.Rpf7ExtractionError, "does not match"):
            rpf7_extract.decode_binary_payload(
                b"abc", encrypted=False, compressed=False, logical_size=4, key=None
            )
        with self.assertRaisesRegex(rpf7_extract.Rpf7ExtractionError, "limit"):
            rpf7_extract.decode_binary_payload(
                b"abc",
                encrypted=False,
                compressed=False,
                logical_size=3,
                key=None,
                max_output_size=2,
            )

    def test_exclusive_output_is_mode_0600(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "private.bin"
            rpf7_extract._write_exclusive(output, b"private")
            self.assertEqual(output.read_bytes(), b"private")
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)
            with self.assertRaises(FileExistsError):
                rpf7_extract._write_exclusive(output, b"replace")


if __name__ == "__main__":
    unittest.main()
