"""Build/validate an explicit read-only, single-drive host metadata snapshot.

The catalog contains private paths and metadata, never file contents. It is not
an original XDVDFS timestamp/allocation oracle or a file-content mount.
"""
from pathlib import Path
import argparse
import os
import re
import stat
import struct
import subprocess

MAX_BYTES = 32 * 1024 * 1024
MAX_RECORDS = 16384
EPOCH = 116444736000000000
MAGIC = b"GFMETA1\0"


def canonical_path(path: str) -> str:
    if not 1 <= len(path) <= 1024 or not path.isascii():
        raise ValueError("metadata path is not bounded ASCII")
    if path.startswith("\\??\\"):
        path = path[4:]
    path = path.lower().replace("/", "\\")
    if not re.match(r"^[a-z0-9_]{1,32}:(?:\\|$)", path):
        raise ValueError("metadata path is not absolute DOS syntax")
    drive, tail = path.split(":", 1)
    if not tail or tail == "\\":
        return drive + ":\\"
    if any(ord(c) < 32 or c in '*?"<>|:' for c in tail):
        raise ValueError("metadata path has forbidden characters")
    parts = tail[1:].removesuffix("\\").split("\\")
    if any(not p or p in {".", ".."} or p.endswith((".", " ")) for p in parts):
        raise ValueError("metadata path has unsafe components")
    return drive + ":\\" + "\\".join(parts)


def parse_catalog(data: bytes) -> dict[str, tuple[int, ...]]:
    if not 16 <= len(data) <= MAX_BYTES or data[:8] != MAGIC:
        raise ValueError("invalid metadata catalog header/size")
    count, mount_size = struct.unpack_from(">II", data, 8)
    if not 1 <= count <= MAX_RECORDS or not 2 <= mount_size <= 33 or 16 + mount_size > len(data):
        raise ValueError("invalid metadata catalog counts")
    mount = data[16:16 + mount_size].decode("ascii")
    root = canonical_path(mount)
    if root != mount + "\\":
        raise ValueError("invalid metadata catalog mount")
    cursor = 16 + mount_size
    records = {}
    for _ in range(count):
        if cursor + 4 > len(data):
            raise ValueError("truncated metadata record")
        length, = struct.unpack_from(">I", data, cursor)
        cursor += 4
        if not 1 <= length <= 1024 or cursor + length + 52 > len(data):
            raise ValueError("invalid metadata record length")
        path = data[cursor:cursor + length].decode("ascii")
        cursor += length
        values = struct.unpack_from(">6QI", data, cursor)
        cursor += 52
        if path != canonical_path(path) or not path.startswith(root) or path in records:
            raise ValueError("invalid or duplicate metadata path")
        if max(values[:6]) > (1 << 63) - 1 or values[6] not in {1, 0x11} \
                or values[4] < values[5] or (values[6] == 0x11 and any(values[4:6])):
            raise ValueError("invalid read-only metadata values")
        records[path] = values
    if cursor != len(data) or records.get(root, (0,) * 7)[6] != 0x11:
        raise ValueError("missing root or trailing metadata bytes")
    for path in records:
        if path == root:
            continue
        parent = path.rsplit("\\", 1)[0]
        if parent == mount:
            parent = root
        if records.get(parent, (0,) * 7)[6] != 0x11:
            raise ValueError("missing directory parent")
    return records


def validate_catalog(data: bytes) -> int:
    return len(parse_catalog(data))


def snapshot(directory: Path, mount: str) -> bytes:
    root = canonical_path(mount)
    if root != mount + "\\" or directory.is_symlink() or not directory.is_dir():
        raise ValueError("explicit canonical root and lowercase mount are required")
    records = []
    seen = set()
    def add(relative, info):
        if not (stat.S_ISDIR(info.st_mode) or stat.S_ISREG(info.st_mode)):
            raise ValueError("snapshot rejects links and special files")
        guest = canonical_path(root + relative)
        if guest in seen:
            raise ValueError("case-folding collision in snapshot")
        seen.add(guest)
        if len(seen) > MAX_RECORDS:
            raise ValueError("snapshot record cap exceeded")
        is_dir = stat.S_ISDIR(info.st_mode)
        # Preserve observed host UTC timestamps, not invented original disc dates.
        times = [getattr(info, "st_birthtime_ns", info.st_mtime_ns), info.st_atime_ns,
                 info.st_mtime_ns, info.st_ctime_ns]
        times = [EPOCH + t // 100 for t in times]
        size = 0 if is_dir else info.st_size
        allocation = 0 if is_dir else (size + 2047) // 2048 * 2048
        encoded = guest.encode("ascii")
        record = struct.pack(">I", len(encoded)) + encoded + struct.pack(">6QI", *times, allocation, size, 0x11 if is_dir else 1)
        records.append((guest, record))
    def walk(descriptor, relative, depth):
        if depth > 64:
            raise ValueError("snapshot nesting cap exceeded")
        add(relative, os.fstat(descriptor))
        children = []
        with os.scandir(descriptor) as iterator:
            for child in iterator:
                children.append(child.name)
                if len(children) + len(seen) > MAX_RECORDS:
                    raise ValueError("snapshot record cap exceeded")
        for name in sorted(children):
            info = os.stat(name, dir_fd=descriptor, follow_symlinks=False)
            child_relative = relative + "/" + name if relative else name
            if stat.S_ISDIR(info.st_mode):
                child_fd = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=descriptor)
                try:
                    if (os.fstat(child_fd).st_dev, os.fstat(child_fd).st_ino) != (info.st_dev, info.st_ino):
                        raise ValueError("snapshot directory changed during inspection")
                    walk(child_fd, child_relative, depth + 1)
                finally:
                    os.close(child_fd)
            else:
                add(child_relative, info)
    descriptor = os.open(directory, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        walk(descriptor, "", 0)
    finally:
        os.close(descriptor)
    data = MAGIC + struct.pack(">II", len(records), len(mount)) + mount.encode("ascii")
    data += b"".join(record for _, record in sorted(records))
    validate_catalog(data)
    return data


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--mount", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    private = repo / "local-analysis"
    for path in (args.root, args.output):
        lexical = path.expanduser().absolute()
        if lexical != lexical.resolve() or lexical.is_symlink() or not lexical.is_relative_to(private) \
                or subprocess.run(["git", "check-ignore", "--quiet", "--", str(lexical)], cwd=repo).returncode:
            raise ValueError("snapshot inputs/output must be ignored private paths without links")
    if args.output.resolve().is_relative_to(args.root.resolve()):
        raise ValueError("catalog output must be outside the inspected tree")
    data = snapshot(args.root, args.mount)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, "wb") as stream:
        stream.write(data)
    print(f"Validated read-only snapshot: {validate_catalog(data)} records, {len(data)} bytes; paths kept private")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, OverflowError, struct.error):
        raise SystemExit("metadata snapshot failed validation; private diagnostics suppressed")
