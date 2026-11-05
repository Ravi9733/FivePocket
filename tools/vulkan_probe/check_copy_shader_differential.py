#!/usr/bin/env python3
"""Compare the native bounded shader proof to the independent offline analyzer.

Synthetic inputs travel only through a child pipe. Optional actual captured
shader programs are read from the existing validated private PM4 capture; no
new payload file is created, printed, uploaded, or executed on a GPU.
"""
import argparse
import json
from pathlib import Path
import random
import struct
import subprocess
import sys

sys.path[:0] = [str(Path(__file__).resolve().parents[1] / 'rpf7_inventory/tests'),
               str(Path(__file__).resolve().parents[1] / 'host_guest_trace')]
from test_raw_shader_analysis import program, cf_alloc, cf_exec, alu
from raw_shader_analysis import analyze_copy_shader, RawShaderError
from analyze_stopped_pm4 import load_capture, pm4


def symbolic(value):
    if value in ('unwritten', '0', '1'): return {'unwritten': 0, '0': 1, '1': 2}[value]
    register, lane = value.removeprefix('initial_r').split('.')
    return 3 + int(register)*4 + 'xyzw'.index(lane)


def compare(probe, data, stage):
    try:
        expected = analyze_copy_shader(data, stage)
    except RawShaderError:
        expected = None
    result = subprocess.run([str(probe), stage, '--stdin'], input=data,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=3)
    if result.returncode not in (0, 1): raise ValueError('native probe did not complete normally')
    observed = json.loads(result.stdout)
    if bool(expected) != observed['accepted']: raise ValueError('native/offline acceptance mismatch')
    if expected is None: return False
    for key, value in {
        'control_count': expected['control_count'],
        'executed': expected['executed_instruction_addresses'],
        'exports': {k: [symbolic(c) for c in v] for k, v in expected['exports'].items()},
        'initial_components': sorted(symbolic(c)-3 for c in expected['initial_components_read']),
        'gpu_executed': False,
    }.items():
        if observed[key] != value: raise ValueError('native/offline semantic mismatch: ' + key)
    return True


def run():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('native_probe', type=Path)
    parser.add_argument('--private-trace-report', type=Path)
    args = parser.parse_args()
    probe = args.native_probe.resolve(strict=True)
    random_source = random.Random(0xC0DEC0DE)
    cases = []
    for swizzle in range(256):
        register = random_source.randrange(64)
        data = program([cf_alloc(), cf_exec(count=2)],
                       [alu(dest=register, export=False, source=register, swizzle=swizzle), alu(source=register)])
        cases.append((data, 'pixel'))
    for mask in range(16):
        cases.append((program([cf_alloc(), cf_exec()], [alu(vector=mask, scalar=mask, zero=True)]), 'pixel'))
    base = program([cf_alloc(), cf_exec()], [alu()])
    for bit in range(len(base)*8):
        changed = bytearray(base); changed[bit//8] ^= 1 << (bit % 8)
        cases.append((changed, 'pixel'))
    for _ in range(256):
        cases.append((random_source.randbytes(random_source.randrange(1, 341)*12),
                      random_source.choice(('vertex', 'pixel'))))
    accepted = sum(compare(probe, data, stage) for data, stage in cases)
    actual = 0
    if args.private_trace_report:
        capture, graph = load_capture(args.private_trace_report)
        for _, packet, payload in pm4.ordered_packets(capture, graph):
            if packet[1] == 3 and packet[3] == 0x2B:
                stage, count = payload[:2]
                if stage not in (0, 1) or count != len(payload)-2: raise ValueError('invalid captured shader load')
                data = struct.pack('>' + 'I'*count, *payload[2:])
                if not compare(probe, data, ('vertex', 'pixel')[stage]): raise ValueError('captured shader unsupported')
                actual += 1
        if not actual: raise ValueError('no actual captured immediate shaders compared')
    print(f'Native/offline agreement: {len(cases)} synthetic ({accepted} accepted), {actual} private captured programs. No GPU execution.')


if __name__ == '__main__':
    run()
