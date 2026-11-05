#!/usr/bin/env python3
"""Inspect a standalone RSC7 header without reading or emitting asset payloads."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import stat
import struct
import sys
from typing import BinaryIO, Sequence


RSC7_MAGIC = b"RSC7"
HEADER_SIZE = 16


class Rsc7Error(ValueError):
    """Raised when a resource header cannot be accepted safely."""


@dataclass(frozen=True)
class Rsc7Limits:
    """Hard bounds for file metadata and decoded resource declarations."""

    max_file_size: int = 2 * 1024 * 1024 * 1024
    max_segment_size: int = 1024 * 1024 * 1024
    max_decoded_size: int = 2 * 1024 * 1024 * 1024
    max_chunks_per_segment: int = 256


@dataclass(frozen=True)
class Rsc7Header:
    version: int
    system_flags: int
    graphics_flags: int
    system_size: int
    graphics_size: int
    system_chunks: int
    graphics_chunks: int
    file_size: int

    @property
    def decoded_size(self) -> int:
        return self.system_size + self.graphics_size

    @property
    def payload_size(self) -> int:
        return self.file_size - HEADER_SIZE


def resource_version(system_flags: int, graphics_flags: int) -> int:
    """Recover the RPF7 resource version packed across both flag high nibbles."""

    return ((system_flags >> 28) << 4) | (graphics_flags >> 28)


def resource_chunk_count(flags: int) -> int:
    """Count page chunks described by one RSC7 segment flag word."""

    return (
        ((flags >> 4) & 0x1)
        + ((flags >> 5) & 0x3)
        + ((flags >> 7) & 0xF)
        + ((flags >> 11) & 0x3F)
        + ((flags >> 17) & 0x7F)
        + ((flags >> 24) & 0x1)
        + ((flags >> 25) & 0x1)
        + ((flags >> 26) & 0x1)
        + ((flags >> 27) & 0x1)
    )


def resource_size(flags: int) -> int:
    """Decode a GTA V RSC7 virtual/physical page-size flag word."""

    base_size = 0x200 << (flags & 0xF)
    weighted_pages = (
        (((flags >> 27) & 0x1) << 0)
        + (((flags >> 26) & 0x1) << 1)
        + (((flags >> 25) & 0x1) << 2)
        + (((flags >> 24) & 0x1) << 3)
        + (((flags >> 17) & 0x7F) << 4)
        + (((flags >> 11) & 0x3F) << 5)
        + (((flags >> 7) & 0xF) << 6)
        + (((flags >> 5) & 0x3) << 7)
        + (((flags >> 4) & 0x1) << 8)
    )
    return base_size * weighted_pages


def parse_rsc7_header(
    header: bytes,
    *,
    file_size: int,
    limits: Rsc7Limits = Rsc7Limits(),
) -> Rsc7Header:
    """Validate a little-endian standalone RSC7 header and its declared sizes."""

    if len(header) != HEADER_SIZE:
        raise Rsc7Error("resource does not contain a complete 16-byte header")
    if file_size <= HEADER_SIZE:
        raise Rsc7Error("resource contains no payload after its header")
    if file_size > limits.max_file_size:
        raise Rsc7Error("resource file exceeds the configured size limit")

    magic, version, system_flags, graphics_flags = struct.unpack("<4sIII", header)
    if magic != RSC7_MAGIC:
        raise Rsc7Error("resource does not have standalone RSC7 magic")
    if version > 0xFF:
        raise Rsc7Error("resource version does not fit the RPF7 eight-bit field")
    expected_version = resource_version(system_flags, graphics_flags)
    if version != expected_version:
        raise Rsc7Error("resource version disagrees with its segment flag words")

    system_chunks = resource_chunk_count(system_flags)
    graphics_chunks = resource_chunk_count(graphics_flags)
    if system_chunks > limits.max_chunks_per_segment:
        raise Rsc7Error("system segment exceeds the configured chunk limit")
    if graphics_chunks > limits.max_chunks_per_segment:
        raise Rsc7Error("graphics segment exceeds the configured chunk limit")
    system_size = resource_size(system_flags)
    graphics_size = resource_size(graphics_flags)
    if system_size == 0 and graphics_size == 0:
        raise Rsc7Error("resource declares no virtual or physical pages")
    if system_size > limits.max_segment_size:
        raise Rsc7Error("system segment exceeds the configured decoded-size limit")
    if graphics_size > limits.max_segment_size:
        raise Rsc7Error("graphics segment exceeds the configured decoded-size limit")
    if system_size + graphics_size > limits.max_decoded_size:
        raise Rsc7Error("resource exceeds the configured total decoded-size limit")

    return Rsc7Header(
        version=version,
        system_flags=system_flags,
        graphics_flags=graphics_flags,
        system_size=system_size,
        graphics_size=graphics_size,
        system_chunks=system_chunks,
        graphics_chunks=graphics_chunks,
        file_size=file_size,
    )


def _open_regular_file_without_following(path: Path) -> tuple[BinaryIO, os.stat_result]:
    before = os.lstat(path)
    if stat.S_ISLNK(before.st_mode):
        raise Rsc7Error("refusing to follow a resource symbolic link")
    if not stat.S_ISREG(before.st_mode):
        raise Rsc7Error("resource path is not a regular file")
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags)
    try:
        opened = os.fstat(descriptor)
        if not stat.S_ISREG(opened.st_mode):
            raise Rsc7Error("opened resource object is not a regular file")
        if (before.st_dev, before.st_ino) != (opened.st_dev, opened.st_ino):
            raise Rsc7Error("resource changed while it was being opened")
        return os.fdopen(descriptor, "rb"), opened
    except BaseException:
        os.close(descriptor)
        raise


def inspect_rsc7_file(
    path: str | os.PathLike[str],
    *,
    limits: Rsc7Limits = Rsc7Limits(),
) -> Rsc7Header:
    """Read only the RSC7 header and stable file metadata."""

    stream, opened = _open_regular_file_without_following(Path(path).absolute())
    with stream:
        header = stream.read(HEADER_SIZE)
        final = os.fstat(stream.fileno())
    if final.st_size != opened.st_size:
        raise Rsc7Error("resource size changed while its header was being read")
    return parse_rsc7_header(header, file_size=opened.st_size, limits=limits)


def summary(header: Rsc7Header) -> dict[str, object]:
    """Return metadata only; no source name, payload bytes, or key material."""

    return {
        "status": "inspected",
        "version": header.version,
        "system_flags": f"0x{header.system_flags:08X}",
        "graphics_flags": f"0x{header.graphics_flags:08X}",
        "system_size": header.system_size,
        "graphics_size": header.graphics_size,
        "decoded_size": header.decoded_size,
        "system_chunks": header.system_chunks,
        "graphics_chunks": header.graphics_chunks,
        "compressed_payload_size": header.payload_size,
        "filename_emitted": False,
        "payload_read": False,
        "payload_emitted": False,
        "key_material_emitted": False,
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("resource", type=Path)
    parser.add_argument("--max-file-bytes", type=int, default=Rsc7Limits.max_file_size)
    parser.add_argument(
        "--max-segment-bytes", type=int, default=Rsc7Limits.max_segment_size
    )
    parser.add_argument(
        "--max-decoded-bytes", type=int, default=Rsc7Limits.max_decoded_size
    )
    args = parser.parse_args(argv)
    try:
        if min(args.max_file_bytes, args.max_segment_bytes, args.max_decoded_bytes) < 1:
            raise Rsc7Error("size limits must be positive")
        limits = Rsc7Limits(
            max_file_size=args.max_file_bytes,
            max_segment_size=args.max_segment_bytes,
            max_decoded_size=args.max_decoded_bytes,
        )
        print(json.dumps(summary(inspect_rsc7_file(args.resource, limits=limits)), indent=2))
        return 0
    except (OSError, Rsc7Error) as error:
        print(f"rsc7_inspect: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
