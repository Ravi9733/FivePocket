#!/usr/bin/env python3
"""Run the standalone Xenos structural validator over private bounded inputs."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
from typing import BinaryIO, Sequence

from fxc_xenos_inspect import FXC_MAGIC, FxcInspectError, FxcLimits, inspect_fxc_bytes
from rpf7_extract import _write_exclusive


PROTOCOL_PREFIX = b"XENOS_PREFLIGHT_V1 "
MAX_PROTOCOL_OUTPUT = 128
HARD_MAX_CONTAINER_BYTES = 16 * 1024 * 1024
HARD_MAX_CONTAINERS = 100_000
HARD_MAX_AGGREGATE_BYTES = 1024 * 1024 * 1024


class XenosPreflightError(ValueError):
    """Raised when a private batch cannot be validated without disclosure."""


@dataclass(frozen=True)
class BatchLimits:
    max_files: int = 1024
    max_file_bytes: int = 64 * 1024 * 1024
    max_container_bytes: int = HARD_MAX_CONTAINER_BYTES
    max_containers: int = HARD_MAX_CONTAINERS
    max_aggregate_bytes: int = HARD_MAX_AGGREGATE_BYTES


@dataclass(frozen=True)
class PrivateInput:
    path: Path
    device: int
    inode: int
    size: int


def repository_root() -> Path:
    return Path(__file__).resolve().parents[2]


def default_validator(repository: Path) -> Path:
    return repository / "out/host-tools/xenos/XenosRecomp/XenosShaderPreflight"


def _validate_limits(limits: BatchLimits) -> None:
    values = (
        limits.max_files,
        limits.max_file_bytes,
        limits.max_container_bytes,
        limits.max_containers,
        limits.max_aggregate_bytes,
    )
    if min(values) < 1:
        raise XenosPreflightError("batch limits must be positive")
    if limits.max_container_bytes > HARD_MAX_CONTAINER_BYTES:
        raise XenosPreflightError("container limit exceeds validator protocol cap")
    if limits.max_containers > HARD_MAX_CONTAINERS:
        raise XenosPreflightError("container count exceeds validator protocol cap")
    if limits.max_aggregate_bytes > HARD_MAX_AGGREGATE_BYTES:
        raise XenosPreflightError("aggregate limit exceeds validator protocol cap")


def _git_says(repository: Path, arguments: list[str]) -> bool:
    result = subprocess.run(
        ["git", *arguments],
        cwd=repository,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.returncode == 0


def _in_repository(repository: Path) -> bool:
    """Whether a Git repository holds this root: the root or a folder above it has a .git entry.

    A player's unpacked builder has none. Nothing there can be tracked and no ignore rule exists to ask about, so Git
    is not asked (it need not be installed); the rules on the path itself stay (inside the root's local-analysis, no
    link on the way). Inside a repository both questions are asked as before, and a Git that cannot answer refuses."""
    root = repository.resolve()
    return any((parent / ".git").exists() for parent in (root, *root.parents))


def _private_lexical_path(path: Path, repository: Path) -> tuple[Path, Path]:
    repository_lexical = Path(os.path.abspath(os.fspath(repository.expanduser())))
    private_lexical = repository_lexical / "local-analysis"
    private_root = repository.resolve() / "local-analysis"
    requested_lexical = Path(os.path.abspath(os.fspath(path.expanduser())))
    try:
        relative = requested_lexical.relative_to(private_lexical)
    except ValueError:
        try:
            relative = requested_lexical.relative_to(private_root)
        except ValueError as error:
            raise XenosPreflightError("path is outside private local analysis") from error
    return private_root / relative, relative


def _reject_symlink_components(relative: Path, private_root: Path) -> None:
    components = (Path(), *reversed(relative.parents), relative)
    seen: set[Path] = set()
    for component in components:
        candidate = private_root / component
        if candidate in seen:
            continue
        seen.add(candidate)
        try:
            mode = os.lstat(candidate).st_mode
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(mode):
            raise XenosPreflightError("symbolic links are not accepted")


def _require_private_input(
    path: Path,
    repository: Path,
    limits: BatchLimits,
) -> PrivateInput:
    requested, relative = _private_lexical_path(path, repository)
    private_root = repository.resolve() / "local-analysis"
    _reject_symlink_components(relative, private_root)
    try:
        metadata = os.lstat(requested)
    except OSError as error:
        raise XenosPreflightError("private input is unavailable") from error
    if not stat.S_ISREG(metadata.st_mode):
        raise XenosPreflightError("private input is not a regular file")
    if metadata.st_size > limits.max_file_bytes:
        raise XenosPreflightError("private input exceeds the file-size limit")
    repository_relative = requested.relative_to(repository.resolve())
    if _in_repository(repository):
        if _git_says(repository, ["ls-files", "--error-unmatch", "--", str(repository_relative)]):
            raise XenosPreflightError("tracked inputs are not accepted")
        if not _git_says(
            repository,
            ["check-ignore", "--quiet", "--no-index", "--", str(repository_relative)],
        ):
            raise XenosPreflightError("private input is not covered by Git ignore rules")
    return PrivateInput(requested, metadata.st_dev, metadata.st_ino, metadata.st_size)


def _require_private_report(path: Path, repository: Path) -> Path:
    requested, relative = _private_lexical_path(path, repository)
    private_root = repository.resolve() / "local-analysis"
    _reject_symlink_components(relative, private_root)
    if not requested.parent.is_dir():
        raise XenosPreflightError("private report parent must already exist")
    try:
        os.lstat(requested)
    except FileNotFoundError:
        pass
    except OSError as error:
        raise XenosPreflightError("private report path is unavailable") from error
    else:
        raise XenosPreflightError("private report already exists")
    repository_relative = requested.relative_to(repository.resolve())
    if _in_repository(repository):
        if _git_says(repository, ["ls-files", "--error-unmatch", "--", str(repository_relative)]):
            raise XenosPreflightError("tracked reports are not accepted")
        if not _git_says(
            repository,
            ["check-ignore", "--quiet", "--no-index", "--", str(repository_relative)],
        ):
            raise XenosPreflightError("private report is not covered by Git ignore rules")
    return requested


def _require_validator(path: Path) -> Path:
    requested = Path(os.path.abspath(os.fspath(path.expanduser())))
    try:
        metadata = os.lstat(requested)
    except OSError as error:
        raise XenosPreflightError("validator executable is unavailable") from error
    if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
        raise XenosPreflightError("validator must be a regular non-symlink file")
    if not metadata.st_mode & stat.S_IXUSR:
        raise XenosPreflightError("validator is not owner-executable")
    return requested


def _prepare_inputs(
    paths: Sequence[Path],
    repository: Path,
    limits: BatchLimits,
) -> list[PrivateInput]:
    if not paths:
        raise XenosPreflightError("at least one private input is required")
    if len(paths) > limits.max_files:
        raise XenosPreflightError("private input count exceeds the file limit")
    prepared = [_require_private_input(path, repository, limits) for path in paths]
    identities = {(entry.device, entry.inode) for entry in prepared}
    if len(identities) != len(prepared):
        raise XenosPreflightError("duplicate private inputs are not accepted")
    aggregate = sum(entry.size for entry in prepared)
    if aggregate > limits.max_aggregate_bytes:
        raise XenosPreflightError("private inputs exceed the aggregate-size limit")
    return prepared


def _read_private_input(entry: PrivateInput) -> bytes:
    descriptor = os.open(
        entry.path,
        os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0),
    )
    try:
        opened = os.fstat(descriptor)
        if (opened.st_dev, opened.st_ino, opened.st_size) != (
            entry.device,
            entry.inode,
            entry.size,
        ):
            raise XenosPreflightError("private input changed before reading")
        chunks: list[bytes] = []
        remaining = entry.size
        while remaining:
            chunk = os.read(descriptor, min(remaining, 1024 * 1024))
            if not chunk:
                raise XenosPreflightError("private input ended during bounded read")
            chunks.append(chunk)
            remaining -= len(chunk)
        final = os.fstat(descriptor)
    finally:
        os.close(descriptor)
    if (final.st_dev, final.st_ino, final.st_size) != (
        entry.device,
        entry.inode,
        entry.size,
    ):
        raise XenosPreflightError("private input changed while being read")
    return b"".join(chunks)


def _write_all(stream: BinaryIO, data: bytes | memoryview) -> None:
    view = memoryview(data)
    while view:
        written = stream.write(view)
        if written is None or written <= 0:
            raise XenosPreflightError("validator protocol write failed")
        view = view[written:]


def _parse_protocol_summary(output: bytes) -> tuple[int, int, int]:
    if len(output) > MAX_PROTOCOL_OUTPUT or not output.endswith(b"\n"):
        raise XenosPreflightError("validator returned an invalid aggregate response")
    fields = output.removesuffix(b"\n").split(b" ")
    if len(fields) != 4 or fields[0] + b" " != PROTOCOL_PREFIX:
        raise XenosPreflightError("validator returned an invalid aggregate response")
    try:
        counts = tuple(int(field.decode("ascii")) for field in fields[1:])
    except (UnicodeDecodeError, ValueError) as error:
        raise XenosPreflightError("validator returned an invalid aggregate response") from error
    if any(count < 0 for count in counts) or counts[0] != counts[1] + counts[2]:
        raise XenosPreflightError("validator returned inconsistent aggregate counts")
    return counts


def validate_private_batch(
    paths: Sequence[Path],
    *,
    validator: Path,
    repository: Path,
    limits: BatchLimits = BatchLimits(),
) -> dict[str, object]:
    _validate_limits(limits)
    prepared = _prepare_inputs(paths, repository, limits)
    validator = _require_validator(validator)
    process = subprocess.Popen(
        [str(validator)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    if process.stdin is None or process.stdout is None:
        process.kill()
        process.wait()
        raise XenosPreflightError("validator pipes are unavailable")

    staged = 0
    rejected = 0
    empty_sources = 0
    try:
        for entry in prepared:
            data = _read_private_input(entry)
            if data.startswith(FXC_MAGIC):
                remaining = limits.max_containers - staged
                if remaining < 1:
                    raise XenosPreflightError("container count exceeds the batch limit")
                inspection = inspect_fxc_bytes(
                    data,
                    limits=FxcLimits(
                        max_file_size=limits.max_file_bytes,
                        max_container_size=limits.max_container_bytes,
                        max_containers=remaining,
                    ),
                )
                rejected += inspection.rejected_magic_candidates
                if not inspection.containers:
                    empty_sources += 1
                for container in inspection.containers:
                    _write_all(process.stdin, struct.pack(">I", container.size))
                    _write_all(
                        process.stdin,
                        memoryview(data)[container.offset : container.offset + container.size],
                    )
                    staged += 1
            else:
                if not data:
                    raise XenosPreflightError("raw container is empty")
                if len(data) > limits.max_container_bytes:
                    raise XenosPreflightError("raw container exceeds the container-size limit")
                if staged >= limits.max_containers:
                    raise XenosPreflightError("container count exceeds the batch limit")
                _write_all(process.stdin, struct.pack(">I", len(data)))
                _write_all(process.stdin, data)
                staged += 1
        _write_all(process.stdin, b"\0\0\0\0")
        process.stdin.close()
        output = process.stdout.read(MAX_PROTOCOL_OUTPUT + 1)
        process.stdout.close()
        result = process.wait()
    except (BrokenPipeError, FxcInspectError, OSError) as error:
        try:
            process.stdin.close()
        except (BrokenPipeError, OSError):
            pass
        process.kill()
        process.wait()
        process.stdout.close()
        raise XenosPreflightError("private batch failed before validation completed") from error
    except BaseException:
        try:
            process.stdin.close()
        except (BrokenPipeError, OSError):
            pass
        process.kill()
        process.wait()
        process.stdout.close()
        raise

    submitted, valid, invalid = _parse_protocol_summary(output)
    if submitted != staged or result not in (0, 1):
        raise XenosPreflightError("validator result does not match the submitted batch")
    if (result == 0) != (invalid == 0):
        raise XenosPreflightError("validator status is inconsistent with its aggregate counts")
    passed = staged > 0 and invalid == 0 and rejected == 0 and empty_sources == 0
    return {
        "schema_version": 1,
        "status": "passed" if passed else "failed",
        "input_file_count": len(prepared),
        "submitted_container_count": submitted,
        "valid_container_count": valid,
        "invalid_container_count": invalid,
        "rejected_magic_candidate_count": rejected,
        "empty_input_count": empty_sources,
        "validator_only": True,
        "recompiler_invoked": False,
        "source_filenames_emitted": False,
        "content_hashes_emitted": False,
        "shader_names_emitted": False,
        "shader_text_emitted": False,
        "shader_bytecode_emitted": False,
        "container_offsets_emitted": False,
        "payload_emitted": False,
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("private_report", type=Path)
    parser.add_argument("inputs", nargs="+", type=Path)
    parser.add_argument("--validator", type=Path)
    parser.add_argument("--max-files", type=int, default=BatchLimits.max_files)
    parser.add_argument("--max-file-bytes", type=int, default=BatchLimits.max_file_bytes)
    parser.add_argument(
        "--max-container-bytes", type=int, default=BatchLimits.max_container_bytes
    )
    parser.add_argument("--max-containers", type=int, default=BatchLimits.max_containers)
    parser.add_argument(
        "--max-aggregate-bytes", type=int, default=BatchLimits.max_aggregate_bytes
    )
    args = parser.parse_args(argv)
    repository = repository_root()
    try:
        report_path = _require_private_report(args.private_report, repository)
        report = validate_private_batch(
            args.inputs,
            validator=args.validator or default_validator(repository),
            repository=repository,
            limits=BatchLimits(
                max_files=args.max_files,
                max_file_bytes=args.max_file_bytes,
                max_container_bytes=args.max_container_bytes,
                max_containers=args.max_containers,
                max_aggregate_bytes=args.max_aggregate_bytes,
            ),
        )
        _write_exclusive(
            report_path,
            (json.dumps(report, indent=2, sort_keys=True) + "\n").encode("utf-8"),
        )
        print("xenos_preflight_batch: private redacted report written")
        return 0 if report["status"] == "passed" else 1
    except (OSError, XenosPreflightError):
        print("xenos_preflight_batch: batch failed safely", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
