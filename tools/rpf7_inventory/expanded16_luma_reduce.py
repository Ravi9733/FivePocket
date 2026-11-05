"""Offline eight-tap scalar reduction of EXPAND/binary16, not draw admission.

Keep the original arithmetic tree and dynamic constants. Xenia's signed
half-texel offsets and 1.5/1024-texel point-sampling adjustment are explicit.
This compiler does not establish output ownership, R32 storage or completion.
"""
import math
from raw_shader_analysis import RawShaderError
from pixel_sample_dataflow import analyze_pixel_sample_dataflow
from inline_shader_translation import translate_inline_pair, COMMON
from expanded16_max_reduce import ROUNDING_OFFSET
from unorm8_vertex_fetch import f32

OFFSETS = {(x, y) for x in (-1.5, -.5, .5, 1.5) for y in (-.5, .5)}


def expanded16_point_binding(fetch):
    if (not isinstance(fetch, (list, tuple)) or len(fetch) != 6 or
            any(type(w) is not int or not 0 <= w <= 0xFFFFFFFF for w in fetch)):
        raise RawShaderError('six submitted fetch words required')
    a, b, c, d, e, f = fetch
    if ((a, c, d, e, f) != (0x82804956, 0x16613F, 0xD11, 0, 0x210) or
            b & 4095 != 0x5D or not b & 0xFFFFF000 or
            b & 0xFFFFF000 >= 0x20000000):
        raise RawShaderError('320x180 signed EXPAND base-only POINT clamp-edge required')
    return dict(width=320, height=180, point=True, native_sampling_verified=False)


def analyze_luma_reduce(pixel_code):
    proof = analyze_pixel_sample_dataflow(pixel_code, ui_operations=True,
        extended_math=True, loading_math=True, rectangle_mrt_profile=True)
    nodes, fetches = proof['nodes'], proof['fetches']
    if (proof['kills'] or set(proof['exports']) != {'0'} or len(fetches) != 8 or
            any(n[0] not in ('initial', 'sample', 'constant', 'mul', 'add', 'select_eq0') for n in nodes)):
        raise RawShaderError('eight unconditional samples and scalar arithmetic required')
    if {tuple(f.get('texel_offset', (0, 0))) for f in fetches} != OFFSETS:
        raise RawShaderError('eight distinct signed half-texel positions required')
    for fetch in fetches:
        if (fetch['slot'] != 0 or not fetch['computed_lod_requested'] or
                not fetch['fetch_valid_only'] or
                [tuple(nodes[n]) for n in fetch['coordinates']] != [('initial', 0, 0), ('initial', 0, 1)]):
            raise RawShaderError('slot0 unmodified normalized XY fetch required')
    out = proof['exports']['0']
    if out[0] is None or any(n != out[0] for n in out):
        raise RawShaderError('one scalar replicated to colour0 required')
    # Walk only output dependencies: no uninitialized value or sample alpha
    # may silently enter the scalar result; every tap must contribute.
    seen, taps = set(), set()
    def visit(i):
        if i in seen: return
        seen.add(i)
        n = nodes[i]
        if n[0] == 'initial': raise RawShaderError('unprovided scalar input')
        if n[0] == 'sample':
            if n[2] >= 3: raise RawShaderError('sample alpha is not part of this reduction')
            taps.add(n[1])
        elif n[0] == 'constant':
            if n[1] not in (18, 254, 255): raise RawShaderError('unexpected scalar constant')
        else:
            for child in n[1:]: visit(child)
    visit(out[0])
    if taps != set(range(8)): raise RawShaderError('every tap must reach the scalar output')
    return proof


