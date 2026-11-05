"""Offline four-tap EXPAND/binary16 maximum reduction, not draw admission.

Reuse the original offset shader's dataflow and rectangle vertex translation.
Xenia's SPIR-V fetch adds 1.5/1024 texel before normalization; preserve this
policy explicitly. It is not a claim of Xbox filtering/interpolation precision.
Only finite base-level RGBA16F, LINEAR XY, clamp-edge and zero bias are modeled.
"""
import math
from raw_shader_analysis import RawShaderError
from pixel_sample_dataflow import analyze_pixel_sample_dataflow
from inline_shader_translation import translate_inline_pair, COMMON
from unorm8_vertex_fetch import f32

ROUNDING_OFFSET = 1.5 / 1024


def expanded16_binding(fetch):
    if (not isinstance(fetch, (list, tuple)) or len(fetch) != 6 or
            any(type(w) is not int or not 0 <= w <= 0xFFFFFFFF for w in fetch)):
        raise RawShaderError('six submitted fetch words required')
    a, b, c, d, e, f = fetch
    width, height = (c & 8191) + 1, ((c >> 13) & 8191) + 1
    # Exact observed numeric/sampler family, variable physical base/dimensions.
    # Sign1 EXPAND still maps to floating point; never SNORM or integer pixels.
    if (a & ~(511 << 22) != 0x80004956 or b & 4095 != 0x5D or
            not b & 0xFFFFF000 or b & 0xFFFFF000 >= 0x20000000 or
            c >> 26 or ((a >> 22) & 511) * 32 < width or
            (d, e, f) != (0xA80D11, 3, 0x210)):
        raise RawShaderError('signed EXPAND base-only LINEAR clamp-edge zero-bias binding required')
    return dict(width=width, height=height, native_sampling_verified=False)


def analyze_max_reduce(pixel_code):
    proof = analyze_pixel_sample_dataflow(pixel_code, ui_operations=True,
        extended_math=True, loading_math=True, rectangle_mrt_profile=True)
    nodes, fetches = proof['nodes'], proof['fetches']
    if (proof['kills'] or set(proof['exports']) != {'0'} or len(fetches) != 4 or
            any(n[0] not in ('initial', 'sample', 'maximum', 'literal') for n in nodes)):
        raise RawShaderError('four unconditional samples and maximum-only colour0 required')
    offsets = []
    for fetch in fetches:
        offsets.append(tuple(fetch.get('texel_offset', (0, 0))))
        if (fetch['slot'] != 5 or fetch['destination_swizzle'] != [0, 1, 2, 7] or
                [tuple(nodes[n]) for n in fetch['coordinates']] != [('initial', 0, 0), ('initial', 0, 1)] or
                not fetch['computed_lod_requested'] or not fetch['fetch_valid_only']):
            raise RawShaderError('unmodified inherited-filter slot5 XY/RGB fetches required')
    if set(offsets) != {(x, y) for x in (-.5, .5) for y in (-.5, .5)}:
        raise RawShaderError('four distinct signed half-texel corners required')

    def leaves(i, channel):
        if i is None:
            raise RawShaderError('unwritten export')
        n = nodes[i]
        if n[0] == 'maximum':
            return leaves(n[1], channel) + leaves(n[2], channel)
        if n[0] == 'sample' and n[2] == channel:
            return [n[1]]
        raise RawShaderError('maximum reduction must consume each RGB sample lane')
    out = proof['exports']['0']
    if (any(sorted(leaves(out[c], c)) != [0, 1, 2, 3] for c in range(3)) or
            out[3] is None or tuple(nodes[out[3]]) != ('literal', 1)):
        raise RawShaderError('all four samples exactly once per RGB channel, alpha1 required')
    return proof


