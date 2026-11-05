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

import rsc7_inspect  # noqa: E402


def resource(
    *,
    version: int = 1,
    system_flags: int = 1 << 17,
    graphics_flags: int = (1 << 28) | (1 << 27),
    payload: bytes = b"compressed",
) -> bytes:
    return struct.pack(
        "<4sIII", b"RSC7", version, system_flags, graphics_flags
    ) + payload


class Rsc7InspectorTests(unittest.TestCase):
    def test_parses_sizes_chunks_and_redacted_summary(self) -> None:
        parsed = rsc7_inspect.parse_rsc7_header(
            resource()[:16], file_size=len(resource())
        )
        self.assertEqual(parsed.version, 1)
        self.assertEqual(parsed.system_size, 16 * 0x200)
        self.assertEqual(parsed.graphics_size, 0x200)
        self.assertEqual(parsed.system_chunks, 1)
        self.assertEqual(parsed.graphics_chunks, 1)
        report = rsc7_inspect.summary(parsed)
        self.assertFalse(report["payload_read"])
        self.assertFalse(report["filename_emitted"])
        self.assertNotIn("path", report)

    def test_rejects_bad_magic_truncation_and_empty_payload(self) -> None:
        with self.assertRaisesRegex(rsc7_inspect.Rsc7Error, "magic"):
            rsc7_inspect.parse_rsc7_header(
                b"BAD!" + resource()[4:16], file_size=len(resource())
            )
        with self.assertRaisesRegex(rsc7_inspect.Rsc7Error, "complete"):
            rsc7_inspect.parse_rsc7_header(b"RSC7", file_size=17)
        with self.assertRaisesRegex(rsc7_inspect.Rsc7Error, "no payload"):
            rsc7_inspect.parse_rsc7_header(resource()[:16], file_size=16)

    def test_rejects_inconsistent_version_and_unbounded_segments(self) -> None:
        with self.assertRaisesRegex(rsc7_inspect.Rsc7Error, "disagrees"):
            data = resource(version=2)
            rsc7_inspect.parse_rsc7_header(data[:16], file_size=len(data))
        with self.assertRaisesRegex(rsc7_inspect.Rsc7Error, "system segment"):
            data = resource(system_flags=(1 << 17) | 4)
            rsc7_inspect.parse_rsc7_header(
                data[:16],
                file_size=len(data),
                limits=rsc7_inspect.Rsc7Limits(max_segment_size=64 * 1024),
            )

    def test_rejects_zero_page_declaration(self) -> None:
        data = resource(version=0, system_flags=0, graphics_flags=0)
        with self.assertRaisesRegex(rsc7_inspect.Rsc7Error, "no virtual"):
            rsc7_inspect.parse_rsc7_header(data[:16], file_size=len(data))

    def test_file_inspection_is_header_only_and_preserves_mode(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "private.rsc7"
            path.write_bytes(resource(payload=b"private payload that is never read"))
            path.chmod(0o600)
            parsed = rsc7_inspect.inspect_rsc7_file(path)
            self.assertEqual(parsed.payload_size, path.stat().st_size - 16)
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)

    @unittest.skipUnless(hasattr(os, "symlink"), "symbolic links unavailable")
    def test_refuses_symbolic_links(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / "target.rsc7"
            link = Path(temporary) / "link.rsc7"
            target.write_bytes(resource())
            link.symlink_to(target)
            with self.assertRaisesRegex(rsc7_inspect.Rsc7Error, "symbolic link"):
                rsc7_inspect.inspect_rsc7_file(link)

    def test_cli_emits_metadata_without_input_name(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "secret-resource-name.ytd"
            path.write_bytes(resource())
            completed = subprocess.run(
                [sys.executable, str(TOOL_DIRECTORY / "rsc7_inspect.py"), str(path)],
                check=False,
                capture_output=True,
                text=True,
            )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        report = json.loads(completed.stdout)
        self.assertFalse(report["filename_emitted"])
        self.assertNotIn(path.name, completed.stdout)


if __name__ == "__main__":
    unittest.main()
