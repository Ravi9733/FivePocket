"""Bounded, name-redacting parser for Xbox 360 RPF7 tables of contents."""

from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
import stat
import struct
from typing import BinaryIO, Callable

from rpf7_crypto import (
    KeyMaterialError,
    KeyProvider,
    decrypt_aes256_ecb_prefix,
)


HEADER_SIZE = 16
ENTRY_SIZE = 16
RPF7_MAGIC = b"RPF7"
BLOCK_SIZE = 512

PLAINTEXT_TAGS = frozenset({0x00000000, 0x4E45504F, 0x4F50454E})
AES_256_ECB_TAGS = frozenset({0x0FFFFFF7, 0x0FFFFFF9})
NG_TAGS = frozenset({0x0FEFFFFF, 0x0FFFFFFE, 0x0FFFFFFF})


class Rpf7TocError(ValueError):
    """Raised when an RPF7 table cannot be safely read or validated."""


class UnsupportedEncryptionError(Rpf7TocError):
    """Raised for an encryption family this deliberately narrow parser lacks."""


@dataclass(frozen=True)
class TocLimits:
    """Hard allocation and scanning limits for untrusted archive metadata."""

    max_entries: int = 1_000_000
    max_names_size: int = 64 * 1024 * 1024
    max_metadata_size: int = 96 * 1024 * 1024
    max_name_bytes: int = 4 * 1024


@dataclass(frozen=True)
class TocHeader:
    entry_count: int
    names_size: int
    name_shift: int
    encryption_tag: int
    file_size: int

    @property
    def entries_size(self) -> int:
        return self.entry_count * ENTRY_SIZE

    @property
    def metadata_size(self) -> int:
        return self.entries_size + self.names_size


Decryptor = Callable[[bytes, bytes], bytes]


def _open_regular_file_without_following(path: Path) -> tuple[BinaryIO, os.stat_result]:
    before = os.lstat(path)
    if stat.S_ISLNK(before.st_mode):
        raise OSError("refusing to follow an archive symbolic link")
    if not stat.S_ISREG(before.st_mode):
        raise OSError("archive path is not a regular file")

    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags)
    try:
        opened = os.fstat(descriptor)
        if not stat.S_ISREG(opened.st_mode):
            raise OSError("opened archive object is not a regular file")
        if (before.st_dev, before.st_ino) != (opened.st_dev, opened.st_ino):
            raise OSError("archive changed while it was being opened")
        return os.fdopen(descriptor, "rb"), opened
    except BaseException:
        os.close(descriptor)
        raise


def parse_toc_header(header: bytes, file_size: int, limits: TocLimits = TocLimits()) -> TocHeader:
    """Validate the public big-endian console header before allocating tables."""

    if len(header) != HEADER_SIZE or header[:4] != RPF7_MAGIC:
        raise Rpf7TocError("not a complete big-endian RPF7 header")
    entry_count, names_info, encryption_tag = struct.unpack(">III", header[4:])
    names_size = names_info & 0x0FFFFFFF
    name_shift = (names_info >> 28) & 0x7
    parsed = TocHeader(entry_count, names_size, name_shift, encryption_tag, file_size)

    if entry_count == 0:
        raise Rpf7TocError("table declares no entries")
    if entry_count > limits.max_entries:
        raise Rpf7TocError("entry count exceeds the configured safety limit")
    if names_size == 0:
        raise Rpf7TocError("table declares an empty name buffer")
    if names_size > limits.max_names_size:
        raise Rpf7TocError("name buffer exceeds the configured safety limit")
    if parsed.metadata_size > limits.max_metadata_size:
        raise Rpf7TocError("metadata exceeds the configured safety limit")
    if HEADER_SIZE + parsed.metadata_size > file_size:
        raise Rpf7TocError("declared metadata extends beyond the archive")
    return parsed


def _decrypt_tables(
    entries: bytes,
    names: bytes,
    *,
    header: TocHeader,
    archive: Path,
    key_provider: KeyProvider,
    decryptor: Decryptor,
) -> tuple[bytes, bytes, bool]:
    tag = header.encryption_tag
    if tag in PLAINTEXT_TAGS:
        return entries, names, False
    if tag in NG_TAGS:
        raise UnsupportedEncryptionError(
            "NG/TFIT table encryption is not implemented; an AES key is insufficient"
        )
    if tag not in AES_256_ECB_TAGS:
        raise UnsupportedEncryptionError(f"unsupported RPF7 encryption tag 0x{tag:08X}")

    key = key_provider.aes256_key_for(archive, tag)
    if key is None:
        raise KeyMaterialError("no AES-256 key is available for this archive")
    if len(key) != 32:
        raise KeyMaterialError("key provider returned a value other than 32 bytes")
    entries_plaintext = decryptor(entries, key)
    names_plaintext = decryptor(names, key)
    if len(entries_plaintext) != len(entries) or len(names_plaintext) != len(names):
        raise Rpf7TocError("decryptor changed a table buffer's length")
    return entries_plaintext, names_plaintext, True


