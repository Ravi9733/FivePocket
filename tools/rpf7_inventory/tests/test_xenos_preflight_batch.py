from __future__ import annotations

import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


TOOL_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOL_DIRECTORY))

import xenos_preflight_batch  # noqa: E402
from rpf7_extract import _write_exclusive  # noqa: E402


def outer_container(*, flags_byte: int = 0) -> bytes:
    virtual_size = 68
    physical_size = 16
    header = struct.pack(
        ">9I",
        0x102A1100 | flags_byte,
        virtual_size,
        physical_size,
        0,
        36,
        0,
        36,
        0,
        0,
    )
    return header + bytes(virtual_size + physical_size - len(header))


def fxc(*containers: bytes) -> bytes:
    return b"rgxe" + struct.pack("<I", 7) + b"".join(containers)


FAKE_VALIDATOR = """#!/usr/bin/env python3
import struct
import sys

submitted = valid = invalid = 0
while True:
    header = sys.stdin.buffer.read(4)
    if len(header) != 4:
        raise SystemExit(2)
    size = struct.unpack(\">I\", header)[0]
    if size == 0:
        if sys.stdin.buffer.read(1):
            raise SystemExit(2)
        break
    payload = sys.stdin.buffer.read(size)
    if len(payload) != size:
        raise SystemExit(2)
    submitted += 1
    if len(payload) >= 36:
        valid += 1
    else:
        invalid += 1
print(f\"XENOS_PREFLIGHT_V1 {submitted} {valid} {invalid}\")
raise SystemExit(0 if invalid == 0 else 1)
"""