def linear_sample_reference(coordinates, offset, binding, read_pixel, *, weight_policy='ideal'):
    """Float32 bilinear policy at explicit coordinates; no raster/LOD proof.

    read_pixel supplies already untiled, endian-normalized binary16 channels.
    GPU filter-weight quantization can differ; do not loosen tests silently.
    adreno-floor8 is an independently measured native policy (8194 authored
    queries), NOT inferred from the Vulkan limit alone or an Xbox oracle.
    """
    if weight_policy not in ('ideal', 'adreno-floor8'):
        raise RawShaderError('explicit measured or ideal weight policy required')
    if (len(coordinates) != 2 or len(offset) != 2 or
            any(not math.isfinite(v) or f32(v) != v or abs(v) > 65536 for v in coordinates) or
            any(v not in (-.5, .5) for v in offset)):
        raise RawShaderError('finite float32 XY and signed half-texel offsets required')
    indices, weights = [], []
    for uv, off, size in zip(coordinates, offset, (binding['width'], binding['height'])):
        shifted = f32(uv + f32(f32(off + ROUNDING_OFFSET) / size))
        at = f32(f32(shifted * size) - .5)
        start = math.floor(at)
        indices.append([min(size-1, max(0, start+i)) for i in (0, 1)])
        weight = f32(at-start)
        weights.append(math.floor(weight*256)/256 if weight_policy=='adreno-floor8' else weight)
    pixels = [read_pixel(x, y) for y in indices[1] for x in indices[0]]
    if any(len(p) != 4 or any(not math.isfinite(v) or f32(v) != v for v in p) for p in pixels):
        raise RawShaderError('finite float32 source channels required')
    def mix(a, b, t):
        return f32(f32(a * f32(1-t)) + f32(b*t))
    return [mix(mix(pixels[0][c], pixels[1][c], weights[0]),
                mix(pixels[2][c], pixels[3][c], weights[0]), weights[1]) for c in range(4)]


def translate_max_reduce(vertex_code, pixel_code, fetch):
    binding = expanded16_binding(fetch)
    pixel = analyze_max_reduce(pixel_code)
    vertex_text, _, pair = translate_inline_pair(vertex_code, pixel_code,
        extended_profile=True, loading_profile=True, rectangle_list=True, offset_sample_profile=True)
    vertex = pair['vertex']
    nodes = vertex['nodes']
    def copied(i):
        if i is None:
            raise RawShaderError('unwritten vertex output')
        n = nodes[i]
        while n[0] == 'maximum' and n[1] == n[2]:
            n = nodes[n[1]]
        return tuple(n)
    if (set(vertex['exports']) != {'0', '62'} or len(vertex['fetches']) != 2 or
            vertex.get('texture_fetches') or pair['stride'] != 24 or
            [copied(i) for i in vertex['exports']['62']] !=
                [('fetch', 0, 0), ('fetch', 0, 1), ('fetch', 0, 2), ('literal', 1)] or
            [copied(i) for i in vertex['exports']['0'][:2]] != [('fetch', 1, 0), ('fetch', 1, 1)]):
        raise RawShaderError('unmodified original position and UV rectangle required')
    for f in vertex['fetches']:
        if f['fetch_slot'] != 95 or f['stride_words'] != 6 or tuple(nodes[f['index_expression']]) != ('initial', 0, 0):
            raise RawShaderError('original sequential inline vertices required')
    if [(f['format'], f['components'], f['offset_words']) for f in vertex['fetches']] != [(57, 3, 0), (37, 2, 4)]:
        raise RawShaderError('float3 position and float2 UV fetch layout required')
    lines = []
    for i, f in enumerate(pixel['fetches']):
        offset = f['texel_offset']
        lines += [f'precise vec2 offset{i}=vec2({offset[0]+ROUNDING_OFFSET},{offset[1]+ROUNDING_OFFSET})/vec2(textureSize(atlas0,0));',
                  f'precise vec2 shifted{i}=interpolator0+offset{i};',
                  f'vec4 sampled{i}=textureLod(atlas0,shifted{i},0.0);']
    done = {}
    def expr(i):
        if i in done:
            return done[i]
        n = pixel['nodes'][i]
        if n[0] == 'sample':
            value = f'sampled{n[1]}[{n[2]}]'
        elif n[0] == 'literal':
            value = repr(float(n[1]))
        elif n[0] == 'maximum':
            a, b = expr(n[1]), expr(n[2])
            value = f'n{i}'
            lines.append(f'precise float {value}=({a}>={b}?{a}:{b});')
        else:
            raise RawShaderError('unexpected live output dependency')
        done[i] = value
        return value
    outputs = [expr(i) for i in pixel['exports']['0']]
    lines.append('color=vec4('+','.join(outputs)+');')
    fragment = COMMON + '''layout(set=0,binding=1) uniform sampler2D atlas0;
layout(location=0) in vec2 interpolator0;
layout(location=0) out vec4 color;
void main(){
''' + '\n'.join(lines) + '\n}\n'
    return vertex_text, fragment, dict(vertex=vertex, pixel=pixel, binding=binding,
        texture_slots=[5], stride=24, xenia_offset_policy=True, base_lod_only=True,
        original_vertex_translation=True, native_game_pipeline=False,
        runtime_state_verified=False, gpu_executed=False, gameplay=False)
