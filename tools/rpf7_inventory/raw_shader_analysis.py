"""Prove copy/constant exports for a deliberately small raw Xenos shader subset.

No ShaderContainer metadata is invented. Initial temporary components remain
symbolic: this module does not establish pixel-input linkage or GPU state.
Unknown control flow, math, predication and memory exports reject. Fetches reject
by default; a separate explicit vertex-only mode traces bounded float32 fetches.
Encoding references: Xenia ucode.h at 95a5c3ee250f80c3b9d139658649d9ffb6db3eec
and pinned XenosRecomp shader_code.h. This is independently written analysis.
"""
import hashlib
import math
import re
import struct


class RawShaderError(ValueError):
    pass


def _vertex_fetch_layout(words, previous, registers, address, fmt, components):
    """Address/swizzle only; caller must validate format and modifier fields."""
    a,b,c = words
    if a & 31 or not a & (1 << 19) or a & (1 << 18) or b & (1 << 31) or c >> 31:
        raise RawShaderError('unsupported vertex fetch opcode/addressing/predication')
    mini = bool(b & (1 << 30))
    offset = (c >> 8) & 0x7FFFFF
    if offset & 0x400000: raise RawShaderError('negative vertex offset unsupported')
    if mini:
        if previous is None: raise RawShaderError('mini fetch lacks a prior full fetch')
        addressing = previous.copy()
    else:
        select = (a >> 25) & 3
        if a & (1 << 11) or select == 3 or not c & 255:
            raise RawShaderError('unsupported full vertex fetch addressing')
        src, axis = (a >> 5) & 63, a >> 30
        addressing = {'index_expression':registers[src][axis],
            'fetch_slot':((a >> 20) & 31)*3+select, 'stride_words':c & 255}
    swizzle = [(b >> (3*i)) & 7 for i in range(4)]
    if any(v == 6 or (v < 4 and v >= components) for v in swizzle):
        raise RawShaderError('undefined or out-of-format vertex swizzle')
    record = dict(addressing, instruction_address=address, mini=mini, format=fmt,
        components=components, offset_words=offset, destination=(a >> 12) & 63,
        destination_swizzle=swizzle)
    return record, previous if mini else addressing


def _float_vertex_fetch(words, previous, registers, address):
    """Restricted float32 fetch; mini inherits the saved full addressing/index."""
    fmt = (words[1] >> 16) & 63
    components = {36:1,37:2,38:4,57:3}.get(fmt)
    if components is None or (words[1] & 0x3FC0F000) != 0x3000:
        raise RawShaderError('unsupported vertex float format/modifiers')
    return _vertex_fetch_layout(words, previous, registers, address, fmt, components)


def _sint16_vertex_fetch(words, previous, registers, address):
    """Signed, unnormalized 16:16 converted to float; no exponent adjustment.

    Xenia ucode.h VertexFetchInstruction: signed bit12, integer bit13;
    spirv_shader_translator_fetch.cc places lane0 in the low half-word.
    """
    if (words[1] >> 16) & 63 != 25 or words[1] & 0x3FC0F000 != 0x3000:
        raise RawShaderError('unsupported signed16 vertex format/modifiers')
    return _vertex_fetch_layout(words, previous, registers, address, 25, 2)


