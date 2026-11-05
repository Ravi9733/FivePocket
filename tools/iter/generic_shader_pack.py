#!/usr/bin/env python3
"""Pack the offline XenosRecomp translations for the generic renderer (attempt v16+).

Reads every validated `shader-translation-v3-*/shader-*/` record (container.xenos +
translated.spv) and writes one private little-endian file the runtime indexes by the
FNV-1a 64 hash of the shader's microcode as it sits in guest memory (big-endian words)
with the bind-time-patchable vertex fetch fields zeroed (canonical_microcode). Vertex shaders also carry their vertex elements: the vfetch
instruction index each (usage, usage index) is read by, and the SPIR-V input location
XenosRecomp gave it, so the runtime can build the vertex input layout from the
bound (patched) microcode. Output stays under local-analysis/ (game-derived data).

Layout (all u32 little-endian unless noted):
  magic 'GSP2', version 2, entry count, reserved
  entries[count]: hash u64, stage (0 vertex, 1 pixel), spirv offset (bytes from file
                  start), spirv words, element count, elements[24] (address | usage<<12 |
                  usage index<<16 | location<<24), pixel outputs, sampler register mask,
                  u8 destination register of each element's vfetch (to follow D3D's reordering)
  then the SPIR-V blobs, 4-byte aligned.
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'own_disc'))
import host_support

ROOT = host_support.package_root(__file__)
MAX_ELEMENTS = 24
# XenosRecomp USAGE_LOCATIONS with patch 0009 (the full GTA V corpus).
LOCATIONS = {
    (0, 0): 0, (0, 1): 1, (0, 2): 2, (0, 3): 3,          # POSITION
    (3, 0): 4, (3, 1): 5, (3, 2): 6, (3, 3): 7,          # NORMAL
    (6, 0): 8, (6, 1): 9, (6, 2): 10, (6, 3): 11,        # TANGENT
    (7, 0): 12,                                          # BINORMAL
    (5, 0): 13, (5, 1): 14, (5, 2): 15, (5, 3): 16,      # TEXCOORD
    (10, 0): 17, (2, 0): 18, (1, 0): 19,                 # COLOR0, BLENDINDICES, BLENDWEIGHT
    (10, 1): 20, (5, 4): 21, (5, 5): 22,                 # COLOR1, TEXCOORD4/5
}


EXEC_OPCODES = {1, 2, 3, 4, 5, 6, 13, 14}


def canonical_microcode(code: bytes) -> bytes:
    """Zero the bind-time-patchable fields of every vertex fetch (the runtime does the same).

    D3D patches vfetch format/offset/stride (and possibly the fetch constant) per vertex
    declaration and reorders the fetches (their destination registers permute), so the
    destination register is dropped too;
    declaration, including the destination swizzle (observed: 0x688 -> 0x60a); every ALU and
    texture instruction is kept.
    Fetch slots come from the exec clauses' sequence bits in the control-flow prefix."""
    words = list(struct.unpack(f'>{len(code) // 4}I', code))
    end = len(words) // 3  # instruction slots; the CF prefix ends at the lowest exec address
    slot = 0
    fetches = set()
    while slot < end:
        w0, w1, w2 = words[3 * slot:3 * slot + 3]
        for lo, hi in ((w0, w1 & 0xFFFF), ((w1 >> 16) | ((w2 << 16) & 0xFFFFFFFF), w2 >> 16)):
            opcode = (hi >> 12) & 0xF
            if opcode in EXEC_OPCODES:
                address, count, sequence = lo & 0xFFF, (lo >> 12) & 7, (lo >> 16) & 0xFFF
                if address:
                    end = min(end, address)
                for i in range(count):
                    if (sequence >> (2 * i)) & 1:
                        fetches.add(address + i)
        slot += 1
    for f in fetches:
        if 3 * f + 2 < len(words) and (words[3 * f] & 0x1F) == 0:  # vertex fetch
            words[3 * f] &= ~((0x3FF << 20) | (0x7F << 12)) & 0xFFFFFFFF  # const, prefetch, dst register
            words[3 * f + 1] = 0                         # format, signedness, dst swizzle (all patched)
            words[3 * f + 2] = 0                         # stride, offset
    return struct.pack(f'>{len(words)}I', *words)


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def parse_container(data: bytes, stage: str) -> tuple[bytes, list[tuple[int, int, int, int, int]], int, int]:
    if len(data) < 36:
        raise ValueError('short container')
    (_flags, virtual_size, physical_size, _c, _const, _defs, shader_offset, _1c, _20) = struct.unpack_from('>9I', data, 0)
    if shader_offset + 24 > virtual_size or virtual_size + physical_size > len(data):
        raise ValueError('container extents')
    physical_offset, size, _f8, _fc, _f10, _interp = struct.unpack_from('>6I', data, shader_offset)
    start = virtual_size + physical_offset
    if size == 0 or start + size > len(data) or size % 12:
        raise ValueError('microcode extent')
    microcode = data[start:start + size]
    elements: list[tuple[int, int, int, int, int]] = []
    outputs = 0
    samplers = 0  # D3DX constant table: sampler register set (3) -> register index bits
    (_flags2, _v2, _p2, _c2, table_offset) = struct.unpack_from('>5I', data, 0)
    if table_offset and table_offset + 32 <= virtual_size:
        table = table_offset + 4  # ConstantTableContainer.size, then the D3DX table
        count, info = struct.unpack_from('>II', data, table + 12)
        if count < 4096 and table + info + 20 * count <= virtual_size:
            for i in range(count):
                register_set, register_index, register_count = struct.unpack_from('>HHH', data, table + info + 20 * i + 4)
                if register_set == 3:
                    for r in range(register_index, min(register_index + max(register_count, 1), 16)):
                        samplers |= 1 << r
    if stage == 'vertex':
        field18, count, _field20 = struct.unpack_from('>3I', data, shader_offset + 24)
        base = shader_offset + 36
        if count > MAX_ELEMENTS or base + 4 * (field18 + count) > virtual_size:
            raise ValueError('vertex elements')
        for i in range(count):
            value, = struct.unpack_from('>I', data, base + 4 * (field18 + i))
            address, usage, index = value & 0xFFF, (value >> 12) & 0xF, (value >> 16) & 0xF
            location = LOCATIONS.get((usage, index))
            if location is None:
                raise ValueError(f'vertex element usage {usage}/{index} has no location')
            if address * 12 >= size:
                raise ValueError('vertex element address outside microcode')
            w0 = struct.unpack_from('>I', microcode, 12 * address)[0]
            elements.append((address, usage, index, location, (w0 >> 12) & 0x3F))
    else:
        _field18, outputs = struct.unpack_from('>2I', data, shader_offset + 24)
    return microcode, elements, outputs, samplers


