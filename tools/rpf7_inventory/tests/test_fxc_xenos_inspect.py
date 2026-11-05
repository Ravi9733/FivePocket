from __future__ import annotations

import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest


TOOL_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOL_DIRECTORY))

import fxc_xenos_inspect  # noqa: E402


def container(*, flags_byte: int, valid_offsets: bool = True) -> bytes:
    virtual_size = 68
    physical_size = 16
    offset = 36 if valid_offsets else 67
    header = struct.pack(
        ">9I",
        0x102A1100 | flags_byte,
        virtual_size,
        physical_size,
        0,
        offset,
        0,
        offset,
        0,
        0,
    )
    return header + bytes(virtual_size + physical_size - len(header))


def fxc(*containers: bytes) -> bytes:
    return b"rgxe" + struct.pack("<I", 7) + b"".join(containers)


class FxcXenosInspectorTests(unittest.TestCase):
    def test_stages_bounded_vertex_and_pixel_metadata_only(self) -> None:
        data = fxc(container(flags_byte=1), container(flags_byte=0))
        result = fxc_xenos_inspect.inspect_fxc_bytes(data)
        self.assertEqual(result.vertex_type, 7)
        self.assertEqual([entry.stage for entry in result.containers], ["vertex", "pixel"])
        self.assertEqual([entry.offset for entry in result.containers], [8, 92])
        report = fxc_xenos_inspect.manifest(result)
        self.assertEqual(report["container_count"], 2)
        self.assertEqual(report["stage_counts"], {"vertex": 1, "pixel": 1})
        self.assertFalse(report["shader_names_read"])
        self.assertFalse(report["shader_bytecode_emitted"])
        self.assertTrue(all("name" not in entry for entry in report["containers"]))
        self.assertTrue(all("hash" not in entry for entry in report["containers"]))

    def test_rejects_wrong_outer_magic_and_file_limit(self) -> None:
        with self.assertRaisesRegex(fxc_xenos_inspect.FxcInspectError, "rgxe"):
            fxc_xenos_inspect.inspect_fxc_bytes(b"rgxa" + bytes(40))
        with self.assertRaisesRegex(fxc_xenos_inspect.FxcInspectError, "file-size"):
            fxc_xenos_inspect.inspect_fxc_bytes(
                fxc(container(flags_byte=1)),
                limits=fxc_xenos_inspect.FxcLimits(max_file_size=8),
            )

    def test_invalid_internal_offsets_are_not_staged(self) -> None:
        result = fxc_xenos_inspect.inspect_fxc_bytes(
            fxc(container(flags_byte=1, valid_offsets=False))
        )
        self.assertEqual(result.containers, ())
        self.assertEqual(result.rejected_magic_candidates, 1)

    def test_byte_packed_records_at_each_alignment_are_not_skipped(self) -> None:
        for padding in (1, 2, 3):
            with self.subTest(padding=padding):
                first = container(flags_byte=1)
                invalid = container(flags_byte=0, valid_offsets=False)
                second = container(flags_byte=0)
                data = fxc(b"x" * padding, first, b"y", invalid, b"zz", second)
                result = fxc_xenos_inspect.inspect_fxc_bytes(data)
                self.assertEqual([entry.offset for entry in result.containers],
                    [8 + padding, 8 + padding + len(first) + 1 + len(invalid) + 2])
                self.assertEqual([entry.stage for entry in result.containers], ["vertex", "pixel"])
                self.assertEqual(result.rejected_magic_candidates, 1)
        # A trailing magic prefix is not a complete metadata record.
        self.assertEqual(fxc_xenos_inspect.inspect_fxc_bytes(fxc(b"\x10\x2a\x11")).containers, ())

    def test_container_count_limit_fails_closed(self) -> None:
        data = fxc(container(flags_byte=1), container(flags_byte=0))
        with self.assertRaisesRegex(fxc_xenos_inspect.FxcInspectError, "count"):
            fxc_xenos_inspect.inspect_fxc_bytes(
                data,
                limits=fxc_xenos_inspect.FxcLimits(max_containers=1),
            )

    def test_file_read_preserves_mode_and_manifest_write_is_0600(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "private.fxc"
            output = Path(temporary) / "manifest.json"
            source.write_bytes(fxc(container(flags_byte=1)))
            source.chmod(0o600)
            data = fxc_xenos_inspect._read_private_fxc(
                source, fxc_xenos_inspect.FxcLimits()
            )
            result = fxc_xenos_inspect.inspect_fxc_bytes(data)
            from rpf7_extract import _write_exclusive

            _write_exclusive(
                output,
                (json.dumps(fxc_xenos_inspect.manifest(result)) + "\n").encode(),
            )
            self.assertEqual(source.stat().st_mode & 0o777, 0o600)
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)


if __name__ == "__main__":
    unittest.main()
