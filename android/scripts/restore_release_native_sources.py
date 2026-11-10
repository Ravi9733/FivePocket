#!/usr/bin/env python3
"""Restore the five Android native files omitted from the public Git tree.

The upstream release source archive is pinned by SHA-256. Only exact,
CMake-referenced basenames are extracted after checking archive paths/types.
"""
from __future__ import annotations

import hashlib
import io
import tarfile
import urllib.request
from pathlib import Path, PurePosixPath

URL = "https://github.com/marcm0de/FivePocket/releases/download/v1.1.0-alpha.1/FivePocket-1.1.0-alpha.1-source.tar.xz"
SHA256 = "b2d7d611b44b48b2b4056b6b9ff8685f50618ae4761ee338023d21bb26bbab51"
ROOT = Path(__file__).resolve().parents[1]
RELATIVE = Path("app/src/main/cpp")
NAMES = (
    "smoke_jni.cpp",
    "smoke_runtime.cpp",
    "vulkan_smoke.cpp",
    "vulkan_smoke.h",
    "shader_constant_upload.cpp",
)
MAX_ARCHIVE = 32 * 1024 * 1024
MAX_MEMBER = 8 * 1024 * 1024


def main() -> None:
    request = urllib.request.Request(URL, headers={"User-Agent": "FivePocket-source-restore/1.0"})
    with urllib.request.urlopen(request, timeout=60) as response:
        archive_bytes = response.read(MAX_ARCHIVE + 1)
    if len(archive_bytes) > MAX_ARCHIVE:
        raise SystemExit("source archive exceeds the 32 MiB safety limit")
    digest = hashlib.sha256(archive_bytes).hexdigest()
    if digest != SHA256:
        raise SystemExit(f"source archive SHA-256 mismatch: {digest}")

    with tarfile.open(fileobj=io.BytesIO(archive_bytes), mode="r:xz") as archive:
        members = archive.getmembers()
        for name in NAMES:
            # Release source bundles may store a path prefix different from the
            # Git checkout. Match the exact basename, then require uniqueness.
            matches = [
                member for member in members
                if PurePosixPath(member.name).name == name
            ]
            if len(matches) != 1:
                candidates = [
                    member.name for member in members
                    if PurePosixPath(member.name).name == name
                ]
                raise SystemExit(
                    f"expected exactly one archived source named {name}; "
                    f"found {len(matches)}; candidates={candidates}"
                )
            member = matches[0]
            if not member.isfile() or member.size <= 0 or member.size > MAX_MEMBER:
                raise SystemExit(f"unsafe or invalid archive member for {name}: {member.name}")
            source = archive.extractfile(member)
            if source is None:
                raise SystemExit(f"cannot read archived source {name}: {member.name}")
            data = source.read(MAX_MEMBER + 1)
            if len(data) != member.size or len(data) > MAX_MEMBER:
                raise SystemExit(f"invalid size for archived source {name}")
            destination = ROOT / RELATIVE / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
            print(f"RESTORED {destination.relative_to(ROOT)} ({len(data)} bytes)")

    print("Verified upstream native sources restored successfully.")


if __name__ == "__main__":
    main()