def parse_toc_buffers(
    entries: bytes,
    names: bytes,
    *,
    header: TocHeader,
    limits: TocLimits = TocLimits(),
    decrypted: bool,
) -> dict[str, object]:
    """Validate table records and names while returning aggregate metadata only."""

    if len(entries) != header.entries_size or len(names) != header.names_size:
        raise Rpf7TocError("table buffer length does not match the header")

    directories = 0
    binary_files = 0
    resources = 0
    compressed_files = 0
    encrypted_file_flags = 0
    referenced_name_bytes = 0
    payload_extents_checked = 0

    for index in range(header.entry_count):
        record = entries[index * ENTRY_SIZE : (index + 1) * ENTRY_SIZE]
        resource_and_offset = int.from_bytes(record[0:3], "big")
        is_resource = bool(resource_and_offset & 0x800000)
        offset_blocks = resource_and_offset & 0x7FFFFF
        stored_size = int.from_bytes(record[3:6], "big")
        name_offset_units = int.from_bytes(record[6:8], "big")
        field5, field6 = struct.unpack(">II", record[8:16])

        name_offset = name_offset_units << header.name_shift
        if name_offset >= len(names):
            raise Rpf7TocError(f"entry {index} has a name offset outside the table")
        name_end_limit = min(len(names), name_offset + limits.max_name_bytes + 1)
        name_end = names.find(b"\0", name_offset, name_end_limit)
        if name_end < 0:
            raise Rpf7TocError(f"entry {index} has no bounded null-terminated name")
        referenced_name_bytes += name_end - name_offset

        if offset_blocks == 0x7FFFFF:
            if is_resource:
                raise Rpf7TocError(f"directory entry {index} is marked as a resource")
            if field5 > header.entry_count or field6 > header.entry_count - field5:
                raise Rpf7TocError(f"directory entry {index} has an invalid child range")
            directories += 1
            continue

        payload_offset = offset_blocks * BLOCK_SIZE
        if is_resource:
            if stored_size == 0xFFFFFF:
                raise Rpf7TocError(f"resource entry {index} uses an unsupported size sentinel")
            payload_size = stored_size
            resources += 1
        else:
            payload_size = field5 if stored_size == 0 else stored_size
            binary_files += 1
            compressed_files += int(stored_size != 0)
            encrypted_file_flags += int(field6 != 0)

        if payload_offset > header.file_size or payload_size > header.file_size - payload_offset:
            raise Rpf7TocError(f"file entry {index} has a payload extent outside the archive")
        payload_extents_checked += 1

    if directories == 0:
        raise Rpf7TocError("table contains no directory entry")
    first_offset = int.from_bytes(entries[0:3], "big") & 0x7FFFFF
    if first_offset != 0x7FFFFF:
        raise Rpf7TocError("first table entry is not the root directory")

    return {
        "status": "parsed",
        "table_decrypted": decrypted,
        "entry_count": header.entry_count,
        "directory_count": directories,
        "binary_file_count": binary_files,
        "resource_file_count": resources,
        "compressed_binary_count": compressed_files,
        "encrypted_file_flag_count": encrypted_file_flags,
        "names_validated": header.entry_count,
        "referenced_name_bytes": referenced_name_bytes,
        "payload_extents_checked": payload_extents_checked,
        "names_emitted": False,
        "payload_read": False,
        "key_material_emitted": False,
    }


def summarize_file_toc(
    path: str | os.PathLike[str],
    key_provider: KeyProvider,
    *,
    limits: TocLimits = TocLimits(),
    decryptor: Decryptor = decrypt_aes256_ecb_prefix,
) -> dict[str, object]:
    """Read, optionally decrypt, and validate only a bounded RPF7 TOC."""

    header, entries, names, decrypted = read_file_toc(
        path,
        key_provider,
        limits=limits,
        decryptor=decryptor,
    )
    return parse_toc_buffers(
        entries,
        names,
        header=header,
        limits=limits,
        decrypted=decrypted,
    )


def read_file_toc(
    path: str | os.PathLike[str],
    key_provider: KeyProvider,
    *,
    limits: TocLimits = TocLimits(),
    decryptor: Decryptor = decrypt_aes256_ecb_prefix,
) -> tuple[TocHeader, bytes, bytes, bool]:
    """Read and decrypt bounded table buffers for a private downstream tool."""

    archive = Path(path).absolute()
    stream, opened = _open_regular_file_without_following(archive)
    with stream:
        header_bytes = stream.read(HEADER_SIZE)
        header = parse_toc_header(header_bytes, opened.st_size, limits)
        entries = stream.read(header.entries_size)
        names = stream.read(header.names_size)
        final = os.fstat(stream.fileno())
    if final.st_size != opened.st_size:
        raise OSError("archive size changed while metadata was being read")
    if len(entries) != header.entries_size or len(names) != header.names_size:
        raise Rpf7TocError("archive ended while its metadata was being read")

    entries, names, decrypted = _decrypt_tables(
        entries,
        names,
        header=header,
        archive=archive,
        key_provider=key_provider,
        decryptor=decryptor,
    )
    return header, entries, names, decrypted


def read_bytes_toc(
    data: bytes,
    key_provider: KeyProvider,
    *,
    archive_identity: Path,
    limits: TocLimits = TocLimits(),
    decryptor: Decryptor = decrypt_aes256_ecb_prefix,
) -> tuple[TocHeader, bytes, bytes, bool]:
    """Read one bounded nested RPF7 TOC from an already decoded byte buffer."""

    if len(data) < HEADER_SIZE:
        raise Rpf7TocError("nested archive is shorter than the RPF7 header")
    header = parse_toc_header(data[:HEADER_SIZE], len(data), limits)
    entries_start = HEADER_SIZE
    entries_end = entries_start + header.entries_size
    names_end = entries_end + header.names_size
    entries, names, decrypted = _decrypt_tables(
        data[entries_start:entries_end],
        data[entries_end:names_end],
        header=header,
        archive=archive_identity,
        key_provider=key_provider,
        decryptor=decryptor,
    )
    return header, entries, names, decrypted
