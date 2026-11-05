#!/usr/bin/env python3
"""Translate explicitly selected private Xenos containers, without running a GPU."""

from __future__ import annotations

import argparse
import hashlib
from dataclasses import dataclass
import json
import os
from pathlib import Path
import platform
try:
    import resource
except ImportError:       # Windows: no resource limits; run_worker bounds wall time and output there (see below)
    resource = None
import selectors
import signal
import stat
import struct
import subprocess
import sys
import threading
import time
from typing import BinaryIO, Sequence

from fxc_xenos_inspect import (
    FXC_MAGIC, FxcInspectError, FxcLimits, _parse_xenos_candidate, inspect_fxc_bytes,
)
from rpf7_extract import _write_exclusive
from xenos_preflight_batch import (
    BatchLimits, XenosPreflightError, _parse_protocol_summary, _prepare_inputs,
    _read_private_input, _require_private_report, _require_validator,
    default_validator, repository_root,
)


MAX_OUTPUT_BYTES = 16 * 1024 * 1024
MAX_LOG_BYTES = 64 * 1024
MAX_TOTAL_OUTPUT_BYTES = 512 * 1024 * 1024


class ShaderTranslationError(ValueError):
    """A conversion could not be performed inside its declared contract."""


@dataclass(frozen=True)
class Toolchain:
    translator: Path
    preflight: Path
    common_header: Path
    dxc: Path
    spirv_validator: Path
    compiler_library_directory: Path | None = None


@dataclass(frozen=True)
class WorkerResult:
    status: str
    returncode: int | None
    output: bytes


def _worker_limits() -> None:
    # Used only by this single-threaded POSIX CLI before exec. These are resource
    # limits, NOT a security sandbox. No inherited core dump or permissive umask.
    os.umask(0o077)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    resource.setrlimit(resource.RLIMIT_CPU, (10, 11))
    resource.setrlimit(resource.RLIMIT_FSIZE, (MAX_OUTPUT_BYTES, MAX_OUTPUT_BYTES))
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    resource.setrlimit(resource.RLIMIT_NOFILE, (min(64, soft), hard))


ZOMBIE_GROUP_WAIT_SECONDS = 2.0   # how long a group of exited, not yet reaped descendants may keep answering EPERM


def _terminate_worker_group(process: subprocess.Popen) -> None:
    """Kill descendants too; distinguish a reaped leader from denied cleanup."""
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    except PermissionError:
        # Darwin can report EPERM for a group whose only members are zombies.
        # Reap only our child, then check cleanup again. A descendant that has
        # exited but is not yet reaped by its new parent keeps the group for a
        # moment longer (seen 2026-10-08 on a loaded machine: two batches of a
        # run failed here), so the check is repeated for a bounded time. A live
        # leader or a group still denied after that remains an error, never a
        # successful cancellation.
        if process.poll() is None:
            raise
        deadline = time.monotonic() + ZOMBIE_GROUP_WAIT_SECONDS
        while True:
            try:
                os.killpg(process.pid, signal.SIGKILL)
                return
            except ProcessLookupError:
                return
            except PermissionError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.02)


