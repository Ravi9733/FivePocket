#!/usr/bin/env python3
"""Extract one cataloged RPF7 binary entry into ignored local analysis."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sqlite3
import stat
import subprocess
import sys
from typing import Callable, Sequence

from rpf7_crypto import ExplicitKeyFileProvider, decrypt_aes256_ecb_prefix


DEFAULT_MAX_OUTPUT = 512 * 1024 * 1024
Decryptor = Callable[[bytes, bytes], bytes]
XmemDecoder = Callable[[bytes, int, int], bytes]


class Rpf7ExtractionError(ValueError):
    """Raised when a selected entry cannot be extracted safely."""


def decode_binary_payload(
    payload: bytes,
    *,
    encrypted: bool,
    compressed: bool,
    logical_size: int,
    key: bytes | None,
    max_output_size: int = DEFAULT_MAX_OUTPUT,
    decryptor: Decryptor = decrypt_aes256_ecb_prefix,
    xmem_decoder: XmemDecoder | None = None,
) -> bytes:
    if logical_size < 0 or logical_size > max_output_size:
        raise Rpf7ExtractionError("logical size exceeds the configured output limit")
    if encrypted:
        if key is None:
            raise Rpf7ExtractionError("encrypted entry requires an AES-256 key")
        payload = decryptor(payload, key)
    if compressed:
        if xmem_decoder is None:
            raise Rpf7ExtractionError("compressed Xbox entry requires an XMem/LZX decoder")
        output = xmem_decoder(payload, logical_size, max_output_size)
    else:
        output = payload
    if len(output) != logical_size:
        raise Rpf7ExtractionError(
            f"decoded size {len(output)} does not match catalog size {logical_size}"
        )
    return output


def decode_xmem_with_tool(
    payload: bytes,
    expected_size: int,
    max_output_size: int,
    *,
    executable: Path,
    max_frames: int = 8192,
) -> bytes:
    if max_frames < 1:
        raise Rpf7ExtractionError("XMem/LZX frame limit must be positive")
    before = _regular_stat(executable, "XMem/LZX decoder")
    if not (before.st_mode & stat.S_IXUSR):
        raise Rpf7ExtractionError("XMem/LZX decoder is not executable")
    completed = subprocess.run(
        [
            str(executable),
            "--expected-size",
            str(expected_size),
            "--max-input",
            str(max(1, len(payload))),
            "--max-output",
            str(max_output_size),
            "--max-frames",
            str(max_frames),
            "-",
            "-",
        ],
        input=payload,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != 0:
        raise Rpf7ExtractionError(
            f"XMem/LZX decoder failed with exit {completed.returncode}"
        )
    if len(completed.stdout) != expected_size:
        raise Rpf7ExtractionError("XMem/LZX decoder returned an unexpected byte count")
    return completed.stdout


def _regular_stat(path: Path, label: str) -> os.stat_result:
    before = os.lstat(path)
    if stat.S_ISLNK(before.st_mode):
        raise Rpf7ExtractionError(f"refusing to follow {label} symbolic link")
    if not stat.S_ISREG(before.st_mode):
        raise Rpf7ExtractionError(f"{label} is not a regular file")
    return before


def read_payload_extent(path: Path, offset: int, size: int) -> bytes:
    before = _regular_stat(path, "archive")
    if offset < 0 or size < 0 or offset > before.st_size or size > before.st_size - offset:
        raise Rpf7ExtractionError("cataloged payload extent is outside the archive")
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags)
    try:
        opened = os.fstat(descriptor)
        if (before.st_dev, before.st_ino) != (opened.st_dev, opened.st_ino):
            raise Rpf7ExtractionError("archive changed while it was being opened")
        os.lseek(descriptor, offset, os.SEEK_SET)
        chunks: list[bytes] = []
        remaining = size
        while remaining:
            chunk = os.read(descriptor, min(remaining, 1024 * 1024))
            if not chunk:
                raise Rpf7ExtractionError("archive ended inside the selected payload")
            chunks.append(chunk)
            remaining -= len(chunk)
        final = os.fstat(descriptor)
    finally:
        os.close(descriptor)
    if final.st_size != opened.st_size:
        raise Rpf7ExtractionError("archive size changed while reading the payload")
    return b"".join(chunks)


def _catalog_row(
    catalog: Path,
    *,
    entry_index: int | None,
    entry_path: str | None,
) -> tuple[dict[str, str], tuple[object, ...]]:
    _regular_stat(catalog, "catalog")
    connection = sqlite3.connect(f"{catalog.as_uri()}?mode=ro", uri=True)
    try:
        metadata = dict(connection.execute("SELECT key, value FROM metadata"))
        columns = (
            "entry_index, kind, path, payload_offset, stored_size, "
            "logical_size, compressed, encrypted"
        )
        if entry_index is not None:
            rows = connection.execute(
                f"SELECT {columns} FROM entries WHERE entry_index = ?", (entry_index,)
            ).fetchall()
        else:
            rows = connection.execute(
                f"SELECT {columns} FROM entries WHERE path = ?", (entry_path,)
            ).fetchall()
    finally:
        connection.close()
    if len(rows) != 1:
        raise Rpf7ExtractionError("entry selector did not identify exactly one record")
    return metadata, rows[0]


def _require_private_output(path: Path, repository: Path) -> Path:
    output = path.expanduser().resolve()
    private_root = (repository / "local-analysis").resolve()
    try:
        output.relative_to(private_root)
    except ValueError as error:
        raise Rpf7ExtractionError(f"output must remain below {private_root}") from error
    ignored = subprocess.run(
        ["git", "check-ignore", "--quiet", "--no-index", str(output)],
        cwd=repository,
        check=False,
    )
    if ignored.returncode != 0:
        raise Rpf7ExtractionError("output path is not covered by Git ignore rules")
    return output


def _write_exclusive(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    flags = (
        os.O_WRONLY
        | os.O_CREAT
        | os.O_EXCL
        | getattr(os, "O_BINARY", 0)
        | getattr(os, "O_NOFOLLOW", 0)
    )
    descriptor = os.open(path, flags, 0o600)
    try:
        if hasattr(os, "fchmod"):
            os.fchmod(descriptor, 0o600)
        view = memoryview(data)
        while view:
            written = os.write(descriptor, view)
            if written <= 0:
                raise OSError("short output write")
            view = view[written:]
        os.fsync(descriptor)
    except BaseException:
        os.close(descriptor)
        descriptor = -1
        path.unlink(missing_ok=True)
        raise
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def repository_root() -> Path:
    return Path(__file__).resolve().parents[2]


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("catalog", type=Path)
    parser.add_argument("archive", type=Path)
    parser.add_argument("private_key_file", type=Path)
    parser.add_argument("private_output", type=Path)
    selector = parser.add_mutually_exclusive_group(required=True)
    selector.add_argument("--entry-index", type=int)
    selector.add_argument("--entry-path")
    parser.add_argument("--max-output-bytes", type=int, default=DEFAULT_MAX_OUTPUT)
    parser.add_argument(
        "--xmem-decoder",
        type=Path,
        help="built tools/xmem_lzx executable (default: out/xmem-lzx/xmem-lzx)",
    )
    args = parser.parse_args(argv)

    try:
        if args.max_output_bytes < 1:
            raise Rpf7ExtractionError("--max-output-bytes must be positive")
        catalog = args.catalog.expanduser().absolute()
        archive = args.archive.expanduser().absolute()
        output = _require_private_output(args.private_output, repository_root())
        metadata, row = _catalog_row(
            catalog,
            entry_index=args.entry_index,
            entry_path=args.entry_path,
        )
        (
            entry_index,
            kind,
            _private_path,
            payload_offset,
            stored_size,
            logical_size,
            compressed,
            encrypted,
        ) = row
        if kind != "binary":
            raise Rpf7ExtractionError("selected catalog record is not a binary entry")
        archive_stat = _regular_stat(archive, "archive")
        if metadata.get("archive_filename") != archive.name:
            raise Rpf7ExtractionError("catalog archive filename does not match input")
        if metadata.get("archive_size") != str(archive_stat.st_size):
            raise Rpf7ExtractionError("catalog archive size does not match input")
        payload = read_payload_extent(archive, int(payload_offset), int(stored_size))
        key = (
            ExplicitKeyFileProvider(args.private_key_file).aes256_key_for(
                archive, int(metadata["encryption_tag"], 0)
            )
            if bool(encrypted)
            else None
        )
        decoded = decode_binary_payload(
            payload,
            encrypted=bool(encrypted),
            compressed=bool(compressed),
            logical_size=int(logical_size),
            key=key,
            max_output_size=args.max_output_bytes,
            xmem_decoder=(
                lambda data, expected, limit: decode_xmem_with_tool(
                    data,
                    expected,
                    limit,
                    executable=(
                        args.xmem_decoder.expanduser().absolute()
                        if args.xmem_decoder is not None
                        else repository_root() / "out/xmem-lzx/xmem-lzx"
                    ),
                )
                if bool(compressed)
                else None
            ),
        )
        _write_exclusive(output, decoded)
        print(
            json.dumps(
                {
                    "status": "extracted",
                    "entry_index": entry_index,
                    "stored_bytes": len(payload),
                    "decoded_bytes": len(decoded),
                    "was_encrypted": bool(encrypted),
                    "was_compressed": bool(compressed),
                    "output_filename": output.name,
                    "entry_name_emitted": False,
                    "payload_emitted_to_stdout": False,
                    "key_material_emitted": False,
                },
                indent=2,
            )
        )
        return 0
    except (KeyError, OSError, Rpf7ExtractionError, sqlite3.Error, ValueError) as error:
        print(f"rpf7_extract: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
