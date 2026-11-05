from __future__ import annotations

import os
from pathlib import Path
import sys
import tempfile
import unittest


TOOL_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOL_DIRECTORY))

import rpf7_crypto  # noqa: E402


class AesTests(unittest.TestCase):
    def test_decrypts_nist_aes256_known_answer(self) -> None:
        key = bytes.fromhex(
            "000102030405060708090a0b0c0d0e0f"
            "101112131415161718191a1b1c1d1e1f"
        )
        ciphertext = bytes.fromhex("8ea2b7ca516745bfeafc49904b496089")
        expected = bytes.fromhex("00112233445566778899aabbccddeeff")
        self.assertEqual(rpf7_crypto.decrypt_aes256_block(ciphertext, key), expected)

    def test_ecb_prefix_preserves_partial_tail(self) -> None:
        key = bytes.fromhex(
            "000102030405060708090a0b0c0d0e0f"
            "101112131415161718191a1b1c1d1e1f"
        )
        ciphertext = bytes.fromhex("8ea2b7ca516745bfeafc49904b496089") + b"tail"
        plaintext = rpf7_crypto.decrypt_aes256_ecb_prefix(ciphertext, key)
        self.assertEqual(plaintext[-4:], b"tail")
        self.assertEqual(plaintext[:16].hex(), "00112233445566778899aabbccddeeff")

    def test_rejects_wrong_key_size(self) -> None:
        with self.assertRaisesRegex(rpf7_crypto.KeyMaterialError, "32"):
            rpf7_crypto.decrypt_aes256_ecb_prefix(bytes(16), bytes(31))


class ExplicitKeyFileProviderTests(unittest.TestCase):
    def test_reads_exact_raw_key_without_returning_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            key_path = Path(temporary_directory) / "private.key"
            key_path.write_bytes(bytes(range(32)))
            provider = rpf7_crypto.ExplicitKeyFileProvider(key_path)
            self.assertEqual(
                provider.aes256_key_for(Path("archive.rpf"), 0x0FFFFFF7),
                bytes(range(32)),
            )

    def test_rejects_wrong_size(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            key_path = Path(temporary_directory) / "private.key"
            key_path.write_bytes(bytes(31))
            provider = rpf7_crypto.ExplicitKeyFileProvider(key_path)
            with self.assertRaisesRegex(rpf7_crypto.KeyMaterialError, "exactly 32"):
                provider.aes256_key_for(Path("archive.rpf"), 0x0FFFFFF7)

    @unittest.skipUnless(hasattr(os, "symlink"), "symbolic links are unavailable")
    def test_rejects_symlink(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            actual = root / "private.key"
            link = root / "linked.key"
            actual.write_bytes(bytes(32))
            try:
                link.symlink_to(actual)
            except OSError as error:
                self.skipTest(f"cannot create symbolic link: {error}")
            provider = rpf7_crypto.ExplicitKeyFileProvider(link)
            with self.assertRaisesRegex(rpf7_crypto.KeyMaterialError, "symbolic link"):
                provider.aes256_key_for(Path("archive.rpf"), 0x0FFFFFF7)


if __name__ == "__main__":
    unittest.main()
