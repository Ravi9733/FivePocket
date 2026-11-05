#!/usr/bin/env python3
"""Stage bounded metadata for Xenos shader containers inside one GTA V FXC."""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
from typing import Sequence

from rpf7_extract import _write_exclusive


FXC_MAGIC = b"rgxe"
XENOS_HEADER_SIZE = 36
XENOS_FLAGS_MASK = 0xFFFFFF00
XENOS_FLAGS_MAGIC = 0x102A1100
CONSTANT_TABLE_MIN_SIZE = 32
SHADER_MIN_SIZE = 24
DEFINITION_TABLE_MIN_SIZE = 24


class FxcInspectError(ValueError):
    """Raised when FXC metadata cannot be staged safely."""


@dataclass(frozen=True)
class FxcLimits:
    max_file_size: int = 64 * 1024 * 1024
    max_container_size: int = 16 * 1024 * 1024
    max_containers: int = 100_000


@dataclass(frozen=True)
class XenosContainer:
    offset: int
    size: int
    virtual_size: int
    physical_size: int
    stage: str
    flags_byte: int
    constant_table_offset: int
    definition_table_offset: int
    shader_offset: int


@dataclass(frozen=True)
class FxcInspection:
    file_size: int
    vertex_type: int
    containers: tuple[XenosContainer, ...]
    rejected_magic_candidates: int


def _bounded_region(offset: int, minimum_size: int, virtual_size: int) -> bool:
    return offset != 0 and offset <= virtual_size and minimum_size <= virtual_size - offset


def _parse_xenos_candidate(
    data: bytes,
    offset: int,
    limits: FxcLimits,
) -> XenosContainer | None:
    if offset < 0 or XENOS_HEADER_SIZE > len(data) - offset:
        return None
    (
        flags,
        virtual_size,
        physical_size,
        _field_c,
        constant_table_offset,
        definition_table_offset,
        shader_offset,
        field_1c,
        field_20,
    ) = struct.unpack_from(">9I", data, offset)
    if (flags & XENOS_FLAGS_MASK) != XENOS_FLAGS_MAGIC:
        return None
    if field_1c != 0 or field_20 != 0:
        return None
    size = virtual_size + physical_size
    if size < XENOS_HEADER_SIZE or size > limits.max_container_size:
        return None
    if size > len(data) - offset or virtual_size > size:
        return None
    if not _bounded_region(
        constant_table_offset, CONSTANT_TABLE_MIN_SIZE, virtual_size
    ):
        return None
    if not _bounded_region(shader_offset, SHADER_MIN_SIZE, virtual_size):
        return None
    if definition_table_offset != 0 and not _bounded_region(
        definition_table_offset, DEFINITION_TABLE_MIN_SIZE, virtual_size
    ):
        return None
    return XenosContainer(
        offset=offset,
        size=size,
        virtual_size=virtual_size,
        physical_size=physical_size,
        stage="vertex" if flags & 0x1 else "pixel",
        flags_byte=flags & 0xFF,
        constant_table_offset=constant_table_offset,
        definition_table_offset=definition_table_offset,
        shader_offset=shader_offset,
    )


def inspect_fxc_bytes(
    data: bytes,
    *,
    limits: FxcLimits = FxcLimits(),
) -> FxcInspection:
    """Find fully bounded containers, including byte-packed FXC records.

    FXC names/records need not preserve DWORD alignment. Search the fixed magic
    prefix rather than stepping four bytes and silently missing real shaders.
    Accepted containers still advance by their validated extent, not their body.
    """

    if min(limits.max_file_size, limits.max_container_size, limits.max_containers) < 1:
        raise FxcInspectError("FXC limits must be positive")
    if len(data) < 8:
        raise FxcInspectError("FXC is shorter than its fixed header")
    if len(data) > limits.max_file_size:
        raise FxcInspectError("FXC exceeds the configured file-size limit")
    if data[:4] != FXC_MAGIC:
        raise FxcInspectError("FXC does not have little-endian rgxe magic")

    vertex_type = int.from_bytes(data[4:8], "little")
    containers: list[XenosContainer] = []
    rejected_magic_candidates = 0
    offset = 0
    while XENOS_HEADER_SIZE <= len(data) - offset:
        offset = data.find(b"\x10\x2a\x11", offset)
        if offset < 0 or XENOS_HEADER_SIZE > len(data) - offset:
            break
        flags = int.from_bytes(data[offset : offset + 4], "big")
        candidate = _parse_xenos_candidate(data, offset, limits)
        if candidate is None:
            if (flags & XENOS_FLAGS_MASK) == XENOS_FLAGS_MAGIC:
                rejected_magic_candidates += 1
            offset += 1
            continue
        if len(containers) >= limits.max_containers:
            raise FxcInspectError("Xenos container count exceeds the configured limit")
        containers.append(candidate)
        offset += candidate.size

    return FxcInspection(
        file_size=len(data),
        vertex_type=vertex_type,
        containers=tuple(containers),
        rejected_magic_candidates=rejected_magic_candidates,
    )


