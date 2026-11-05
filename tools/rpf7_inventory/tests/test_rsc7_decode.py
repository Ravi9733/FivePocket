from __future__ import annotations

from pathlib import Path
import struct
import sys
import tempfile
import unittest


TOOL_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOL_DIRECTORY))

import rsc7_decode  # noqa: E402
import rsc7_inspect  # noqa: E402


SYSTEM_FLAGS = 1 << 27
GRAPHICS_FLAGS = (1 << 28) | (1 << 27)


def resource(payload: bytes = b"xmem-framed") -> bytes:
    return struct.pack("<4sIII", b"RSC7", 1, SYSTEM_FLAGS, GRAPHICS_FLAGS) + payload


def header(payload: bytes = b"xmem-framed") -> rsc7_inspect.Rsc7Header:
    data = resource(payload)
    return rsc7_inspect.parse_rsc7_header(data[:16], file_size=len(data))


class Rsc7DecodeTests(unittest.TestCase):
    def test_decodes_once_then_splits_at_system_boundary(self) -> None:
        payload = b"synthetic-framing"
        calls: list[tuple[bytes, int, int, int]] = []

        def decoder(data: bytes, expected: int, maximum: int, frames: int) -> bytes:
            calls.append((data, expected, maximum, frames))
            return b"S" * 512 + b"G" * 512

        segments = rsc7_decode.decode_and_split(
            payload,
            header(payload),
            decoder,
            max_input_size=1024,
            max_frames=4,
        )
        self.assertEqual(segments.system, b"S" * 512)
        self.assertEqual(segments.graphics, b"G" * 512)
        self.assertEqual(calls, [(payload, 1024, 1024, 4)])

    def test_rejects_input_and_output_size_mismatches(self) -> None:
        payload = b"synthetic-framing"
        parsed = header(payload)
        with self.assertRaisesRegex(rsc7_decode.Rsc7DecodeError, "payload length"):
            rsc7_decode.decode_and_split(payload[:-1], parsed, lambda *args: b"")
        with self.assertRaisesRegex(rsc7_decode.Rsc7DecodeError, "decoder output"):
            rsc7_decode.decode_and_split(payload, parsed, lambda *args: b"short")
        with self.assertRaisesRegex(rsc7_decode.Rsc7DecodeError, "input limit"):
            rsc7_decode.decode_and_split(
                payload,
                parsed,
                lambda *args: bytes(1024),
                max_input_size=1,
            )

    def test_reads_stable_resource_and_preserves_source_mode(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "private.rsc7"
            path.write_bytes(resource())
            path.chmod(0o600)
            parsed, payload = rsc7_decode._read_resource(
                path,
                limits=rsc7_inspect.Rsc7Limits(),
                max_input_size=1024,
            )
            self.assertEqual(payload, b"xmem-framed")
            self.assertEqual(parsed.decoded_size, 1024)
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)

    def test_writes_new_private_segment_files_mode_0600(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "segments"
            segments = rsc7_decode.DecodedSegments(b"system", b"graphics")
            rsc7_decode._write_segments(output, segments)
            self.assertEqual(output.stat().st_mode & 0o777, 0o700)
            self.assertEqual((output / "system.bin").stat().st_mode & 0o777, 0o600)
            self.assertEqual((output / "graphics.bin").stat().st_mode & 0o777, 0o600)
            with self.assertRaises(FileExistsError):
                rsc7_decode._write_segments(output, segments)


if __name__ == "__main__":
    unittest.main()
