#!/usr/bin/env python3
"""Decode and split one bounded Xbox 360 standalone RSC7 resource."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
from typing import Callable, Sequence

from rpf7_extract import Rpf7ExtractionError, _write_exclusive, decode_xmem_with_tool
from rsc7_inspect import (
    HEADER_SIZE,
    Rsc7Error,
    Rsc7Header,
    Rsc7Limits,
    parse_rsc7_header,
)


DEFAULT_MAX_INPUT = 64 * 1024 * 1024
DEFAULT_MAX_FRAMES = 8192


class Rsc7DecodeError(ValueError):
    """Raised when an RSC7 resource cannot be decoded within its bounds."""


Decoder = Callable[[bytes, int, int, int], bytes]


@dataclass(frozen=True)
class DecodedSegments:
    system: bytes
    graphics: bytes


def decode_and_split(
    payload: bytes,
    header: Rsc7Header,
    decoder: Decoder,
    *,
    max_input_size: int = DEFAULT_MAX_INPUT,
    max_frames: int = DEFAULT_MAX_FRAMES,
) -> DecodedSegments:
    """Decode an XMem/LZX payload and split its declared memory-image segments."""

    if max_input_size < 1 or max_frames < 1:
        raise Rsc7DecodeError("decode limits must be positive")
    if len(payload) != header.payload_size:
        raise Rsc7DecodeError("payload length changed after header validation")
    if len(payload) > max_input_size:
        raise Rsc7DecodeError("compressed payload exceeds the configured input limit")
    decoded = decoder(payload, header.decoded_size, header.decoded_size, max_frames)
    if len(decoded) != header.decoded_size:
        raise Rsc7DecodeError("decoder output does not match the RSC7 segment sizes")
    split = header.system_size
    if split > len(decoded):
        raise Rsc7DecodeError("system segment boundary exceeds decoded output")
    system = decoded[:split]
    graphics = decoded[split:]
    if len(system) != header.system_size or len(graphics) != header.graphics_size:
        raise Rsc7DecodeError("decoded segment split does not match the RSC7 header")
    return DecodedSegments(system=system, graphics=graphics)


def _read_resource(
    path: Path,
    *,
    limits: Rsc7Limits,
    max_input_size: int,
) -> tuple[Rsc7Header, bytes]:
    before = os.lstat(path)
    if stat.S_ISLNK(before.st_mode):
        raise Rsc7DecodeError("refusing to follow a resource symbolic link")
    if not stat.S_ISREG(before.st_mode):
        raise Rsc7DecodeError("resource path is not a regular file")
    if before.st_size > limits.max_file_size:
        raise Rsc7DecodeError("resource file exceeds the configured size limit")
    if before.st_size > HEADER_SIZE + max_input_size:
        raise Rsc7DecodeError("compressed payload exceeds the configured input limit")

    descriptor = os.open(
        path,
        os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0),
    )
    try:
        opened = os.fstat(descriptor)
        if (before.st_dev, before.st_ino) != (opened.st_dev, opened.st_ino):
            raise Rsc7DecodeError("resource changed while it was being opened")
        header_bytes = os.read(descriptor, HEADER_SIZE)
        header = parse_rsc7_header(header_bytes, file_size=opened.st_size, limits=limits)
        remaining = header.payload_size
        chunks: list[bytes] = []
        while remaining:
            chunk = os.read(descriptor, min(remaining, 1024 * 1024))
            if not chunk:
                raise Rsc7DecodeError("resource ended inside its compressed payload")
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
        raise Rsc7DecodeError("resource changed while it was being read")
    return header, b"".join(chunks)


def _require_private_output_directory(path: Path, repository: Path) -> Path:
    requested = path.expanduser().absolute()
    try:
        os.lstat(requested)
    except FileNotFoundError:
        pass
    else:
        raise Rsc7DecodeError("output directory already exists")
    output = requested.resolve()
    private_root = (repository / "local-analysis").resolve()
    try:
        output.relative_to(private_root)
    except ValueError as error:
        raise Rsc7DecodeError(f"output must remain below {private_root}") from error
    ignored = subprocess.run(
        ["git", "check-ignore", "--quiet", "--no-index", str(output)],
        cwd=repository,
        check=False,
    )
    if ignored.returncode != 0:
        raise Rsc7DecodeError("output path is not covered by Git ignore rules")
    return output


def _write_segments(output: Path, segments: DecodedSegments) -> None:
    output.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.mkdir(output, 0o700)
    os.chmod(output, 0o700)
    system_path = output / "system.bin"
    graphics_path = output / "graphics.bin"
    try:
        _write_exclusive(system_path, segments.system)
        _write_exclusive(graphics_path, segments.graphics)
    except BaseException:
        system_path.unlink(missing_ok=True)
        graphics_path.unlink(missing_ok=True)
        output.rmdir()
        raise


def repository_root() -> Path:
    return Path(__file__).resolve().parents[2]


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("resource", type=Path)
    parser.add_argument("private_output_directory", type=Path)
    parser.add_argument("--max-input-bytes", type=int, default=DEFAULT_MAX_INPUT)
    parser.add_argument("--max-file-bytes", type=int, default=Rsc7Limits.max_file_size)
    parser.add_argument(
        "--max-segment-bytes", type=int, default=Rsc7Limits.max_segment_size
    )
    parser.add_argument(
        "--max-decoded-bytes", type=int, default=Rsc7Limits.max_decoded_size
    )
    parser.add_argument("--max-frames", type=int, default=DEFAULT_MAX_FRAMES)
    parser.add_argument(
        "--xmem-decoder",
        type=Path,
        help="built tools/xmem_lzx executable (default: out/xmem-lzx/xmem-lzx)",
    )
    args = parser.parse_args(argv)
    try:
        if min(
            args.max_input_bytes,
            args.max_file_bytes,
            args.max_segment_bytes,
            args.max_decoded_bytes,
            args.max_frames,
        ) < 1:
            raise Rsc7DecodeError("decode limits must be positive")
        limits = Rsc7Limits(
            max_file_size=args.max_file_bytes,
            max_segment_size=args.max_segment_bytes,
            max_decoded_size=args.max_decoded_bytes,
        )
        output = _require_private_output_directory(
            args.private_output_directory, repository_root()
        )
        header, payload = _read_resource(
            args.resource.expanduser().absolute(),
            limits=limits,
            max_input_size=args.max_input_bytes,
        )
        decoder_path = (
            args.xmem_decoder.expanduser().absolute()
            if args.xmem_decoder is not None
            else repository_root() / "out/xmem-lzx/xmem-lzx"
        )
        segments = decode_and_split(
            payload,
            header,
            lambda data, expected, maximum, frames: decode_xmem_with_tool(
                data,
                expected,
                maximum,
                executable=decoder_path,
                max_frames=frames,
            ),
            max_input_size=args.max_input_bytes,
            max_frames=args.max_frames,
        )
        _write_segments(output, segments)
        print(
            json.dumps(
                {
                    "status": "decoded",
                    "version": header.version,
                    "system_bytes": len(segments.system),
                    "graphics_bytes": len(segments.graphics),
                    "decoded_bytes": header.decoded_size,
                    "source_filename_emitted": False,
                    "output_names_emitted": False,
                    "payload_emitted": False,
                    "key_material_emitted": False,
                    "output_mode": "0600",
                },
                indent=2,
            )
        )
        return 0
    except (OSError, Rpf7ExtractionError, Rsc7DecodeError, Rsc7Error) as error:
        print(f"rsc7_decode: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
