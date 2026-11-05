"""Bounded side-effect inventory, NOT translation or execution permission.

For a stencil-rejection proof, arithmetic values are immaterial only after the
native consumer proves that *every* raster sample fails, that stencil/depth do
not change, and that no query is active. This inventory excludes shader memory,
depth and kill effects across BOTH sides of forward branches. It never picks a
branch from a saved constant or assumes that an unsupported shader is inert.

Encoding reference: pinned Xenia-derived ucode.h, XenDroid 779680ad. Independently
implemented; no shader payload or external source is copied into this module.
"""
import hashlib
import struct
from raw_shader_analysis import RawShaderError


# Explicit observed arithmetic families: register/predicate changes only.
# FLOORv, EXP_IEEE and SIN/COS need no numeric model to classify their effects.
# Unknown encodings, kills, address-register mutation and memory exports refuse.
# Xenia ucode.h: SGTv5 and CNDGTv14 only write register lanes;
# SUB_CONST_0s46 only writes scalar arithmetic state. These are NOT the
# similarly named KILLGTv25/KILLSGTs36 instructions. Classifying their effects
# does not implement their numeric or texture-sampling semantics.
VECTORS = frozenset((0, 1, 2, 5, 6, 8, 10, 11, 12, 14, 16, 17))
SCALARS = frozenset((3, 5, 11, 13, 14, 19, 22, 28, 40, 42, 43, 44, 46, 48, 49, 50))


def inventory_colour_pixel_effects(data):
    if type(data) is not bytes or not data or len(data) % 12 or len(data) > 4096:
        raise RawShaderError('invalid bounded pixel shader extent')
    words = struct.unpack('>' + str(len(data) // 4) + 'I', data)
    extent = len(words) // 3
    controls, body, ended = [], extent, False
    for pair in range(extent):
        if pair >= body:
            break
        a, b, c = words[pair * 3:pair * 3 + 3]
        packed = (a | ((b & 65535) << 32), (b >> 16) | (c << 16))
        for half, cf in enumerate(packed):
            if len(controls) == 128:
                raise RawShaderError('pixel effect control budget')
            op = cf >> 44
            row = dict(index=len(controls), opcode=op)
            if op == 0:
                if cf:
                    raise RawShaderError('noncanonical NOP')
            elif op == 12:
                if cf & ~((15 << 44) | (7 << 40) | 7) or (cf >> 41) & 3 != 2:
                    raise RawShaderError('memory/unknown allocation')
                if cf & 7:
                    raise RawShaderError('only single colour allocation supported')
            elif op in (1, 2, 5):
                allowed = (15 << 44) | (1 << 43) | (1 << 41) | ((1 << 34) - 1)
                if op == 5:
                    allowed |= 1 << 42
                if cf & ~allowed:
                    raise RawShaderError('unknown execution control fields')
                address, count, sequence = cf & 4095, (cf >> 12) & 7, (cf >> 16) & 4095
                if not 1 <= count <= 6 or address + count > extent or sequence >> (count * 2):
                    raise RawShaderError('invalid execution extent/sequence')
                row.update(address=address, count=count, sequence=sequence)
                body = min(body, address)
            elif op == 11:
                # Only a forward predicate jump. Both successors are inspected;
                # do not translate SIMD-wide branch semantics to scalar tests.
                allowed = (15 << 44) | (1 << 42) | (1 << 14) | 8191
                if cf & ~allowed or not cf & (1 << 14):
                    raise RawShaderError('only forward predicate jump fields supported')
                row['target'] = cf & 8191
            else:
                raise RawShaderError('loop/call/unknown control effect')
            controls.append(row)
            if op == 2:
                if half == 0 and packed[1]:
                    raise RawShaderError('nonzero trailing control padding')
                ended = True
                break
        if ended:
            break
    if not ended or (len(controls) + 1) // 2 > body:
        raise RawShaderError('unterminated or overlapping control storage')
    for row in controls:
        if row['opcode'] == 11 and not row['index'] < row['target'] < len(controls):
            raise RawShaderError('backward/self/out-of-range jump')
    if sum(r['opcode'] == 12 for r in controls) != 1:
        raise RawShaderError('single colour allocation required')

    instructions, exports, slots, seen = [], set(), set(), {}
    for control in controls:
        if control['opcode'] not in (1, 2, 5):
            continue
        for at in range(control['address'], control['address'] + control['count']):
            if at < body or at < (len(controls) + 1) // 2:
                raise RawShaderError('execution overlaps control storage')
            fetch = bool(control['sequence'] & (1 << (2 * (at - control['address']))))
            if at in seen:
                if seen[at] != fetch:
                    raise RawShaderError('conflicting instruction decoding')
                continue
            if len(instructions) == 256:
                raise RawShaderError('pixel effect instruction budget')
            seen[at] = fetch
            a, b, c = words[at * 3:at * 3 + 3]
            row = dict(address=at, kind='fetch' if fetch else 'alu')
            if fetch:
                if a & 31 != 1:
                    raise RawShaderError('non-texture-fetch effect')
                # Texture fetch is read-only regardless of its addressing,
                # filter, LOD or predicate. No sampling compatibility claimed.
                slots.add((a >> 20) & 31)
                row['slot'] = (a >> 20) & 31
            else:
                vector, scalar = (c >> 24) & 31, a >> 26
                if vector not in VECTORS or scalar not in SCALARS:
                    raise RawShaderError('unknown/kill/address ALU effect')
                if a & ((1 << 6) | (1 << 7)) or b & 0xE0000000:
                    raise RawShaderError('relative or unknown ALU addressing')
                # Do not decode unused ALU operands as active register reads:
                # scalar-constant instructions repurpose these fields. Operand
                # values cannot create a memory export absent export_data and
                # its destination; this is effect classification, not dataflow.
                row.update(vector=vector, scalar=scalar)
                if a & (1 << 15):
                    if a & 63 != 0:
                        raise RawShaderError('non-colour0 export including depth/memory')
                    exports.add(0)
                    row['export'] = 0
            instructions.append(row)
    if exports != {0}:
        raise RawShaderError('colour0 export required')
    return dict(sha256=hashlib.sha256(data).hexdigest(), bytes=len(data),
                controls=controls, instructions=instructions, exports=[0],
                texture_slots=sorted(slots), all_forward_paths_inspected=True,
                memory_exports=False, depth_exports=False, kill_effects=False,
                arithmetic_verified=False, sampling_verified=False,
                control_flow_execution_verified=False, native_admission=False)