def main() -> None:
    global ROOT
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--root', type=Path, default=ROOT, help='translation records root')
    parser.add_argument('--out', type=Path, default=ROOT / 'local-analysis/generic-shaders/generic-shaders.bin')
    # (v41, additive) translation record directories to read, in order (the first record of a
    # microcode key wins); default: the v3 corpus, exactly as before.
    parser.add_argument('--corpus', action='append', metavar='GLOB',
                        help='glob of translation record directories under the repository root '
                             '(repeatable; default local-analysis/shader-translation-v3-*/shader-*)')
    args = parser.parse_args()
    ROOT = args.root.resolve()
    corpus = args.corpus or ['local-analysis/shader-translation-v3-*/shader-*']
    records: dict[int, tuple[int, bytes, list, int]] = {}
    seen = duplicates = conflicts = refused = 0
    for directory in [d for pattern in corpus for d in sorted(ROOT.glob(pattern))]:
        spv, container, checkpoint = (directory / 'translated.spv', directory / 'container.xenos',
                                      directory / 'checkpoint.json')
        if not (spv.exists() and container.exists() and checkpoint.exists()):
            continue
        index = int(directory.name.split('-')[1])  # each checkpoint lists every record so far
        record = next((r for r in json.loads(checkpoint.read_text(encoding='utf-8'))['records'] if r.get('index') == index), None)
        if record is None or record.get('status') != 'validated':
            continue
        seen += 1
        stage = record['stage']
        try:
            microcode, elements, outputs, samplers = parse_container(container.read_bytes(), stage)
        except ValueError as error:
            refused += 1
            print(f'refused {directory.relative_to(ROOT)}: {error}')
            continue
        key = fnv1a64(canonical_microcode(microcode))
        code = spv.read_bytes()
        if key in records:
            duplicates += 1
            if records[key][1] != code:
                conflicts += 1  # same microcode, different constant table; keep the first
            continue
        records[key] = (0 if stage == 'vertex' else 1, code, elements, outputs, samplers)
    entry_size = 8 + 4 * 4 + 4 * MAX_ELEMENTS + 8 + MAX_ELEMENTS
    header = struct.pack('<4sIII', b'GSP2', 2, len(records), 0)
    offset = len(header) + entry_size * len(records)
    entries, blobs = [], []
    for key in sorted(records):
        stage, code, elements, outputs, samplers = records[key]
        words = [a | (u << 12) | (i << 16) | (loc << 24) for a, u, i, loc, _ in elements]
        dsts = bytes([dst for *_, dst in elements] + [0] * (MAX_ELEMENTS - len(elements)))
        words += [0] * (MAX_ELEMENTS - len(words))
        entries.append(struct.pack('<QIIII', key, stage, offset, len(code) // 4, len(elements)) +
                       struct.pack(f'<{MAX_ELEMENTS}I', *words) + struct.pack('<II', outputs, samplers) + dsts)
        blobs.append(code + b'\0' * (-len(code) % 4))
        offset += len(blobs[-1])
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(header + b''.join(entries) + b''.join(blobs))
    stages = [r[0] for r in records.values()]
    print(f'{args.out}: {len(records)} shaders ({stages.count(0)} vertex, {stages.count(1)} pixel) from {seen} validated '
          f'records; {duplicates} duplicate microcode ({conflicts} with different SPIR-V), {refused} refused; '
          f'{args.out.stat().st_size} bytes')


if __name__ == '__main__':
    main()