def _read_private_fxc(path: Path, limits: FxcLimits) -> bytes:
    before = os.lstat(path)
    if stat.S_ISLNK(before.st_mode):
        raise FxcInspectError("refusing to follow an FXC symbolic link")
    if not stat.S_ISREG(before.st_mode):
        raise FxcInspectError("FXC path is not a regular file")
    if before.st_size > limits.max_file_size:
        raise FxcInspectError("FXC exceeds the configured file-size limit")
    descriptor = os.open(
        path,
        os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0),
    )
    try:
        opened = os.fstat(descriptor)
        if (before.st_dev, before.st_ino) != (opened.st_dev, opened.st_ino):
            raise FxcInspectError("FXC changed while it was being opened")
        chunks: list[bytes] = []
        remaining = opened.st_size
        while remaining:
            chunk = os.read(descriptor, min(remaining, 1024 * 1024))
            if not chunk:
                raise FxcInspectError("FXC ended during its bounded read")
            chunks.append(chunk)
            remaining -= len(chunk)
        final = os.fstat(descriptor)
    finally:
        os.close(descriptor)
    if (final.st_dev, final.st_ino, final.st_size) != (
        opened.st_dev,
        opened.st_ino,
        opened.st_size,
    ):
        raise FxcInspectError("FXC changed while it was being read")
    return b"".join(chunks)


def manifest(inspection: FxcInspection) -> dict[str, object]:
    stage_counts = {"vertex": 0, "pixel": 0}
    virtual_bytes = 0
    physical_bytes = 0
    for container in inspection.containers:
        stage_counts[container.stage] += 1
        virtual_bytes += container.virtual_size
        physical_bytes += container.physical_size
    return {
        "schema_version": 1,
        "status": "staged",
        "file_size": inspection.file_size,
        "vertex_type": inspection.vertex_type,
        "container_count": len(inspection.containers),
        "stage_counts": stage_counts,
        "virtual_bytes": virtual_bytes,
        "physical_bytes": physical_bytes,
        "rejected_magic_candidates": inspection.rejected_magic_candidates,
        "containers": [asdict(container) for container in inspection.containers],
        "source_filename_emitted": False,
        "shader_names_read": False,
        "shader_names_emitted": False,
        "shader_bytecode_emitted": False,
        "content_hashes_emitted": False,
        "key_material_emitted": False,
    }


def aggregate_summary(inspection: FxcInspection) -> dict[str, object]:
    report = manifest(inspection)
    report.pop("containers")
    return report


def _require_private_manifest(path: Path, repository: Path) -> Path:
    requested = path.expanduser().absolute()
    try:
        os.lstat(requested)
    except FileNotFoundError:
        pass
    else:
        raise FxcInspectError("manifest output already exists")
    output = requested.resolve()
    private_root = (repository / "local-analysis").resolve()
    try:
        output.relative_to(private_root)
    except ValueError as error:
        raise FxcInspectError(f"manifest must remain below {private_root}") from error
    ignored = subprocess.run(
        ["git", "check-ignore", "--quiet", "--no-index", str(output)],
        cwd=repository,
        check=False,
    )
    if ignored.returncode != 0:
        raise FxcInspectError("manifest path is not covered by Git ignore rules")
    return output


def repository_root() -> Path:
    return Path(__file__).resolve().parents[2]


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fxc", type=Path)
    parser.add_argument("private_manifest", type=Path)
    parser.add_argument("--max-file-bytes", type=int, default=FxcLimits.max_file_size)
    parser.add_argument(
        "--max-container-bytes", type=int, default=FxcLimits.max_container_size
    )
    parser.add_argument("--max-containers", type=int, default=FxcLimits.max_containers)
    args = parser.parse_args(argv)
    try:
        limits = FxcLimits(
            max_file_size=args.max_file_bytes,
            max_container_size=args.max_container_bytes,
            max_containers=args.max_containers,
        )
        data = _read_private_fxc(args.fxc.expanduser().absolute(), limits)
        inspection = inspect_fxc_bytes(data, limits=limits)
        output = _require_private_manifest(args.private_manifest, repository_root())
        encoded = (json.dumps(manifest(inspection), indent=2) + "\n").encode("utf-8")
        _write_exclusive(output, encoded)
        print(json.dumps(aggregate_summary(inspection), indent=2))
        return 0
    except (OSError, FxcInspectError) as error:
        print(f"fxc_xenos_inspect: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
