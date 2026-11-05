#!/usr/bin/env python3
"""Create a private, bounded COMPUTE probe from one stopped vertex shader/buffer.

Explicit index input and slot0 binding are experimental preconditions, not PM4
execution. No vertex pipeline, predicate, viewport, pixel, EDRAM or frame proof.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'rpf7_inventory'))
from raw_shader_analysis import evaluate_float_vertex_copy, translate_float_vertex_copy_compute
from rpf7_extract import _write_exclusive
from xenos_preflight_batch import (BatchLimits, _prepare_inputs, _read_private_input,
    _require_private_report, _require_validator, repository_root)
from xenos_translate_batch import run_worker


def expected_bytes(code, source):
    """CPU oracle; all three vertices must validate before any output is returned."""
    output = bytearray([0x5A] * 128)
    translate_float_vertex_copy_compute(code)  # Enforce the same bounded profile.
    for index in range(3):
        exports = evaluate_float_vertex_copy(code, {0: source}, index)
        for slot, offset in (('62', 16), ('0', 32)):
            if slot in exports:
                struct.pack_into('<4f', output, offset + index * 32, *exports[slot])
    return bytes(output)


def read_artifact(parent, artifact, expected_size):
    relative = Path(artifact['path'])
    if relative.is_absolute() or '..' in relative.parts or not 0 < expected_size <= 4096:
        raise ValueError('invalid bounded artifact path/extent')
    entry = _prepare_inputs([parent / relative], repository_root(),
        BatchLimits(max_files=1, max_file_bytes=4096))[0]
    data = _read_private_input(entry)
    if len(data) != expected_size or hashlib.sha256(data).hexdigest() != artifact['sha256']:
        raise ValueError('artifact extent/hash differs from stopped report')
    return data


def load_inputs(report_path, shader_index, range_index):
    if any(type(i) is not int or not 0 <= i < 8 for i in (shader_index, range_index)):
        raise ValueError('bounded artifact index required')
    entry = _prepare_inputs([report_path], repository_root(),
        BatchLimits(max_files=1, max_file_bytes=1024*1024))[0]
    encoded = _read_private_input(entry)
    execution = json.loads(encoded)['execution']
    shaders, ranges = execution['stopped_shader_capture'], execution['stopped_physical_capture']
    if (shaders['status'] != 1 or ranges['status'] != 1 or
            not 1 <= len(shaders['artifacts']) <= 8 or not 1 <= len(ranges['artifacts']) <= 8):
        raise ValueError('complete bounded stopped capture required')
    shader, source = shaders['artifacts'][shader_index], ranges['artifacts'][range_index]
    if shader['stage'] != 0:
        raise ValueError('vertex-stage artifact required')
    code = read_artifact(entry.path.parent, shader, shader['instruction_words'] * 4)
    data = read_artifact(entry.path.parent, source, source['byte_count'])
    return code, data, {'report_sha256': hashlib.sha256(encoded).hexdigest(),
        'shader': shader, 'buffer': source, 'same_report': True,
        'gpu_execution_time_contents_verified': False, 'binding_inferred_from_pm4': False}


def compile_compute(path, output, compiler, validator):
    """Publish a module only after bounded compilation and SPIR-V validation."""
    pending = output / 'dataflow.spv.unvalidated'
    for command in ([str(compiler), '--target-env=vulkan1.1', str(path), '-o', str(pending)],
                    [str(validator), '--target-env', 'vulkan1.1', str(pending)]):
        result = run_worker(command, cwd=output, environment=os.environ.copy())
        if pending.exists(): pending.chmod(0o600)
        if result.status != 'exited' or result.returncode != 0:
            raise ValueError('bounded shader compile/validation failed; private output suppressed')
    binary = _read_private_input(_prepare_inputs([pending], repository_root(),
        BatchLimits(max_files=1, max_file_bytes=65536))[0])
    pending.rename(output / 'dataflow.spv')
    return binary


def run(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('output_directory', type=Path)
    parser.add_argument('--shader-index', type=int, required=True)
    parser.add_argument('--range-index', type=int, required=True)
    parser.add_argument('--glslc', type=Path, required=True)
    parser.add_argument('--spirv-validator', type=Path, required=True)
    args = parser.parse_args(argv)
    output = _require_private_report(args.output_directory, repository_root())
    compiler, validator = _require_validator(args.glslc), _require_validator(args.spirv_validator)
    code, data, provenance = load_inputs(args.report, args.shader_index, args.range_index)
    source, proof = translate_float_vertex_copy_compute(code)
    expected = expected_bytes(code, data)
    output.mkdir(mode=0o700)  # Fresh directory, never overwrite a tested checkpoint.
    _write_exclusive(output / 'input.bin', data)
    _write_exclusive(output / 'expected.bin', expected)
    path = output / 'dataflow.comp'
    _write_exclusive(path, source.encode())
    binary = compile_compute(path, output, compiler, validator)
    report = dict(proof, provenance=provenance, spirv_sha256=hashlib.sha256(binary).hexdigest(),
        expected_sha256=hashlib.sha256(expected).hexdigest(), spirv_validated=True,
        cpu_reference_vertices=3, gpu_executed=False)
    _write_exclusive(output / 'proof.json', (json.dumps(report, indent=2) + '\n').encode())
    print(f'Validated derived compute module; 3 CPU reference vertices; GPU not run. Private output: {output}')


if __name__ == '__main__':
    try: run()
    except (ValueError, KeyError, IndexError, TypeError, OSError) as error:
        print(f'error: {error}', file=sys.stderr)
        raise SystemExit(2)