def run_worker_without_groups(
    command: Sequence[str], *, cwd: Path, environment: dict[str, str],
    input_file: BinaryIO | None = None, wall_seconds: float = 30,
) -> WorkerResult:
    """The same bounds on a host without process groups, resource limits or selectable pipes (Windows).

    Wall time and pipe output are bounded here and the worker itself is ended; the translators, the compiler and the
    validator start no children. CPU time and written file size have no limit of their own on such a host: the wall
    limit stands for the first, and every output file is still refused over MAX_OUTPUT_BYTES when it is read back."""
    if not 0 < wall_seconds <= 60:
        raise ShaderTranslationError("worker wall limit is outside the bounded range")
    process = subprocess.Popen(
        list(command), cwd=cwd, env=environment,
        stdin=input_file if input_file is not None else subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    assert process.stdout is not None
    output = bytearray()
    over = threading.Event()

    def read() -> None:
        while True:
            chunk = process.stdout.read1(8192)
            if not chunk:
                return
            output.extend(chunk[:MAX_LOG_BYTES + 1 - len(output)])
            if len(output) > MAX_LOG_BYTES:
                over.set()
                process.kill()
                return

    reader = threading.Thread(target=read, daemon=True)
    reader.start()
    status = "exited"
    try:
        process.wait(timeout=wall_seconds)
    except subprocess.TimeoutExpired:
        status = "timeout"
    finally:
        if process.poll() is None:
            process.kill()
        process.wait()
        reader.join(timeout=5)
        process.stdout.close()
    if over.is_set():
        status = "output-limit"
    elif status == "exited" and process.returncode < 0:
        status = "signal"
    return WorkerResult(status, process.returncode, bytes(output[:MAX_LOG_BYTES]))


def run_worker(
    command: Sequence[str], *, cwd: Path, environment: dict[str, str],
    input_file: BinaryIO | None = None, wall_seconds: float = 30,
) -> WorkerResult:
    """Bound child CPU/file output, all pipe output, and the entire process group."""
    if os.name == "nt":
        return run_worker_without_groups(command, cwd=cwd, environment=environment,
                                         input_file=input_file, wall_seconds=wall_seconds)
    if not 0 < wall_seconds <= 60:
        raise ShaderTranslationError("worker wall limit is outside the bounded range")
    process = subprocess.Popen(
        list(command), cwd=cwd, env=environment,
        stdin=input_file if input_file is not None else subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        start_new_session=True, preexec_fn=_worker_limits,
    )
    assert process.stdout is not None
    output = bytearray()
    status = "exited"
    deadline = time.monotonic() + wall_seconds
    try:
        with selectors.DefaultSelector() as selector:
            os.set_blocking(process.stdout.fileno(), False)
            selector.register(process.stdout, selectors.EVENT_READ)
            eof = False
            while not eof:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    status = "timeout"
                    break
                for key, _ in selector.select(min(remaining, 0.1)):
                    chunk = os.read(key.fd, min(8192, MAX_LOG_BYTES + 1 - len(output)))
                    if not chunk:
                        eof = True
                        break
                    output.extend(chunk)
                    if len(output) > MAX_LOG_BYTES:
                        status = "output-limit"
                        break
                if status != "exited":
                    break
            if status == "exited":
                try:
                    process.wait(timeout=max(0.001, deadline - time.monotonic()))
                except subprocess.TimeoutExpired:
                    status = "timeout"
    finally:
        # Also close any descendants retaining stdout after their leader exits.
        try:
            _terminate_worker_group(process)
            process.wait()
        finally:
            process.stdout.close()
    if status == "exited" and process.returncode < 0:
        status = "signal"
    return WorkerResult(status, process.returncode, bytes(output[:MAX_LOG_BYTES]))


def _artifact(path: Path, *, max_bytes: int = MAX_OUTPUT_BYTES,
              normalize_permissions: bool = True) -> bytes:
    metadata = os.lstat(path)
    if not stat.S_ISREG(metadata.st_mode) or metadata.st_size > max_bytes:
        raise ShaderTranslationError("worker output is not a bounded regular file")
    # (flags a host lacks are left out: Windows has no O_NOFOLLOW, the lstat above already refused a link; it needs O_BINARY)
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0))
    try:
        opened = os.fstat(fd)
        if (opened.st_dev, opened.st_ino, opened.st_size) != (
            metadata.st_dev, metadata.st_ino, metadata.st_size,
        ):
            raise ShaderTranslationError("worker output changed before reading")
        if normalize_permissions and hasattr(os, "fchmod"):
            os.fchmod(fd, 0o600)
        data = bytearray()
        while len(data) < opened.st_size:
            chunk = os.read(fd, min(1024 * 1024, opened.st_size - len(data)))
            if not chunk:
                raise ShaderTranslationError("worker output ended during bounded read")
            data.extend(chunk)
        final = os.fstat(fd)
        if (final.st_size, final.st_mtime_ns) != (opened.st_size, opened.st_mtime_ns):
            raise ShaderTranslationError("worker output changed while reading")
    finally:
        os.close(fd)
    return bytes(data)


