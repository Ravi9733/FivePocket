#!/usr/bin/env python3
"""Recover a runtime-built RPF7 key from constant stores in private codegen.

The recovered bytes are retained in memory until they validate a complete,
bounded RPF7 table. They are then written only to an explicit Git-ignored file
and are never printed, hashed, or included in the JSON report.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
from typing import Sequence

from rpf7_crypto import AES_256_KEY_SIZE
from rpf7_toc import KeyProvider, summarize_file_toc


FUNCTION_PATTERN = re.compile(
    r"PPC_FUNC_IMPL\(__imp__(sub_[0-9A-F]+)\)\s*\{", re.IGNORECASE
)
REGISTER = r"r(?:[12]?\d|3[01]|0)"
LIS_PATTERN = re.compile(rf"lis ({REGISTER}),(-?\d+)")
LI_PATTERN = re.compile(rf"li ({REGISTER}),(-?\d+)")
ADDI_PATTERN = re.compile(rf"addi ({REGISTER}),({REGISTER}),(-?\d+)")
ORI_PATTERN = re.compile(rf"ori ({REGISTER}),({REGISTER}),(\d+)")
STORE_PATTERN = re.compile(
    rf"(stb|sth|stw) ({REGISTER}),(-?\d+)\(({REGISTER})\)"
)
CHUNK_PATTERN = re.compile(r"ppc_recomp\.(\d+)\.cpp")


class StaticKeyRecoveryError(ValueError):
    """Raised when constant-store recovery cannot prove one complete key."""


@dataclass(frozen=True)
class RecoveredStore:
    address: int
    data: bytes
    function: str
    source: str
    line: int


class _MemoryKeyProvider(KeyProvider):
    def __init__(self, key: bytes) -> None:
        self._key = key

    def aes256_key_for(self, archive: Path, encryption_tag: int) -> bytes:
        del archive, encryption_tag
        return self._key


def _signed_16(value: int) -> int:
    value &= 0xFFFF
    return value - 0x10000 if value & 0x8000 else value


def _chunk_sort_key(path: Path) -> tuple[int, str]:
    match = CHUNK_PATTERN.fullmatch(path.name)
    return (int(match.group(1)), path.name) if match else (sys.maxsize, path.name)


def recover_constant_stores(
    source_dir: Path,
    *,
    target_address: int,
    length: int = AES_256_KEY_SIZE,
) -> tuple[bytes, list[RecoveredStore]]:
    """Reconstruct a byte range written by immediate PPC store instructions."""

    if not 0 <= target_address <= 0xFFFFFFFF:
        raise StaticKeyRecoveryError("target address is outside the 32-bit guest range")
    if length < 1 or target_address + length > 0x1_0000_0000:
        raise StaticKeyRecoveryError("target byte range is invalid")

    sources = sorted(source_dir.glob("ppc_recomp.*.cpp"), key=_chunk_sort_key)
    if not sources:
        raise StaticKeyRecoveryError("no generated PPC chunks were found")

    recovered: list[int | None] = [None] * length
    stores: list[RecoveredStore] = []
    target_end = target_address + length

    for source in sources:
        if source.is_symlink() or not source.is_file():
            continue
        registers: dict[str, int] = {}
        function = "unknown"
        try:
            lines = source.read_text(encoding="utf-8").splitlines()
        except (OSError, UnicodeError) as error:
            raise StaticKeyRecoveryError(f"cannot read generated source: {source}") from error

        for line_number, line in enumerate(lines, start=1):
            function_match = FUNCTION_PATTERN.match(line)
            if function_match:
                function = function_match.group(1).upper().replace("SUB_", "sub_")
                registers.clear()
                continue

            stripped = line.strip()
            if not stripped.startswith("// "):
                continue
            instruction = stripped[3:]

            match = LIS_PATTERN.fullmatch(instruction)
            if match:
                registers[match.group(1)] = (int(match.group(2)) & 0xFFFF) << 16
                continue
            match = LI_PATTERN.fullmatch(instruction)
            if match:
                registers[match.group(1)] = _signed_16(int(match.group(2))) & 0xFFFFFFFF
                continue
            match = ADDI_PATTERN.fullmatch(instruction)
            if match:
                source_value = registers.get(match.group(2))
                if source_value is None:
                    registers.pop(match.group(1), None)
                else:
                    registers[match.group(1)] = (
                        source_value + _signed_16(int(match.group(3)))
                    ) & 0xFFFFFFFF
                continue
            match = ORI_PATTERN.fullmatch(instruction)
            if match:
                source_value = registers.get(match.group(2))
                if source_value is None:
                    registers.pop(match.group(1), None)
                else:
                    registers[match.group(1)] = source_value | (int(match.group(3)) & 0xFFFF)
                continue
            match = STORE_PATTERN.fullmatch(instruction)
            if match:
                opcode, value_register, displacement_text, base_register = match.groups()
                value = registers.get(value_register)
                base = registers.get(base_register)
                if value is None or base is None:
                    registers.clear()
                    continue
                address = (base + _signed_16(int(displacement_text))) & 0xFFFFFFFF
                width = {"stb": 1, "sth": 2, "stw": 4}[opcode]
                data = (value & ((1 << (width * 8)) - 1)).to_bytes(width, "big")
                store_end = address + width
                overlaps = address < target_end and store_end > target_address
                if overlaps and not (target_address <= address and store_end <= target_end):
                    raise StaticKeyRecoveryError(
                        f"store at {source.name}:{line_number} partially overlaps target"
                    )
                if target_address <= address and store_end <= target_end:
                    offset = address - target_address
                    for byte_index, byte_value in enumerate(data):
                        existing = recovered[offset + byte_index]
                        if existing is not None and existing != byte_value:
                            raise StaticKeyRecoveryError(
                                f"conflicting writes at guest address "
                                f"0x{address + byte_index:08X}"
                            )
                        recovered[offset + byte_index] = byte_value
                    stores.append(
                        RecoveredStore(
                            address=address,
                            data=data,
                            function=function,
                            source=source.name,
                            line=line_number,
                        )
                    )
                registers.clear()
                continue

            # Constant propagation is deliberately narrow. An unmodelled PPC
            # instruction invalidates all registers rather than risking a
            # stale value being mistaken for key material.
            registers.clear()

    missing = [index for index, value in enumerate(recovered) if value is None]
    if missing:
        raise StaticKeyRecoveryError(
            f"constant stores cover {length - len(missing)}/{length} target bytes"
        )
    return bytes(value for value in recovered if value is not None), stores


def _require_private_ignored_path(path: Path, repository: Path, label: str) -> Path:
    resolved = path.expanduser().resolve()
    private_root = (repository / "local-analysis").resolve()
    try:
        resolved.relative_to(private_root)
    except ValueError as error:
        raise StaticKeyRecoveryError(
            f"{label} must remain below {private_root}"
        ) from error
    ignored = subprocess.run(
        ["git", "check-ignore", "--quiet", "--no-index", str(resolved)],
        cwd=repository,
        check=False,
    )
    if ignored.returncode != 0:
        raise StaticKeyRecoveryError(f"{label} is not covered by Git ignore rules")
    return resolved


def _write_private_key(path: Path, key: bytes) -> None:
    if len(key) != AES_256_KEY_SIZE:
        raise StaticKeyRecoveryError("recovered AES-256 key is not exactly 32 bytes")
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
        view = memoryview(key)
        while view:
            written = os.write(descriptor, view)
            if written <= 0:
                raise OSError("short key-file write")
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


def _integer(text: str) -> int:
    return int(text, 0)


def repository_root() -> Path:
    return Path(__file__).resolve().parents[2]


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("generated_source_dir", type=Path)
    parser.add_argument("archive", type=Path)
    parser.add_argument("private_key_output", type=Path)
    parser.add_argument("--target-address", type=_integer, required=True)
    args = parser.parse_args(argv)

    try:
        root = repository_root()
        source_dir = _require_private_ignored_path(
            args.generated_source_dir, root, "generated source directory"
        )
        if not source_dir.is_dir():
            raise StaticKeyRecoveryError("generated source directory does not exist")
        output = _require_private_ignored_path(
            args.private_key_output, root, "private key output"
        )
        key, stores = recover_constant_stores(
            source_dir,
            target_address=args.target_address,
        )
        toc_summary = summarize_file_toc(args.archive, _MemoryKeyProvider(key))
        _write_private_key(output, key)
        report = {
            "status": "recovered-and-validated",
            "target_address": f"0x{args.target_address:08X}",
            "bytes_recovered": len(key),
            "constant_store_count": len(stores),
            "contributing_functions": sorted({store.function for store in stores}),
            "toc_summary": toc_summary,
            "key_written": True,
            "key_material_emitted_to_stdout": False,
        }
        print(json.dumps(report, indent=2))
        return 0
    except (OSError, StaticKeyRecoveryError, ValueError) as error:
        print(f"rpf7_static_key_recover: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
