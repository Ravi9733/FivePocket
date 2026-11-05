#!/usr/bin/env python3
"""Inspect Xbox 360 RPF7 headers and optionally summarize tables safely."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import stat
import struct
import sys
from typing import BinaryIO, Sequence

from rpf7_crypto import ExplicitKeyFileProvider, KeyMaterialError, KeyProvider, NoKeyProvider
from rpf7_toc import Rpf7TocError, summarize_file_toc


SCHEMA_VERSION = 2
HEADER_SIZE = 16
ENTRY_SIZE = 16
RPF7_MAGIC = b"RPF7"

# These labels describe the public header tag. Table decryption is opt-in and
# receives key bytes only through the separate provider boundary.
ENCRYPTION_TAGS = {
    0x00000000: "none",
    0x4E45504F: "open",
    0x4F50454E: "open",
    0x0FFFFFF7: "console-aes",
    0x0FFFFFF9: "aes",
    0x0FEFFFFF: "ng",
    0x0FFFFFFE: "ng",
    0x0FFFFFFF: "default-ng",
}


class Rpf7FormatError(ValueError):
    """Raised when a file cannot contain a valid Xbox 360 RPF7 header."""


def _open_regular_file_without_following(path: Path) -> tuple[BinaryIO, os.stat_result]:
    before = os.lstat(path)
    if stat.S_ISLNK(before.st_mode):
        raise OSError("refusing to follow a symbolic link")
    if not stat.S_ISREG(before.st_mode):
        raise OSError("not a regular file")

    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags)
    try:
        opened = os.fstat(descriptor)
        if not stat.S_ISREG(opened.st_mode):
            raise OSError("opened object is not a regular file")
        if (before.st_dev, before.st_ino) != (opened.st_dev, opened.st_ino):
            raise OSError("file changed while it was being opened")
        return os.fdopen(descriptor, "rb"), opened
    except BaseException:
        os.close(descriptor)
        raise


def parse_header(header: bytes, file_size: int) -> dict[str, object]:
    """Parse the unencrypted 16-byte, big-endian console RPF7 header."""

    if len(header) < HEADER_SIZE:
        raise Rpf7FormatError("RPF7 header is shorter than 16 bytes")
    if header[:4] != RPF7_MAGIC:
        raise Rpf7FormatError("not a big-endian console RPF7 file")
    if file_size < HEADER_SIZE:
        raise Rpf7FormatError("file is shorter than the RPF7 header")

    entry_count, name_info, encryption_tag = struct.unpack(">III", header[4:16])
    platform_bit = (name_info >> 31) & 1
    name_offset_shift = (name_info >> 28) & 0x7
    name_table_size = name_info & 0x0FFFFFFF

    entries_offset = HEADER_SIZE
    entries_size = entry_count * ENTRY_SIZE
    names_offset = entries_offset + entries_size
    metadata_end = names_offset + name_table_size
    metadata_within_file = metadata_end <= file_size

    encryption = ENCRYPTION_TAGS.get(encryption_tag, "unknown-nonzero")
    toc_plaintext = encryption in {"none", "open"}
    platform_hint = "xbox-360" if platform_bit == 1 else "playstation-3"

    return {
        "format": "rpf7",
        "byte_order": "big",
        "file_size": file_size,
        "entry_count": entry_count,
        "platform_bit": platform_bit,
        "platform_hint": platform_hint,
        "name_offset_shift": name_offset_shift,
        "name_offset_alignment": 1 << name_offset_shift,
        "name_table_size": name_table_size,
        "encryption_tag": f"0x{encryption_tag:08X}",
        "encryption": encryption,
        "toc_plaintext": toc_plaintext,
        "decryption_attempted": False,
        "entry_records_parsed": False,
        "metadata_layout": {
            "header_size": HEADER_SIZE,
            "entry_record_size": ENTRY_SIZE,
            "entries_offset": entries_offset,
            "entries_size": entries_size,
            "names_offset": names_offset,
            "names_size": name_table_size,
            "minimum_end_offset": metadata_end,
            "within_file": metadata_within_file,
        },
        "structurally_valid": entry_count > 0 and metadata_within_file,
    }


def inspect_file(path: Path) -> dict[str, object]:
    """Read only an RPF7 header and verify that the file did not change size."""

    stream, opened = _open_regular_file_without_following(path)
    with stream:
        header = stream.read(HEADER_SIZE)
        result = parse_header(header, opened.st_size)
        final = os.fstat(stream.fileno())
    if final.st_size != opened.st_size:
        raise OSError("file size changed while its header was being read")
    return result


def _relative_path(path: Path, root: Path) -> str:
    return "/".join(path.relative_to(root).parts)


def _walk_without_symlinks(
    root: Path,
) -> tuple[list[Path], list[str], list[dict[str, str]]]:
    files: list[Path] = []
    skipped_symlinks: list[str] = []
    errors: list[dict[str, str]] = []
    pending = [root]

    while pending:
        directory = pending.pop()
        try:
            with os.scandir(directory) as iterator:
                children = sorted(iterator, key=lambda child: child.name)
        except OSError as error:
            display = "." if directory == root else _relative_path(directory, root)
            errors.append({"path": display, "error": str(error)})
            continue

        directories: list[Path] = []
        for child in children:
            path = Path(child.path)
            display = _relative_path(path, root)
            try:
                if child.is_symlink():
                    skipped_symlinks.append(display)
                elif child.is_dir(follow_symlinks=False):
                    directories.append(path)
                elif child.is_file(follow_symlinks=False):
                    files.append(path)
            except OSError as error:
                errors.append({"path": display, "error": str(error)})
        pending.extend(reversed(directories))

    files.sort(key=lambda path: _relative_path(path, root))
    skipped_symlinks.sort()
    errors.sort(key=lambda item: item["path"])
    return files, skipped_symlinks, errors


def inventory_path(
    input_path: str | os.PathLike[str],
    *,
    summarize_toc: bool = False,
    key_provider: KeyProvider = NoKeyProvider(),
) -> dict[str, object]:
    """Find RPF7 files and optionally produce redacted aggregate TOC summaries."""

    path = Path(input_path).absolute()
    root_stat = os.lstat(path)
    if stat.S_ISLNK(root_stat.st_mode):
        raise ValueError("inventory path must not be a symbolic link")

    if stat.S_ISREG(root_stat.st_mode):
        root = path.parent
        files = [path]
        skipped_symlinks: list[str] = []
        errors: list[dict[str, str]] = []
    elif stat.S_ISDIR(root_stat.st_mode):
        root = path
        files, skipped_symlinks, errors = _walk_without_symlinks(root)
    else:
        raise ValueError("inventory path must be a regular file or directory")

    archives: list[dict[str, object]] = []
    toc_summaries_blocked = 0
    for candidate in files:
        display = (
            candidate.name
            if candidate == path and path.is_file()
            else _relative_path(candidate, root)
        )
        try:
            stream, opened = _open_regular_file_without_following(candidate)
            with stream:
                header = stream.read(HEADER_SIZE)
                final = os.fstat(stream.fileno())
            if final.st_size != opened.st_size:
                raise OSError("file size changed while its header was being read")
            if header[:4] != RPF7_MAGIC:
                if candidate.suffix.lower() == ".rpf":
                    errors.append(
                        {
                            "path": display,
                            "error": "RPF extension without RPF7 magic",
                        }
                    )
                continue
            parsed = parse_header(header, opened.st_size)
            archive_result: dict[str, object] = {"path": display, **parsed}
            if summarize_toc:
                try:
                    toc_summary = summarize_file_toc(candidate, key_provider)
                    archive_result["toc_summary"] = toc_summary
                    archive_result["decryption_attempted"] = toc_summary[
                        "table_decrypted"
                    ]
                    archive_result["entry_records_parsed"] = True
                except (KeyMaterialError, Rpf7TocError) as error:
                    toc_summaries_blocked += 1
                    archive_result["toc_summary"] = {
                        "status": "blocked",
                        "reason": str(error),
                        "names_emitted": False,
                        "payload_read": False,
                        "key_material_emitted": False,
                    }
            archives.append(archive_result)
        except (OSError, Rpf7FormatError) as error:
            errors.append({"path": display, "error": str(error)})

    errors.sort(key=lambda item: item["path"])
    archives.sort(key=lambda item: str(item["path"]))
    return {
        "schema_version": SCHEMA_VERSION,
        "mode": (
            "read-only-redacted-toc-summary"
            if summarize_toc
            else "read-only-header-inventory"
        ),
        "scanned_regular_files": len(files),
        "archives_found": len(archives),
        "toc_summaries_blocked": toc_summaries_blocked,
        "archives": archives,
        "skipped_symlinks": skipped_symlinks,
        "errors": errors,
    }


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", help="RPF7 file or extracted game directory to inspect")
    parser.add_argument("--pretty", action="store_true", help="indent JSON output")
    parser.add_argument(
        "--toc-summary",
        action="store_true",
        help="validate tables and emit aggregate counts, never entry names",
    )
    parser.add_argument(
        "--aes-key-file",
        type=Path,
        help="explicit 32-byte raw AES key file (never included in output)",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    if args.aes_key_file is not None and not args.toc_summary:
        print("rpf7_inventory: --aes-key-file requires --toc-summary", file=sys.stderr)
        return 2
    key_provider: KeyProvider = (
        ExplicitKeyFileProvider(args.aes_key_file)
        if args.aes_key_file is not None
        else NoKeyProvider()
    )
    try:
        result = inventory_path(
            args.path,
            summarize_toc=args.toc_summary,
            key_provider=key_provider,
        )
    except (KeyMaterialError, OSError, ValueError) as error:
        print(f"rpf7_inventory: {error}", file=sys.stderr)
        return 2

    json.dump(result, sys.stdout, indent=2 if args.pretty else None)
    sys.stdout.write("\n")
    return 1 if result["errors"] or result["toc_summaries_blocked"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