def spirv_metadata(data: bytes, expected_stage: str) -> dict[str, object]:
    """Inspect a validated module's framing and stage; not a replacement for spirv-val."""
    if len(data) < 20 or len(data) % 4 or len(data) > MAX_OUTPUT_BYTES:
        raise ShaderTranslationError("invalid SPIR-V extent")
    words = struct.unpack(f"<{len(data) // 4}I", data)
    if (words[0] != 0x07230203 or words[1] not in (0x10000, 0x10100, 0x10200, 0x10300)
            or not 0 < words[3] <= 1_000_000 or words[4] != 0):
        raise ShaderTranslationError("invalid or unsupported SPIR-V header")
    capabilities: set[int] = set()
    entry_models: list[int] = []
    extensions: list[str] = []
    cursor = 5
    while cursor < len(words):
        count, opcode = words[cursor] >> 16, words[cursor] & 0xffff
        if count < 1 or count > len(words) - cursor:
            raise ShaderTranslationError("invalid SPIR-V instruction framing")
        if opcode == 17:  # OpCapability
            if count != 2:
                raise ShaderTranslationError("invalid capability instruction")
            capabilities.add(words[cursor + 1])
        elif opcode == 15:  # OpEntryPoint
            if count < 4:
                raise ShaderTranslationError("invalid entry point instruction")
            entry_models.append(words[cursor + 1])
        elif opcode == 10:  # OpExtension (standard extension identifiers only)
            encoded = data[(cursor + 1) * 4 : (cursor + count) * 4]
            if b"\0" not in encoded or len(encoded) > 256:
                raise ShaderTranslationError("invalid extension identifier")
            name = encoded.split(b"\0", 1)[0]
            if not name.startswith(b"SPV_") or any(
                not (65 <= c <= 90 or 97 <= c <= 122 or 48 <= c <= 57 or c == 95)
                for c in name
            ):
                raise ShaderTranslationError("invalid extension identifier")
            extensions.append(name.decode("ascii"))
        cursor += count
    if expected_stage not in ("vertex", "pixel") or entry_models != [
        0 if expected_stage == "vertex" else 4
    ]:
        raise ShaderTranslationError("SPIR-V entry stage differs from its container")
    return {
        "byte_count": len(data), "version_word": words[1],
        "capabilities": sorted(capabilities), "extensions": sorted(set(extensions)),
    }


def _staged_containers(data: bytes, remaining: int) -> list[tuple[str, bytes]]:
    limits = FxcLimits(max_containers=remaining)
    if data.startswith(FXC_MAGIC):
        inspection = inspect_fxc_bytes(data, limits=limits)
        if inspection.rejected_magic_candidates or not inspection.containers:
            raise ShaderTranslationError("FXC staging was incomplete or ambiguous")
        return [(entry.stage, data[entry.offset:entry.offset + entry.size])
                for entry in inspection.containers]
    candidate = _parse_xenos_candidate(data, 0, limits)
    if candidate is None or candidate.size != len(data):
        raise ShaderTranslationError("input is not one complete bounded container")
    return [(candidate.stage, data)]


def module_pack(records: Sequence[dict[str, object]], directory: Path,
                constant_binding: str, identities: Sequence[dict[str, object]]) -> bytes:
    """Private transport of validated modules; no shader names or host paths."""
    valid = [r for r in records if r["status"] == "validated"]
    if not valid or len(valid) > 1024:
        raise ShaderTranslationError("module pack count is outside its contract")
    output = bytearray(b"XSPVPK1\0" + struct.pack("<II",
        1 if constant_binding == "device-address" else 2, len(valid)))
    expected = {int(item["index"]): item["spirv_sha256"] for item in identities}
    for record in valid:
        index, stage = int(record["index"]), str(record["stage"])
        data = _artifact(directory / f"shader-{index:04d}" / "translated.spv")
        if hashlib.sha256(data).hexdigest() != expected.get(index):
            raise ShaderTranslationError("module changed after validation")
        spirv_metadata(data, stage)
        if len(output) + len(data) + 12 > 64 * 1024 * 1024:
            raise ShaderTranslationError("module pack byte budget exceeded")
        output.extend(struct.pack("<III", index, 0 if stage == "vertex" else 4, len(data)))
        output.extend(data)
    return bytes(output)


def _name_library_directory(environment: dict[str, str], library: str) -> None:
    """Where the started programs find the compiler library: the loader's variable on macOS and Linux; Windows has
    none, there the folder goes in front of PATH."""
    if os.name == "nt":
        environment["PATH"] = library + ";" + environment["PATH"] if environment.get("PATH") else library
    else:
        environment["DYLD_LIBRARY_PATH" if platform.system() == "Darwin" else "LD_LIBRARY_PATH"] = library