def point_sample_reference(coordinates, offset, binding, read_pixel):
    if (len(coordinates) != 2 or tuple(offset) not in OFFSETS or
            (binding.get('width'), binding.get('height'), binding.get('point')) != (320, 180, True) or
            any(not math.isfinite(v) or f32(v) != v or abs(v) > 65536 for v in coordinates)):
        raise RawShaderError('finite float32 XY and exact point profile required')
    position = []
    for uv, off, size in zip(coordinates, offset, (320, 180)):
        shifted = f32(uv + f32(f32(off + ROUNDING_OFFSET) / size))
        position.append(min(size - 1, max(0, math.floor(f32(shifted * size)))))
    pixel = read_pixel(*position)
    if len(pixel) != 4 or any(not math.isfinite(v) or f32(v) != v for v in pixel):
        raise RawShaderError('finite float32 source channels required')
    return pixel


def translate_luma_reduce(vertex_code, pixel_code, fetch):
    binding, pixel = expanded16_point_binding(fetch), analyze_luma_reduce(pixel_code)
    vertex_text, _, pair = translate_inline_pair(vertex_code, pixel_code,
        extended_profile=True, loading_profile=True, rectangle_list=True, offset_sample_profile=True)
    vertex = pair['vertex']
    def copied(i):
        if i is None: raise RawShaderError('unwritten vertex output')
        n = vertex['nodes'][i]
        while n[0] == 'maximum' and n[1] == n[2]: n = vertex['nodes'][n[1]]
        return tuple(n)
    if (set(vertex['exports']) != {'0', '62'} or len(vertex['fetches']) != 2 or
            vertex.get('texture_fetches') or pair['stride'] != 24 or
            [copied(i) for i in vertex['exports']['62']] !=
                [('fetch', 0, 0), ('fetch', 0, 1), ('fetch', 0, 2), ('literal', 1)] or
            [copied(i) for i in vertex['exports']['0'][:2]] != [('fetch', 1, 0), ('fetch', 1, 1)]):
        raise RawShaderError('original position and UV rectangle required')
    for f in vertex['fetches']:
        if (f['fetch_slot'] != 95 or f['stride_words'] != 6 or
                tuple(vertex['nodes'][f['index_expression']]) != ('initial', 0, 0)):
            raise RawShaderError('sequential inline vertices required')
    if [(f['format'], f['components'], f['offset_words']) for f in vertex['fetches']] != [(57, 3, 0), (37, 2, 4)]:
        raise RawShaderError('float3 position and float2 UV required')
    lines, done = [], {}
    for i, f in enumerate(pixel['fetches']):
        x, y = f['texel_offset']
        lines += [f'precise vec2 offset{i}=vec2({x+ROUNDING_OFFSET},{y+ROUNDING_OFFSET})/vec2(textureSize(atlas0,0));',
                  f'precise vec2 shifted{i}=interpolator0+offset{i};',
                  f'vec4 sampled{i}=textureLod(atlas0,shifted{i},0.0);']
    def expression(i):
        if i in done: return done[i]
        n = pixel['nodes'][i]
        if n[0] == 'sample': value = f'sampled{n[1]}[{n[2]}]'
        elif n[0] == 'constant': value = f'uintBitsToFloat(readBE({1028+n[1]*4+n[2]}u))'
        elif n[0] in ('mul', 'add', 'select_eq0'):
            children = [expression(child) for child in n[1:]]
            value = f'n{i}'
            rhs = (f'({children[0]}==0.0?{children[1]}:{children[2]})' if n[0] == 'select_eq0'
                   else f'({children[0]}{"*" if n[0] == "mul" else "+"}{children[1]})')
            lines.append(f'precise float {value}={rhs};')
        else: raise RawShaderError('unexpected scalar output dependency')
        done[i] = value
        return value
    output = expression(pixel['exports']['0'][0])
    lines.append(f'color=vec4({output});')
    fragment = COMMON + '''layout(set=0,binding=1) uniform sampler2D atlas0;
layout(location=0) in vec2 interpolator0;
layout(location=0) out vec4 color;
void main(){
''' + '\n'.join(lines) + '\n}\n'
    return vertex_text, fragment, dict(vertex=vertex, pixel=pixel, binding=binding,
        texture_slots=[0], stride=24, xenia_offset_policy=True, base_lod_only=True,
        original_vertex_translation=True, native_game_pipeline=False,
        runtime_state_verified=False, gpu_executed=False, gameplay=False)
