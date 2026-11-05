#!/usr/bin/env python3
"""Analyze a private stopped graph and optionally compile its restricted probe pair.

No guest memory, GPU state or completion is changed. Unknown conditional/control
packets reject analysis. Hardware configuration writes remain uninterpreted.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import sys

import stopped_pm4_artifacts as pm4
from register_knowledge import RegisterKnowledge
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'rpf7_inventory'))
from raw_shader_analysis import translate_constant_pair
from xenos_preflight_batch import (BatchLimits, _prepare_inputs, _read_private_input,
    _require_private_report, _require_validator, repository_root)
from xenos_translate_batch import run_worker, spirv_metadata
from rpf7_extract import _write_exclusive


def load_capture(report_path):
    repository = repository_root()
    entry = _prepare_inputs([report_path], repository, BatchLimits(max_files=1, max_file_bytes=1024*1024))[0]
    report = json.loads(_read_private_input(entry))['execution']
    metadata = report['stopped_pm4_capture']
    artifacts = metadata['artifacts']
    if metadata['status'] != 1 or not 1 <= len(artifacts) <= 17:
        raise ValueError('complete bounded PM4 capture required')
    paths = []
    for index, artifact in enumerate(artifacts):
        path = Path(artifact['path'])
        if path.is_absolute() or '..' in path.parts or artifact['node'] != index:
            raise ValueError('invalid private PM4 artifact path or order')
        paths.append(entry.path.parent / path)
    inputs = _prepare_inputs(paths, repository, BatchLimits(max_files=17,
        max_file_bytes=pm4.MAX_CAPTURE_BYTES, max_aggregate_bytes=pm4.MAX_CAPTURE_BYTES))
    records = []
    for index, (source, artifact) in enumerate(zip(inputs, artifacts)):
        data = _read_private_input(source)
        if hashlib.sha256(data).hexdigest() != artifact['sha256']:
            raise ValueError('private PM4 artifact changed after capture')
        records.append(f'{index},' + data.hex())
    graph = report['stopped_gpu_indirect_graph']
    capture = pm4.parse_pm4_capture('1', records, report['stopped_gpu_submission'], graph)
    return capture, graph


def analyze_packets(packets):
    registers, shaders, draws, events, rmw, writes = {}, {}, [], [], [], []
    pair = None
    def value(index):
        if index not in registers: raise ValueError('draw depends on an unset required register')
        return registers[index].require()
    for node, packet, payload in packets:
        offset, kind, count, opcode, first, second, predicated, repeat = packet
        if predicated: raise ValueError('predicated command analysis unsupported')
        if kind in (0, 1):
            for i, data in enumerate(payload):
                index = first + (0 if repeat else i) if kind == 0 else (first if i == 0 else second)
                registers[index] = RegisterKnowledge.assigned(data)
                writes.append([node, offset, index, data])
            continue
        if kind == 2: continue
        if kind != 3: raise ValueError('unsupported packet kind')
        if opcode == 0x10: continue
        if opcode == 0x21:
            if count != 3 or payload[0] & ~0x1FFF:
                raise ValueError('only immediate REG_RMW layout supported')
            index, and_mask, or_mask = payload
            rmw.append([node, offset, index, and_mask, or_mask])
            # Preserve exactly known bits, without substituting a reset value.
            registers[index] = registers.get(index, RegisterKnowledge()).read_modify_write(and_mask, or_mask)
            continue
        if opcode == 0x3B:
            if count != 1: raise ValueError('invalid INVALIDATE_STATE framing')
            continue  # Metadata analysis only: no invalidation is performed.
        if opcode == 0x2B:
            if count < 2 or payload[0] > 1 or payload[1] != count - 2 or payload[1] % 3:
                raise ValueError('unsupported immediate shader load')
            shaders[payload[0]] = struct.pack('>' + 'I' * (count - 2), *payload[2:])
            continue
        if opcode == 0x36:
            if count != 1 or payload[0] != 0x10081 or set(shaders) != {0, 1}:
                raise ValueError('only this bounded one-point draw profile is analyzed')
            if len(draws) == 64: raise ValueError('draw analysis budget exceeded')
            configured = {str(index): value(index) for index in (0x2180, 0x2181, 0x2104, 0x2200, 0x2208, 0x2293, 0x2280)}
            translated = translate_constant_pair(shaders[0], shaders[1], value(0x2180), value(0x2181))
            if pair is not None and translated != pair:
                raise ValueError('multiple different shader pairs unsupported by probe')
            pair = translated
            draws.append({'node': node, 'offset': offset, 'registers': configured,
                'color_writes_disabled': value(0x2104) == 0,
                'depth_stencil_disabled': value(0x2200) == 0,
                'visibility_query_disabled': value(0x2293) == 0,
                'color_depth_mode': value(0x2208) == 4,
                'one_pixel_point': value(0x2280) == 0x80008})
            continue
        if opcode == 0x58:
            if count != 3 or payload[0] != 3 or payload[1] & 3 != 2:
                raise ValueError('only literal PS_DONE_TS big-endian event metadata supported')
            address = payload[1] & ~3
            if address + 4 > 0x20000000: raise ValueError('noncanonical event destination')
            events.append({'node': node, 'offset': offset, 'physical_address': address,
                'value': payload[2], 'bytes': 4, 'byte_order': 'big-endian',
                'prior_point_requests': len(draws), 'memory_ownership_validated': False,
                'written': False})
            continue
        raise ValueError('uninterpreted command may alter subsequent control/state')
    if pair is None: raise ValueError('no analyzable shader pair/draw')
    return {'classification': 'implementation-work', 'draws': draws, 'events': events,
        'hardware_register_writes': writes, 'read_modify_write_requests': rmw,
        'incomplete_register_knowledge': {str(index): state.metadata() for index, state in sorted(registers.items())
            if not state.metadata()['fully_known']},
        'hardware_register_side_effects_implemented': False, 'guest_commands_executed': False,
        'shader_pair': pair[2]}, pair[:2]


def run(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace_report', type=Path)
    parser.add_argument('output_directory', type=Path)
    parser.add_argument('--glslc', type=Path)
    parser.add_argument('--spirv-validator', type=Path)
    args = parser.parse_args(argv)
    if bool(args.glslc) != bool(args.spirv_validator): raise ValueError('both compiler and validator are required')
    output = _require_private_report(args.output_directory, repository_root())
    compiler = _require_validator(args.glslc) if args.glslc else None
    validator = _require_validator(args.spirv_validator) if args.spirv_validator else None
    capture, graph = load_capture(args.trace_report)
    analysis, sources = analyze_packets(pm4.ordered_packets(capture, graph))
    output.mkdir(mode=0o700)
    analysis['compiled_modules'] = []
    for stage, suffix, source in zip(('vertex', 'pixel'), ('vert', 'frag'), sources):
        path = output / ('translated.' + suffix)
        _write_exclusive(path, source.encode())
        if compiler:
            binary = output / ('translated.' + suffix + '.spv')
            for command in ([str(compiler), '--target-env=vulkan1.1', str(path), '-o', str(binary)],
                            [str(validator), '--target-env', 'vulkan1.1', str(binary)]):
                result = run_worker(command, cwd=output, environment=os.environ.copy())
                if result.status != 'exited' or result.returncode != 0:
                    raise ValueError('bounded shader compile/validation failed; private output suppressed')
            data = _read_private_input(_prepare_inputs([binary], repository_root(), BatchLimits(max_files=1, max_file_bytes=1024*1024))[0])
            analysis['compiled_modules'].append({'stage': stage, 'path': binary.name,
                'sha256': hashlib.sha256(data).hexdigest(), 'spirv': spirv_metadata(data, stage)})
    _write_exclusive(output / 'analysis.json', (json.dumps(analysis, indent=2) + '\n').encode())
    print(f"Stopped analysis: {len(analysis['draws'])} point requests, {len(analysis['events'])} unwritten events; "
          f"{len(analysis['compiled_modules'])} validated probe modules. No guest submission executed. Private output: {output}")


if __name__ == '__main__':
    try: run()
    except (ValueError, KeyError, TypeError, OSError) as error:
        print(f'error: {error}', file=sys.stderr)
        raise SystemExit(2)