def translate_batch(
    inputs: Sequence[Path], output_directory: Path, *, toolchain: Toolchain,
    repository: Path, max_containers: int = 256,
    constant_binding: str = "device-address",
) -> dict[str, object]:
    if not 1 <= max_containers <= 1024:
        raise ShaderTranslationError("conversion requires 1..1024 containers")
    if constant_binding not in ("device-address", "descriptor32"):
        raise ShaderTranslationError("unknown shader constant binding ABI")
    prepared = _prepare_inputs(inputs, repository, BatchLimits(
        max_files=64, max_aggregate_bytes=64 * 1024 * 1024,
        max_containers=max_containers,
    ))
    executable_paths = [toolchain.translator, toolchain.preflight, toolchain.dxc,
                        toolchain.spirv_validator]
    translator, preflight, dxc, validator = map(_require_validator, executable_paths)
    common_header = toolchain.common_header.resolve(strict=True)
    common_data = _artifact(common_header, max_bytes=1024 * 1024,
                            normalize_permissions=False)
    environment = os.environ.copy()
    if toolchain.compiler_library_directory is not None:
        library = toolchain.compiler_library_directory.resolve(strict=True)
        if not library.is_dir():
            raise ShaderTranslationError("compiler library directory is unavailable")
        _name_library_directory(environment, str(library))
    output_directory = _require_private_report(output_directory, repository)
    # Validate the whole bounded selection before creating any output.
    containers: list[tuple[str, bytes]] = []
    for entry in prepared:
        remaining = max_containers - len(containers)
        if remaining < 1:
            raise ShaderTranslationError("batch container limit exceeded")
        containers.extend(_staged_containers(_read_private_input(entry), remaining))
    output_directory.mkdir(mode=0o700)
    _write_exclusive(output_directory / "shader-common.h", common_data)
    records: list[dict[str, object]] = []
    identities: list[dict[str, object]] = []
    total_bytes = len(common_data)
    for index, (stage, container) in enumerate(containers, 1):
        if (total_bytes + 2 * len(container) + 2 * MAX_OUTPUT_BYTES +
                4 * MAX_LOG_BYTES + 1024 * 1024 > MAX_TOTAL_OUTPUT_BYTES):
            raise ShaderTranslationError("batch output budget exhausted; partial outputs preserved")
        directory = output_directory / f"shader-{index:04d}"
        directory.mkdir(mode=0o700)
        source = directory / "container.xenos"
        hlsl = directory / "translated.hlsl"
        spirv = directory / "translated.spv"
        protocol = directory / "preflight-input.bin"
        _write_exclusive(source, container)
        _write_exclusive(protocol, struct.pack(">I", len(container)) + container + bytes(4))
        record: dict[str, object] = {"index": index, "stage": stage, "status": "preflight-failed"}
        records.append(record)
        with protocol.open("rb") as stream:
            result = run_worker([str(preflight)], cwd=directory,
                                environment=environment, input_file=stream)
        _write_exclusive(directory / "preflight.log", result.output)
        if result.status == "exited" and result.returncode == 0:
            try:
                valid = _parse_protocol_summary(result.output) == (1, 1, 0)
            except XenosPreflightError:
                valid = False
        else:
            valid = False
        if valid:
            command = [str(translator), str(source), str(hlsl),
                       str(output_directory / "shader-common.h")]
            result = run_worker(command, cwd=directory, environment=environment)
            _write_exclusive(directory / "translation.log", result.output)
            record["status"] = "translation-failed"
            if result.status == "exited" and result.returncode == 0:
                try:
                    if not _artifact(hlsl):
                        raise ShaderTranslationError("translator produced empty source")
                    record["status"] = "compilation-failed"
                    command = [str(dxc), "-T", "ps_6_0" if stage == "pixel" else "vs_6_0",
                               "-E", "shaderMain", "-HV", "2021", "-all-resources-bound",
                               "-spirv", "-fspv-target-env=vulkan1.1", "-fvk-use-dx-layout",
                               "-Qstrip_debug", "-Fo", str(spirv), str(hlsl)]
                    if stage == "vertex":
                        command.append("-fvk-invert-y")
                    if constant_binding == "descriptor32":
                        command.extend(["-D", "XENOS_RECOMP_DESCRIPTOR_CONSTANTS=1"])
                    result = run_worker(command, cwd=directory, environment=environment)
                    _write_exclusive(directory / "compilation.log", result.output)
                    if result.status == "exited" and result.returncode == 0:
                        spirv_bytes = _artifact(spirv)
                        metadata = spirv_metadata(spirv_bytes, stage)
                        if constant_binding == "descriptor32" and (
                            11 in metadata["capabilities"] or 5347 in metadata["capabilities"]
                        ):
                            raise ShaderTranslationError("descriptor32 still requires 64-bit addresses")
                        record["status"] = "validation-failed"
                        result = run_worker(
                            [str(validator), "--target-env", "vulkan1.1",
                             "--relax-block-layout", str(spirv)],
                            cwd=directory, environment=environment,
                        )
                        _write_exclusive(directory / "validation.log", result.output)
                        if result.status == "exited" and result.returncode == 0:
                            record["status"] = "validated"
                            record["spirv"] = metadata
                            identities.append({
                                "index": index,
                                "container_sha256": hashlib.sha256(container).hexdigest(),
                                "spirv_sha256": hashlib.sha256(spirv_bytes).hexdigest(),
                            })
                except ShaderTranslationError as error:
                    record["status"] = "invalid-output"
                    record["failure_reason"] = str(error)  # Fixed messages, no source/compiler text.
                except OSError:
                    record["status"] = "invalid-output"
                    record["failure_reason"] = "worker artifact unavailable"
        record["last_worker_status"] = result.status
        record["last_worker_returncode"] = result.returncode
        # Only files in the newly created, owned per-container directory.
        for artifact in directory.iterdir():
            metadata = artifact.lstat()
            if not stat.S_ISREG(metadata.st_mode) or metadata.st_size > MAX_OUTPUT_BYTES:
                raise ShaderTranslationError("unexpected worker output; partial outputs preserved")
            total_bytes += metadata.st_size
            artifact.chmod(0o600)
        report = {
            "schema_version": 1, "status": "incomplete",
            "constant_binding": constant_binding,
            "input_file_count": len(prepared), "container_count": len(containers),
            "completed_container_count": len(records),
            "validated_container_count": sum(r["status"] == "validated" for r in records),
            "records": records, "gpu_execution": False,
            "semantic_equivalence_verified": False, "artifacts_private": True,
            "source_filenames_emitted": False, "payload_in_report": False,
        }
        # Per-step checkpoints are exclusive and never overwrite prior work.
        checkpoint = (json.dumps(report, indent=2, sort_keys=True) + "\n").encode()
        total_bytes += len(checkpoint)
        _write_exclusive(directory / "checkpoint.json", checkpoint)
    report["status"] = "validated" if all(r["status"] == "validated" for r in records) else "partial"
    if report["validated_container_count"]:
        _write_exclusive(output_directory / "validated-modules.bin",
                         module_pack(records, output_directory, constant_binding, identities))
    _write_exclusive(output_directory / "artifact-identities.json", (json.dumps({
        "schema_version": 1, "constant_binding": constant_binding, "artifacts": identities,
    }, indent=2, sort_keys=True) + "\n").encode())
    _write_exclusive(output_directory / "report.json",
                     (json.dumps(report, indent=2, sort_keys=True) + "\n").encode())
    return report


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output_directory", type=Path)
    parser.add_argument("inputs", type=Path, nargs="+")
    parser.add_argument("--spirv-validator", type=Path, required=True)
    parser.add_argument("--translator", type=Path)
    parser.add_argument("--preflight", type=Path)
    parser.add_argument("--dxc", type=Path)
    parser.add_argument("--compiler-library-directory", type=Path)
    parser.add_argument("--common-header", type=Path)
    parser.add_argument("--max-containers", type=int, default=256)
    parser.add_argument("--constant-binding", choices=("device-address", "descriptor32"),
                        default="device-address")
    args = parser.parse_args(argv)
    repository = repository_root()
    xenos = repository / "tools/XenosRecomp"
    arch = "arm64" if platform.machine().lower() in ("arm64", "aarch64") else "x64"
    dxc_root = xenos / "thirdparty/dxc-bin"
    try:
        report = translate_batch(args.inputs, args.output_directory,
            repository=repository, max_containers=args.max_containers,
            constant_binding=args.constant_binding,
            toolchain=Toolchain(
                args.translator or repository / "out/host-tools/xenos/XenosRecomp/XenosRecomp",
                args.preflight or default_validator(repository),
                args.common_header or xenos / "XenosRecomp/shader_common.h",
                args.dxc or dxc_root / "bin" / arch / (
                    "dxc-macos" if platform.system() == "Darwin" else "dxc-linux"),
                args.spirv_validator,
                args.compiler_library_directory or dxc_root / "lib" / arch,
            ))
        print(f"Private shader conversion: {report['validated_container_count']}/"
              f"{report['container_count']} SPIR-V modules validated; no GPU execution.")
        return 0 if report["status"] == "validated" else 1
    except (OSError, ShaderTranslationError, XenosPreflightError, FxcInspectError):
        print("Shader conversion stopped safely; existing private outputs preserved.", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
