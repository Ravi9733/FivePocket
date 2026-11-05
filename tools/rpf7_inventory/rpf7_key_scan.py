#!/usr/bin/env python3
"""Locate an RPF7 AES key embedded in a privately decoded executable image.

The key is written only to an explicit local file. It is never printed, hashed,
or included in JSON output.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import stat
import struct
import sys
from typing import Callable, Sequence

from rpf7_crypto import AES_256_KEY_SIZE, decrypt_aes256_block
from rpf7_toc import KeyProvider, summarize_file_toc


DecryptBlock = Callable[[bytes, bytes], bytes]


class KeyScanError(ValueError):
    """Raised when bounded key scanning cannot proceed safely."""


def _read_regular_file(path: Path, limit: int | None = None) -> bytes:
    absolute = path.absolute()
    before = os.lstat(absolute)
    if stat.S_ISLNK(before.st_mode):
        raise KeyScanError(f"refusing to follow symbolic link: {absolute}")
    if not stat.S_ISREG(before.st_mode):
        raise KeyScanError(f"not a regular file: {absolute}")
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(absolute, flags)
    try:
        opened = os.fstat(descriptor)
        if (before.st_dev, before.st_ino) != (opened.st_dev, opened.st_ino):
            raise KeyScanError("file changed while it was being opened")
        with os.fdopen(descriptor, "rb") as stream:
            descriptor = -1
            data = stream.read() if limit is None else stream.read(limit)
            final = os.fstat(stream.fileno())
    finally:
        if descriptor >= 0:
            os.close(descriptor)
    expected_size = opened.st_size if limit is None else min(limit, opened.st_size)
    if final.st_size != opened.st_size or len(data) != expected_size:
        raise KeyScanError("file size changed while it was being read")
    return data


def looks_like_root_directory(
    plaintext: bytes,
    *,
    entry_count: int,
    names_size: int,
    name_shift: int,
) -> bool:
    """Apply public RPF7 directory bounds to one decrypted entry record."""

    if len(plaintext) != 16:
        return False
    resource_and_offset = int.from_bytes(plaintext[0:3], "big")
    stored_size = int.from_bytes(plaintext[3:6], "big")
    name_offset = int.from_bytes(plaintext[6:8], "big") << name_shift
    first_child, child_count = struct.unpack(">II", plaintext[8:16])
    return (
        resource_and_offset == 0x7FFFFF
        and stored_size == 0
        and name_offset < names_size
        and first_child <= entry_count
        and 0 < child_count <= entry_count - first_child
    )


def scan_candidates(
    image: bytes,
    encrypted_root: bytes,
    *,
    entry_count: int,
    names_size: int,
    name_shift: int,
    start: int,
    size: int,
    stride: int,
    decrypt_block: DecryptBlock = decrypt_aes256_block,
) -> list[tuple[int, bytes]]:
    if start < 0 or size < AES_256_KEY_SIZE or stride < 1:
        raise KeyScanError("invalid scan range or stride")
    end = start + size
    if end < start or end > len(image):
        raise KeyScanError("scan range extends beyond the decoded image")
    candidates: list[tuple[int, bytes]] = []
    for offset in range(start, end - AES_256_KEY_SIZE + 1, stride):
        key = image[offset : offset + AES_256_KEY_SIZE]
        plaintext = decrypt_block(encrypted_root, key)
        if looks_like_root_directory(
            plaintext,
            entry_count=entry_count,
            names_size=names_size,
            name_shift=name_shift,
        ):
            candidates.append((offset, key))
    return candidates


class _MemoryKeyProvider(KeyProvider):
    def __init__(self, key: bytes) -> None:
        self._key = key

    def aes256_key_for(self, archive: Path, encryption_tag: int) -> bytes:
        del archive, encryption_tag
        return self._key


def _write_private_key(path: Path, key: bytes) -> None:
    if len(key) != AES_256_KEY_SIZE:
        raise KeyScanError("refusing to write malformed key material")
    path.parent.mkdir(parents=True, exist_ok=True)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0)
    descriptor = os.open(path, flags, 0o600)
    try:
        written = os.write(descriptor, key)
        if written != len(key):
            raise OSError("short key-file write")
    finally:
        os.close(descriptor)


def _integer(value: str) -> int:
    return int(value, 0)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("decoded_image", type=Path)
    parser.add_argument("archive", type=Path)
    parser.add_argument("private_key_output", type=Path)
    parser.add_argument("--start", type=_integer, default=0)
    parser.add_argument("--size", type=_integer)
    parser.add_argument("--stride", type=_integer, default=16)
    args = parser.parse_args(argv)

    try:
        image = _read_regular_file(args.decoded_image)
        archive_header = _read_regular_file(args.archive, 32)
        if len(archive_header) < 32 or archive_header[:4] != b"RPF7":
            raise KeyScanError("archive does not contain an RPF7 header and root block")
        entry_count, names_info, encryption_tag = struct.unpack(">III", archive_header[4:16])
        if encryption_tag not in {0x0FFFFFF7, 0x0FFFFFF9}:
            raise KeyScanError("archive is not marked as AES-encrypted RPF7")
        names_size = names_info & 0x0FFFFFFF
        name_shift = (names_info >> 28) & 0x7
        scan_size = args.size if args.size is not None else len(image) - args.start
        candidates = scan_candidates(
            image,
            archive_header[16:32],
            entry_count=entry_count,
            names_size=names_size,
            name_shift=name_shift,
            start=args.start,
            size=scan_size,
            stride=args.stride,
        )

        verified: list[tuple[int, bytes, dict[str, object]]] = []
        for offset, key in candidates:
            try:
                summary = summarize_file_toc(args.archive, _MemoryKeyProvider(key))
            except (OSError, ValueError):
                continue
            verified.append((offset, key, summary))
        if len(verified) != 1:
            result = {
                "status": "not-found" if not verified else "ambiguous",
                "windows_scanned": max(0, (scan_size - AES_256_KEY_SIZE) // args.stride + 1),
                "root_candidates": len(candidates),
                "verified_candidates": len(verified),
                "key_material_emitted": False,
            }
            print(json.dumps(result, indent=2))
            return 1

        offset, key, toc_summary = verified[0]
        _write_private_key(args.private_key_output.absolute(), key)
        result = {
            "status": "found",
            "image_offset": f"0x{offset:X}",
            "windows_scanned": max(0, (scan_size - AES_256_KEY_SIZE) // args.stride + 1),
            "toc_summary": toc_summary,
            "key_written": True,
            "key_material_emitted_to_stdout": False,
        }
        print(json.dumps(result, indent=2))
        return 0
    except (OSError, KeyScanError, ValueError) as error:
        print(f"rpf7_key_scan: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