class PrivateRepository:
    def __init__(self, root: Path):
        self.root = root
        self.private = root / "local-analysis"
        self.private.mkdir()
        (root / ".gitignore").write_text("/local-analysis/\n")
        subprocess.run(
            ["git", "init", "--quiet"],
            cwd=root,
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

    def validator(self) -> Path:
        path = self.root / "validator"
        path.write_text(FAKE_VALIDATOR)
        path.chmod(0o700)
        return path


class XenosPreflightBatchTests(unittest.TestCase):
    def test_stages_every_fxc_container_and_writes_only_redacted_aggregate(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            repository = PrivateRepository(Path(temporary))
            source = repository.private / "do-not-disclose.fxc"
            source.write_bytes(fxc(outer_container(), outer_container(flags_byte=1)))
            source.chmod(0o600)
            report = xenos_preflight_batch.validate_private_batch(
                [source],
                validator=repository.validator(),
                repository=repository.root,
            )
            self.assertEqual(report["status"], "passed")
            self.assertEqual(report["submitted_container_count"], 2)
            self.assertEqual(report["valid_container_count"], 2)
            self.assertFalse(report["recompiler_invoked"])
            self.assertFalse(report["container_offsets_emitted"])

            output = repository.private / "report.json"
            _write_exclusive(output, (json.dumps(report) + "\n").encode())
            encoded = output.read_text()
            self.assertNotIn(source.name, encoded)
            self.assertNotIn("offset", encoded.replace("container_offsets_emitted", ""))
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)
            self.assertEqual(source.stat().st_mode & 0o777, 0o600)

    def test_raw_invalid_container_is_aggregated_without_payload(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            repository = PrivateRepository(Path(temporary))
            source = repository.private / "private.bin"
            source.write_bytes(b"short")
            report = xenos_preflight_batch.validate_private_batch(
                [source],
                validator=repository.validator(),
                repository=repository.root,
            )
            self.assertEqual(report["status"], "failed")
            self.assertEqual(report["invalid_container_count"], 1)
            self.assertFalse(report["payload_emitted"])

    def test_rejects_symlink_duplicate_and_aggregate_overflow(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            repository = PrivateRepository(Path(temporary))
            source = repository.private / "private.bin"
            source.write_bytes(outer_container())
            link = repository.private / "linked.bin"
            link.symlink_to(source)
            validator = repository.validator()
            with self.assertRaisesRegex(
                xenos_preflight_batch.XenosPreflightError, "symbolic"
            ):
                xenos_preflight_batch.validate_private_batch(
                    [link], validator=validator, repository=repository.root
                )
            with self.assertRaisesRegex(
                xenos_preflight_batch.XenosPreflightError, "duplicate"
            ):
                xenos_preflight_batch.validate_private_batch(
                    [source, source], validator=validator, repository=repository.root
                )
            with self.assertRaisesRegex(
                xenos_preflight_batch.XenosPreflightError, "aggregate"
            ):
                xenos_preflight_batch.validate_private_batch(
                    [source],
                    validator=validator,
                    repository=repository.root,
                    limits=xenos_preflight_batch.BatchLimits(max_aggregate_bytes=1),
                )

    def test_rejects_tracked_and_nonignored_inputs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            repository = PrivateRepository(Path(temporary))
            tracked = repository.private / "tracked.bin"
            tracked.write_bytes(outer_container())
            subprocess.run(
                ["git", "add", "-f", "--", "local-analysis/tracked.bin"],
                cwd=repository.root,
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            with self.assertRaisesRegex(
                xenos_preflight_batch.XenosPreflightError, "tracked"
            ):
                xenos_preflight_batch.validate_private_batch(
                    [tracked],
                    validator=repository.validator(),
                    repository=repository.root,
                )

            outside = repository.root / "outside.bin"
            outside.write_bytes(outer_container())
            with self.assertRaisesRegex(
                xenos_preflight_batch.XenosPreflightError, "outside"
            ):
                xenos_preflight_batch.validate_private_batch(
                    [outside],
                    validator=repository.validator(),
                    repository=repository.root,
                )

    def test_enforces_file_and_container_count_caps(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            repository = PrivateRepository(Path(temporary))
            first = repository.private / "first.bin"
            second = repository.private / "second.bin"
            first.write_bytes(outer_container())
            second.write_bytes(outer_container())
            validator = repository.validator()
            with self.assertRaisesRegex(
                xenos_preflight_batch.XenosPreflightError, "file limit"
            ):
                xenos_preflight_batch.validate_private_batch(
                    [first, second],
                    validator=validator,
                    repository=repository.root,
                    limits=xenos_preflight_batch.BatchLimits(max_files=1),
                )
            with self.assertRaisesRegex(
                xenos_preflight_batch.XenosPreflightError, "file-size"
            ):
                xenos_preflight_batch.validate_private_batch(
                    [first],
                    validator=validator,
                    repository=repository.root,
                    limits=xenos_preflight_batch.BatchLimits(max_file_bytes=1),
                )

            staged = repository.private / "staged.fxc"
            staged.write_bytes(fxc(outer_container(), outer_container(flags_byte=1)))
            with self.assertRaisesRegex(
                xenos_preflight_batch.XenosPreflightError, "failed before"
            ):
                xenos_preflight_batch.validate_private_batch(
                    [staged],
                    validator=validator,
                    repository=repository.root,
                    limits=xenos_preflight_batch.BatchLimits(max_containers=1),
                )

    def test_rejects_invalid_validator_protocol(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            repository = PrivateRepository(Path(temporary))
            source = repository.private / "private.bin"
            source.write_bytes(outer_container())
            validator = repository.root / "validator"
            validator.write_text("#!/bin/sh\nprintf 'not-the-protocol\\n'\n")
            validator.chmod(stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)
            with self.assertRaisesRegex(
                xenos_preflight_batch.XenosPreflightError, "aggregate response"
            ):
                xenos_preflight_batch.validate_private_batch(
                    [source], validator=validator, repository=repository.root
                )


class RepositoryQuestionTests(unittest.TestCase):
    """Git is asked about a private path only where a repository holds the root."""

    def test_a_root_no_repository_holds_is_not_asked_about(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            if any((folder / ".git").exists() for folder in (root, *root.parents)):     # (asked here, not of the tool)
                self.skipTest("the temporary folder lies inside a repository")
            self.assertFalse(xenos_preflight_batch._in_repository(root))
            private = root / "local-analysis"
            private.mkdir()
            source = private / "input.bin"
            source.write_bytes(outer_container())
            limits = xenos_preflight_batch.BatchLimits()
            with mock.patch.object(xenos_preflight_batch, "_git_says",
                                   side_effect=AssertionError("Git is not asked outside a repository")):
                entry = xenos_preflight_batch._require_private_input(source, root, limits)
                self.assertEqual((entry.path, entry.size), (source, len(outer_container())))
                self.assertEqual(xenos_preflight_batch._require_private_report(private / "report", root), private / "report")
                # the rules on the path itself stay
                outside = root / "outside.bin"
                outside.write_bytes(outer_container())
                with self.assertRaisesRegex(xenos_preflight_batch.XenosPreflightError, "outside"):
                    xenos_preflight_batch._require_private_input(outside, root, limits)
                link = private / "link.bin"
                link.symlink_to(source)
                with self.assertRaisesRegex(xenos_preflight_batch.XenosPreflightError, "symbolic links"):
                    xenos_preflight_batch._require_private_input(link, root, limits)
                with self.assertRaisesRegex(xenos_preflight_batch.XenosPreflightError, "already exists"):
                    xenos_preflight_batch._require_private_report(source, root)

    def test_a_repository_above_the_root_is_still_asked(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            repository = PrivateRepository(Path(temporary).resolve())      # .git, and /local-analysis/ ignored at the top
            root = repository.root / "nested"
            (root / "local-analysis").mkdir(parents=True)
            source = root / "local-analysis" / "input.bin"
            source.write_bytes(outer_container())
            self.assertTrue(xenos_preflight_batch._in_repository(root))
            self.assertTrue(xenos_preflight_batch._in_repository(repository.root))
            # the top's rule does not cover nested/local-analysis: Git is asked, and says so
            with self.assertRaisesRegex(xenos_preflight_batch.XenosPreflightError, "input is not covered by Git ignore rules"):
                xenos_preflight_batch._require_private_input(source, root, xenos_preflight_batch.BatchLimits())
            with self.assertRaisesRegex(xenos_preflight_batch.XenosPreflightError, "report is not covered by Git ignore rules"):
                xenos_preflight_batch._require_private_report(root / "local-analysis" / "report", root)


if __name__ == "__main__":
    unittest.main()
