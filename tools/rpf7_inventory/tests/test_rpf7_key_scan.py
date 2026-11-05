from pathlib import Path
import sys
import unittest


TOOL_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOL_DIRECTORY))

from rpf7_key_scan import looks_like_root_directory, scan_candidates  # noqa: E402


class KeyScanTests(unittest.TestCase):
    def test_validates_bounded_root_directory(self) -> None:
        root = bytes.fromhex("7fffff00000000000000000100000003")
        self.assertTrue(
            looks_like_root_directory(
                root, entry_count=10, names_size=32, name_shift=0
            )
        )
        self.assertFalse(
            looks_like_root_directory(
                bytes.fromhex("ffffff00000000000000000100000003"),
                entry_count=10,
                names_size=32,
                name_shift=0,
            )
        )

    def test_scan_returns_only_candidate_that_decrypts_to_root(self) -> None:
        wanted = bytes(range(32))
        image = bytes(32) + wanted + bytes(32)
        root = bytes.fromhex("7fffff00000000000000000100000003")

        def synthetic_decrypt(block: bytes, key: bytes) -> bytes:
            del block
            return root if key == wanted else bytes(16)

        candidates = scan_candidates(
            image,
            bytes(16),
            entry_count=10,
            names_size=32,
            name_shift=0,
            start=0,
            size=len(image),
            stride=32,
            decrypt_block=synthetic_decrypt,
        )
        self.assertEqual(candidates, [(32, wanted)])

    def test_scan_rejects_out_of_bounds_range(self) -> None:
        with self.assertRaisesRegex(ValueError, "beyond"):
            scan_candidates(
                bytes(64),
                bytes(16),
                entry_count=1,
                names_size=1,
                name_shift=0,
                start=32,
                size=64,
                stride=1,
            )


if __name__ == "__main__":
    unittest.main()