def analyze_copy_shader(data, stage, *, allow_vertex_fetch=False):
    if stage not in ('vertex', 'pixel') or not data or len(data) % 12 or len(data) > 4096:
        raise RawShaderError('invalid raw shader stage or instruction extent')
    if allow_vertex_fetch and stage != 'vertex':
        raise RawShaderError('vertex fetch analysis is vertex-stage only')
    words = struct.unpack('>' + 'I' * (len(data) // 4), data)
    instruction_count = len(words) // 3
    controls, body_start, ended = [], instruction_count, False
    # Two little-bit-order 48-bit control words within three stored BE dwords.
    for pair in range(instruction_count):
        if pair >= body_start: break
        a, b, c = words[pair * 3:pair * 3 + 3]
        for cf in (a | ((b & 65535) << 32), (b >> 16) | (c << 16)):
            if len(controls) == 128: raise RawShaderError('control-flow budget exceeded')
            opcode = cf >> 44
            if opcode == 0:
                if cf: raise RawShaderError('noncanonical NOP control')
                controls.append(('nop',))
            elif opcode == 12:
                # Only buffer reservation size/type/unserialized hint. Reserved
                # bits are not assigned semantics by this restricted analyzer.
                if cf & ~((15 << 44) | (7 << 40) | 7):
                    raise RawShaderError('unsupported ALLOC fields')
                kind, size = (cf >> 41) & 3, (cf & 7) + 1
                if kind not in ((1, 2) if stage == 'vertex' else (2,)):
                    raise RawShaderError('unsupported output allocation (including memory export)')
                if (kind == 1 and size != 1) or (stage == 'pixel' and size > 4):
                    raise RawShaderError('unsupported output allocation extent')
                controls.append(('alloc', kind, size))
            elif opcode in (1, 2):
                # Accept predicate-clean/address-mode and serialize/yield hints;
                # no relative references, predicates, fetches or cache fields.
                cache_fields = (63 << 28) if allow_vertex_fetch else 0
                if cf & ~((15 << 44) | (1 << 43) | (1 << 41) | ((1 << 28) - 1) | cache_fields):
                    raise RawShaderError('unsupported EXEC fields')
                address, count, sequence = cf & 4095, (cf >> 12) & 7, (cf >> 16) & 4095
                if not count or count > 6 or address + count > instruction_count or sequence >> (count * 2):
                    raise RawShaderError('invalid EXEC range or sequence')
                if sequence & 0x555 and not allow_vertex_fetch: raise RawShaderError('fetch instruction unsupported')
                body_start = min(body_start, address)
                controls.append(('exec', address, count, sequence))
                if opcode == 2:
                    ended = True
                    break
            else:
                raise RawShaderError('control-flow opcode unsupported')
        if ended: break
    if not ended or (len(controls) + 1) // 2 > body_start:
        raise RawShaderError('missing termination or overlapping control/instruction storage')

    registers = [[f'initial_r{r}.{axis}' for axis in 'xyzw'] for r in range(64)]
    exports, allocated, executed, inputs = {}, {}, [], set()
    fetches, previous_full = [], None
    for control in controls:
        if control[0] == 'alloc':
            kind, size = control[1:]
            # Repeated allocations have hardware sequencing implications.
            if kind in allocated: raise RawShaderError('repeated output allocation unsupported')
            allocated[kind] = size
        if control[0] != 'exec': continue
        for address in range(control[1], control[1] + control[2]):
            a, b, c = words[address * 3:address * 3 + 3]
            if control[3] & (1 << ((address-control[1])*2)):
                fetch, previous_full = _float_vertex_fetch((a,b,c),previous_full,registers,address)
                index = len(fetches)
                if index == 32: raise RawShaderError('vertex fetch budget exceeded')
                target = registers[fetch['destination']]
                for axis, source in enumerate(fetch['destination_swizzle']):
                    if source < 4: target[axis] = f'fetch{index}.{ "xyzw"[source] }'
                    elif source < 6: target[axis] = str(source-4)
                if fetch['index_expression'].startswith('initial_'): inputs.add(fetch['index_expression'])
                fetches.append(fetch);executed.append(address)
                if len(executed)>256: raise RawShaderError('executed instruction budget exceeded')
                continue
            if (c >> 24) & 31 != 2 or a >> 26 != 50:
                raise RawShaderError('only MAX vector / RETAIN_PREV scalar supported')
            if a & ((1 << 6) | (1 << 7) | (3 << 24)) or b >> 24:
                raise RawShaderError('relative, absolute, clamp, negate or predicate unsupported')
            export, dest = bool(a & (1 << 15)), a & 63
            vector, scalar = (a >> 16) & 15, (a >> 20) & 15
            scalar_relative = bool(a & (1 << 14))
            if export:
                vector_result, scalar_result = vector & ~scalar, scalar & ~vector
                zero = 15 & ~(vector | scalar) if scalar_relative else 0
                one = vector & scalar
                kind = 1 if stage == 'vertex' and dest == 62 else 2
                if kind not in allocated or (kind == 2 and dest >= allocated[kind]):
                    raise RawShaderError('export outside allocated outputs')
                target = exports.setdefault(dest, ['unwritten'] * 4)
            else:
                vector_result, scalar_result, zero, one = vector, scalar, 0, 0
                if scalar_relative: raise RawShaderError('relative temporary destination unsupported')
                target = registers[dest]
            if scalar_result: raise RawShaderError('previous scalar result dependency unsupported')
            if vector_result:
                src1, src2 = (c >> 16) & 255, (c >> 8) & 255
                swizzle1, swizzle2 = (b >> 16) & 255, (b >> 8) & 255
                if c >> 30 != 3 or src1 >= 64 or src2 != src1 or swizzle1 != swizzle2:
                    raise RawShaderError('only identical absolute temporary MAX operands supported')
                # MAX(t,t) is a copy for the defined finite inputs used by the
                # eventual probe. No NaN payload/denormal fidelity is claimed.
                source = registers[src1].copy()
                for component in range(4):
                    if vector_result & (1 << component):
                        value = source[((swizzle1 >> (2 * component)) + component) & 3]
                        if value.startswith('initial_'): inputs.add(value)
                        target[component] = value
            for component in range(4):
                if zero & (1 << component): target[component] = '0'
                if one & (1 << component): target[component] = '1'
            executed.append(address)
            if len(executed) > 256: raise RawShaderError('executed instruction budget exceeded')
    if not exports: raise RawShaderError('no analyzed output exports')
    report = {'stage': stage, 'byte_count': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
        'analysis': 'restricted-copy-constant-exports', 'control_count': len(controls),
        'executed_instruction_addresses': executed,
        'exports': {str(key): value for key, value in sorted(exports.items())},
        'initial_components_read': sorted(inputs), 'linkage_validated': False,
        'hardware_float_edge_cases_validated': False, 'gpu_executed': False}
    if allow_vertex_fetch:
        report.update(analysis='restricted-float32-vertex-fetch-copy-exports',vertex_fetches=fetches,
            vertex_cache_timing_modeled=False,resource_bytes_read=False,vertex_index_input_verified=False)
    return report


def evaluate_float_vertex_copy(data, buffers, vertex_index):
    """CPU experiment: explicit r0.x auto-index and owned BE float32 buffers.

    This is not the runtime shader interpreter and does not establish actual
    Xbox input-register setup, draw predication, viewport, linkage or cache state.
    Buffer entries are exact captured byte ranges keyed by fetch slot. Unsupported
    dataflow, out-of-range reads and non-finite values reject the whole result.
    """
    if type(vertex_index) is not int or not 0 <= vertex_index < (1 << 24):
        raise RawShaderError('invalid explicit vertex index')
    proof = analyze_copy_shader(data,'vertex',allow_vertex_fetch=True)
    fetched = []
    for fetch in proof['vertex_fetches']:
        if fetch['index_expression'] != 'initial_r0.x':
            raise RawShaderError('fetch index is not the explicit auto-index input')
        source = buffers.get(fetch['fetch_slot'])
        if not isinstance(source,bytes) or not source or len(source) > 4096 or len(source) % 4:
            raise RawShaderError('missing bounded owned BE vertex buffer')
        start = (vertex_index*fetch['stride_words'] + fetch['offset_words'])*4
        size = fetch['components']*4
        if start+size > len(source): raise RawShaderError('vertex fetch exceeds captured buffer')
        values = struct.unpack_from('>'+str(fetch['components'])+'f',source,start)
        if not all(math.isfinite(v) for v in values): raise RawShaderError('non-finite vertex input unsupported')
        fetched.append(values)
    def resolve(expression):
        if expression in ('0','1'): return float(expression)
        match = re.fullmatch(r'fetch([0-9]+)\.([xyzw])',expression)
        if match is None: raise RawShaderError('unresolved vertex export dependency')
        return fetched[int(match[1])]['xyzw'.index(match[2])]
    return {slot:[resolve(e) for e in values] for slot,values in proof['exports'].items()}


def translate_float_vertex_copy_compute(data):
    """Derived compute dataflow probe, NOT a translated native vertex pipeline.

    Only explicit auto-index 0..2, one captured BE buffer and position/parameter0
    exports. Output uvec4 slots 0/7 remain canaries; each vertex owns two slots.
    No viewport, primitive completion, predicate, pixel shader or EDRAM model.
    """
    proof=analyze_copy_shader(data,'vertex',allow_vertex_fetch=True)
    if (not proof['vertex_fetches'] or set(proof['exports']) not in ({'62'},{'0','62'}) or
            any(f['fetch_slot']!=0 or f['index_expression']!='initial_r0.x' for f in proof['vertex_fetches'])):
        raise RawShaderError('compute probe requires explicit slot0 auto-index position/parameter exports')
    statements=[]
    for i,f in enumerate(proof['vertex_fetches']):
        base=f"(index*{f['stride_words']}u+{f['offset_words']}u)"
        statements.append(f"if ({base}+{f['components']}u>uint(source.words.length())) return;")
        for c in range(f['components']):
            statements.append(f"float fetch{i}_{'xyzw'[c]}=uintBitsToFloat(readBE({base}+{c}u));")
            lane=f"fetch{i}_{'xyzw'[c]}"
            statements.append(f"if (isnan({lane}) || isinf({lane})) return;")
    def expression(e):
        if e in ('0','1'):return e+'.0'
        match=re.fullmatch(r'fetch([0-9]+)\.([xyzw])',e)
        if not match:raise RawShaderError('unresolved compute export')
        return f'fetch{match[1]}_{match[2]}'
    for slot,offset in (('62',1),('0',2)):
        if slot in proof['exports']:
            values=','.join(expression(e) for e in proof['exports'][slot])
            statements.append(f"result.words[{offset}u+index*2u]=floatBitsToUint(vec4({values}));")
    shader=('''#version 450
layout(local_size_x=1) in;
layout(set=0,binding=0,std430) readonly buffer Source { uint words[]; } source;
layout(set=0,binding=1,std430) buffer Result { uvec4 words[]; } result;
uint readBE(uint at) {
    uint v=source.words[at];
    return (v<<24)|((v&0xff00u)<<8)|((v>>8)&0xff00u)|(v>>24);
}
void main() {
    uint index=gl_GlobalInvocationID.x;
    if(index>=3u || result.words.length()<8) return;
'''+''.join('    '+line+'\n' for line in statements)+'}\n')
    return shader,dict(proof,probe_stage='compute',native_vertex_pipeline=False,
        input_byte_order='big-endian-captured-dwords',native_shader_memory_byte_order='little-endian',
        finite_input_required=True,output_bytes=128,maximum_invocations=3,frame_presented=False)


def translate_constant_pair(vertex, pixel, program_control, context_misc):
    """GLSL for the proved pair in an explicit location-0 native probe linkage.

This does not replay title viewport/raster state. No Xenos container or shader
declaration is synthesized. Pixel parameter generation must be disabled.
"""
    vs, ps = analyze_copy_shader(vertex, 'vertex'), analyze_copy_shader(pixel, 'pixel')
    if program_control != 0x1000000E or context_misc != 0:
        raise RawShaderError('unvalidated shader register/linkage profile')
    if (set(vs['exports']) != {'0', '62'} or vs['initial_components_read'] or
            any(v not in ('0', '1') for values in vs['exports'].values() for v in values) or
            ps['exports'] != {'0': ['initial_r0.' + c for c in 'xyzw']}):
        raise RawShaderError('pair does not prove constant vertex / interpolator-copy pixel exports')
    # Values are exclusively analyzer-produced literal zero/one components,
    # not arbitrary shader text. Point size is the native 1-pixel probe profile.
    def vector(values): return 'vec4(' + ','.join(v + '.0' for v in values) + ')'
    vertex_glsl = ('#version 450\nlayout(location=0) out vec4 parameter0;\nvoid main(){\n'
        'gl_Position=' + vector(vs['exports']['62']) + ';\ngl_PointSize=1.0;\n'
        'parameter0=' + vector(vs['exports']['0']) + ';\n}\n')
    pixel_glsl = ('#version 450\nlayout(location=0) in vec4 parameter0;\n'
        'layout(location=0) out vec4 color0;\nvoid main(){color0=parameter0;}\n')
    return vertex_glsl, pixel_glsl, {'vertex': vs, 'pixel': ps,
        'native_linkage': 'location-0-from-vertex-export-0-to-pixel-r0',
        'parameter_generation_disabled': True, 'title_raster_state_replayed': False,
        'native_probe_point_size': 1, 'gpu_executed': False}
