#!/usr/bin/env python3
"""Build a private, ambiguity-preserving raw-microcode to SPIR-V catalog."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
from typing import Sequence

from fxc_xenos_inspect import FxcLimits, _parse_xenos_candidate
from rpf7_extract import _write_exclusive
from xenos_preflight_batch import (
    BatchLimits, XenosPreflightError, _prepare_inputs, _read_private_input,
    _require_private_report, _require_validator, repository_root,
)
from xenos_translate_batch import ShaderTranslationError, run_worker, spirv_metadata


class ShaderCatalogError(ValueError):
    pass


def microcode_identity(container: bytes, stage: str) -> dict[str, object]:
    """Exact stored big-endian code bytes, not a guessed swapped-word variant."""
    outer = _parse_xenos_candidate(container, 0, FxcLimits())
    if outer is None or outer.size != len(container) or outer.stage != stage:
        raise ShaderCatalogError("container extent or stage is inconsistent")
    offset, size = struct.unpack_from(">II", container, outer.shader_offset)
    if not size or size % 12 or offset % 4 or offset > outer.physical_size or size > outer.physical_size - offset:
        raise ShaderCatalogError("microcode range is not a complete bounded instruction stream")
    begin = outer.virtual_size + offset
    return {
        "stage": stage, "microcode_bytes": size,
        "microcode_sha256": hashlib.sha256(container[begin:begin + size]).hexdigest(),
        "container_sha256": hashlib.sha256(container).hexdigest(),
    }


def group_entries(entries: Sequence[dict[str, object]], binding: str) -> dict[str, object]:
    groups: dict[tuple[str, str, int], dict[str, list[int]]] = {}
    for entry in entries:
        key = str(entry["stage"]), str(entry["microcode_sha256"]), int(entry["microcode_bytes"])
        candidates = groups.setdefault(key, {})
        candidates.setdefault(str(entry["spirv_sha256"]), []).append(int(entry["index"]))
    records = []
    for (stage, digest, size), candidates in sorted(groups.items()):
        records.append({
            "stage": stage, "microcode_sha256": digest, "microcode_bytes": size,
            "resolution": "unique" if len(candidates) == 1 else "ambiguous",
            "candidates": [{"spirv_sha256": key, "indices": sorted(value)}
                           for key, value in sorted(candidates.items())],
        })
    return {
        "schema_version": 1, "constant_binding": binding,
        "microcode_encoding": "exact-stored-big-endian-bytes",
        "artifact_count": len(entries), "lookup_key_count": len(records),
        "ambiguous_key_count": sum(r["resolution"] == "ambiguous" for r in records),
        "artifacts": list(entries), "lookups": records,
        "runtime_lookup_integrated": False, "payload_emitted": False,
    }


def build_catalog(conversion: Path, *, repository: Path, validator: Path) -> dict[str, object]:
    conversion = Path(os.path.abspath(conversion.expanduser()))
    validator = _require_validator(validator)
    metadata_inputs = _prepare_inputs([conversion / "report.json", conversion / "artifact-identities.json"],
        repository, BatchLimits(max_files=2, max_file_bytes=4 * 1024 * 1024))
    report, identities = (json.loads(_read_private_input(item)) for item in metadata_inputs)
    if not isinstance(report, dict) or not isinstance(identities, dict):
        raise ShaderCatalogError("conversion metadata must be objects")
    records = report.get("records")
    binding = report.get("constant_binding")
    if (report.get("schema_version") != 1 or report.get("status") not in ("validated", "partial") or
        binding not in ("device-address", "descriptor32") or not isinstance(records, list) or
        not 1 <= len(records) <= 1024 or report.get("completed_container_count") != len(records)):
        raise ShaderCatalogError("conversion report is incomplete or unsupported")
    selected = []
    indices: set[int] = set()
    for record in records:
        if not isinstance(record, dict):
            raise ShaderCatalogError("conversion record must be an object")
        index = record.get("index")
        if type(index) is not int or not 1 <= index <= 1024 or index in indices:
            raise ShaderCatalogError("conversion indices are invalid or duplicated")
        indices.add(index)
        if record.get("status") != "validated":
            continue
        if record.get("stage") not in ("vertex", "pixel"):
            raise ShaderCatalogError("conversion stage is unsupported")
        selected.append(record)
    if not selected:
        raise ShaderCatalogError("there are no validated conversion artifacts")
    if (identities.get("schema_version") != 1 or identities.get("constant_binding") != binding or
        not isinstance(identities.get("artifacts"), list) or len(identities["artifacts"]) != len(selected)):
        raise ShaderCatalogError("conversion integrity records are missing")
    hashes = {}
    for identity in identities["artifacts"]:
        if (not isinstance(identity, dict) or type(identity.get("index")) is not int or
            identity["index"] in hashes):
            raise ShaderCatalogError("invalid conversion integrity record")
        hashes[identity["index"]] = identity
    paths = [conversion / f"shader-{r['index']:04d}" / name
             for r in selected for name in ("container.xenos", "translated.spv")]
    prepared = _prepare_inputs(paths, repository, BatchLimits(
        max_files=2048, max_file_bytes=16 * 1024 * 1024,
        max_aggregate_bytes=128 * 1024 * 1024,
    ))
    entries = []
    for i, record in enumerate(selected):
        container = _read_private_input(prepared[i * 2])
        binary = _read_private_input(prepared[i * 2 + 1])
        identity = hashes.get(record["index"], {})
        if (identity.get("container_sha256") != hashlib.sha256(container).hexdigest() or
            identity.get("spirv_sha256") != hashlib.sha256(binary).hexdigest()):
            raise ShaderCatalogError("artifact changed after validated conversion")
        metadata = spirv_metadata(binary, record["stage"])
        if binding == "descriptor32" and any(c in (11, 5347) for c in metadata["capabilities"]):
            raise ShaderCatalogError("descriptor artifact has incompatible capabilities")
        # Validate the exact read snapshot, not a path another process could replace.
        # The original conversion already did full container preflight; this pass
        # validates bounded identity ranges and revalidates each module.
        with tempfile.TemporaryDirectory(prefix="catalog-validation-", dir=conversion) as scratch:
            checked = Path(scratch) / "module.spv"
            _write_exclusive(checked, binary)
            result = run_worker([str(validator), "--target-env", "vulkan1.1", "--relax-block-layout", str(checked)],
                cwd=Path(scratch), environment=os.environ.copy())
        if result.status != "exited" or result.returncode != 0:
            raise ShaderCatalogError("shader revalidation failed")
        entry = microcode_identity(container, record["stage"])
        entry.update(index=record["index"], spirv_sha256=hashlib.sha256(binary).hexdigest())
        entries.append(entry)
    return group_entries(entries, binding)


def native_catalog_pack(conversion: Path, catalog: dict[str, object], repository: Path) -> bytes:
    """Exact-byte native lookup pack; caller supplies a freshly validated catalog."""
    entries = catalog["artifacts"]
    prepared = _prepare_inputs([conversion / f"shader-{e['index']:04d}" / name
        for e in entries for name in ("container.xenos", "translated.spv")], repository,
        BatchLimits(max_files=2048, max_file_bytes=16 * 1024 * 1024, max_aggregate_bytes=128 * 1024 * 1024))
    keys = bytearray()
    modules = bytearray(b"XSPVPK1\0" + struct.pack("<II",
        2 if catalog["constant_binding"] == "descriptor32" else 1, len(entries)))
    for i, entry in enumerate(entries):
        container, binary = (_read_private_input(prepared[i * 2 + offset]) for offset in (0, 1))
        if (hashlib.sha256(container).hexdigest() != entry["container_sha256"] or
                hashlib.sha256(binary).hexdigest() != entry["spirv_sha256"]):
            raise ShaderCatalogError("native pack input changed after validation")
        outer = _parse_xenos_candidate(container, 0, FxcLimits())
        identity = microcode_identity(container, entry["stage"])
        if identity["microcode_sha256"] != entry["microcode_sha256"]:
            raise ShaderCatalogError("native pack microcode identity mismatch")
        offset, size = struct.unpack_from(">II", container, outer.shader_offset)
        if size > 65535 * 4:
            raise ShaderCatalogError("native shader lookup extent exceeds the PM4 full-load profile")
        begin = outer.virtual_size + offset
        stage = 0 if entry["stage"] == "vertex" else 1
        keys.extend(struct.pack("<III", entry["index"], stage, size))
        keys.extend(container[begin:begin + size])
        modules.extend(struct.pack("<III", entry["index"], 0 if stage == 0 else 4, len(binary)))
        modules.extend(binary)
        if (len(keys) > 16 * 1024 * 1024 or len(modules) > 64 * 1024 * 1024 or
                16 + len(keys) + len(modules) > 80 * 1024 * 1024):
            raise ShaderCatalogError("native lookup pack budget exceeded")
    return b"XSCAT1\0\0" + struct.pack("<II", len(entries), len(modules)) + keys + modules


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("conversion_directory", type=Path)
    parser.add_argument("output_catalog", type=Path)
    parser.add_argument("--spirv-validator", type=Path, required=True)
    parser.add_argument("--native-pack", action="store_true", help="emit a private binary exact-byte lookup pack instead of JSON")
    args = parser.parse_args(argv)
    repository = repository_root()
    try:
        output = _require_private_report(args.output_catalog, repository)
        catalog = build_catalog(args.conversion_directory, repository=repository,
                                validator=args.spirv_validator)
        payload = native_catalog_pack(args.conversion_directory, catalog, repository) if args.native_pack else (
            json.dumps(catalog, indent=2, sort_keys=True) + "\n").encode()
        _write_exclusive(output, payload)
        print(f"Private microcode catalog: {catalog['artifact_count']} artifacts, "
              f"{catalog['lookup_key_count']} keys, {catalog['ambiguous_key_count']} ambiguous keys.")
        return 0
    except (OSError, ValueError, TypeError, KeyError, ShaderTranslationError, XenosPreflightError):
        print("Private microcode catalog rejected; no catalog published.", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
