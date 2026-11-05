"""Bounded recursion over binary RPF7 entries for private catalog generation."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import stat
from typing import Callable, Sequence

from rpf7_catalog import CatalogEntry, parse_catalog_entries
from rpf7_crypto import KeyProvider
from rpf7_extract import decode_binary_payload, read_payload_extent
from rpf7_toc import TocHeader, TocLimits, read_bytes_toc


XmemDecoder = Callable[[bytes, int, int], bytes]


class NestedCatalogError(ValueError):
    """Raised when recursive archive traversal cannot remain bounded."""


@dataclass(frozen=True)
class NestedCatalogLimits:
    max_depth: int = 4
    max_archives: int = 64
    max_single_archive_size: int = 256 * 1024 * 1024
    max_total_decoded_size: int = 512 * 1024 * 1024
    max_root_archive_size: int = 4 * 1024 * 1024 * 1024


@dataclass(frozen=True)
class NestedArchive:
    archive_id: int
    parent_archive_id: int
    source_entry_index: int
    depth: int
    path: str
    header: TocHeader
    entries: tuple[CatalogEntry, ...]


@dataclass(frozen=True)
class _ArchiveSource:
    archive_id: int
    depth: int
    path: str
    identity: Path
    header: TocHeader
    entries: tuple[CatalogEntry, ...]
    data: bytes | None
    ancestor_digests: frozenset[bytes]


def _hash_root_archive(path: Path, max_size: int) -> bytes:
    before = os.lstat(path)
    if stat.S_ISLNK(before.st_mode):
        raise NestedCatalogError("refusing to follow a root archive symbolic link")
    if not stat.S_ISREG(before.st_mode):
        raise NestedCatalogError("root archive is not a regular file")
    if before.st_size > max_size:
        raise NestedCatalogError("root archive exceeds the configured recursion limit")
    descriptor = os.open(
        path,
        os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0),
    )
    digest = hashlib.sha256()
    try:
        opened = os.fstat(descriptor)
        if (before.st_dev, before.st_ino) != (opened.st_dev, opened.st_ino):
            raise NestedCatalogError("root archive changed while it was being opened")
        while chunk := os.read(descriptor, 1024 * 1024):
            digest.update(chunk)
        final = os.fstat(descriptor)
    finally:
        os.close(descriptor)
    if final.st_size != opened.st_size:
        raise NestedCatalogError("root archive size changed while hashing")
    return digest.digest()


def _nested_candidates(entries: Sequence[CatalogEntry]) -> list[CatalogEntry]:
    return [
        entry
        for entry in entries
        if entry.kind == "binary" and entry.name.casefold().endswith(".rpf")
    ]


def scan_nested_archives(
    root_archive: Path,
    root_header: TocHeader,
    root_entries: Sequence[CatalogEntry],
    key_provider: KeyProvider,
    *,
    limits: NestedCatalogLimits = NestedCatalogLimits(),
    toc_limits: TocLimits = TocLimits(),
    xmem_decoder: XmemDecoder | None = None,
) -> list[NestedArchive]:
    """Decode and catalog nested RPF binaries without emitting their contents."""

    if min(
        limits.max_archives,
        limits.max_single_archive_size,
        limits.max_total_decoded_size,
        limits.max_root_archive_size,
    ) < 1 or limits.max_depth < 0:
        raise NestedCatalogError("nested archive limits must be positive")

    root_archive = root_archive.absolute()
    root_digest = _hash_root_archive(root_archive, limits.max_root_archive_size)
    pending = [
        _ArchiveSource(
            archive_id=0,
            depth=0,
            path="",
            identity=root_archive,
            header=root_header,
            entries=tuple(root_entries),
            data=None,
            ancestor_digests=frozenset({root_digest}),
        )
    ]
    result: list[NestedArchive] = []
    total_decoded = 0

    while pending:
        parent = pending.pop()
        for entry in _nested_candidates(parent.entries):
            depth = parent.depth + 1
            if depth > limits.max_depth:
                raise NestedCatalogError("nested archive depth exceeds the configured limit")
            if len(result) >= limits.max_archives:
                raise NestedCatalogError("nested archive count exceeds the configured limit")
            if entry.logical_size is None or entry.stored_size is None:
                raise NestedCatalogError("nested archive entry has incomplete size metadata")
            if entry.logical_size < 16 or entry.logical_size > limits.max_single_archive_size:
                raise NestedCatalogError("nested archive exceeds the configured per-file limit")
            if entry.stored_size < 1:
                raise NestedCatalogError("nested archive has an empty stored payload")
            if entry.payload_offset is None:
                raise NestedCatalogError("nested archive has no payload extent")
            if total_decoded > limits.max_total_decoded_size - entry.logical_size:
                raise NestedCatalogError("nested archives exceed the configured total-size limit")

            if parent.data is None:
                payload = read_payload_extent(
                    root_archive, entry.payload_offset, entry.stored_size
                )
            else:
                start = entry.payload_offset
                end = start + entry.stored_size
                if start < 0 or end < start or end > len(parent.data):
                    raise NestedCatalogError("nested payload extent is outside its parent archive")
                payload = parent.data[start:end]

            key = None
            if entry.encrypted:
                key = key_provider.aes256_key_for(
                    parent.identity, parent.header.encryption_tag
                )
            decoded = decode_binary_payload(
                payload,
                encrypted=bool(entry.encrypted),
                compressed=bool(entry.compressed),
                logical_size=entry.logical_size,
                key=key,
                max_output_size=limits.max_single_archive_size,
                xmem_decoder=xmem_decoder,
            )
            total_decoded += len(decoded)
            digest = hashlib.sha256(decoded).digest()
            if digest in parent.ancestor_digests:
                raise NestedCatalogError("nested archive cycle detected")

            private_path = f"{parent.path}!/{entry.path}" if parent.path else entry.path
            identity = Path(private_path)
            header, entry_bytes, name_bytes, decrypted = read_bytes_toc(
                decoded,
                key_provider,
                archive_identity=identity,
                limits=toc_limits,
            )
            entries = tuple(
                parse_catalog_entries(
                    entry_bytes,
                    name_bytes,
                    header=header,
                    limits=toc_limits,
                    decrypted=decrypted,
                )
            )
            archive_id = len(result) + 1
            result.append(
                NestedArchive(
                    archive_id=archive_id,
                    parent_archive_id=parent.archive_id,
                    source_entry_index=entry.index,
                    depth=depth,
                    path=private_path,
                    header=header,
                    entries=entries,
                )
            )
            pending.append(
                _ArchiveSource(
                    archive_id=archive_id,
                    depth=depth,
                    path=private_path,
                    identity=identity,
                    header=header,
                    entries=entries,
                    data=decoded,
                    ancestor_digests=parent.ancestor_digests | {digest},
                )
            )
    return result
