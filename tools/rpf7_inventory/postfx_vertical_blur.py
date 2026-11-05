"""Offline ABI43 PostFX vertical 5-tap blur (pass 82) translation; never admission.

The exact mirror of ABI42 (postfx_blur.py, left unchanged so its recorded
provenance stays valid): five slot5 samples on one COLUMN, once at the
interpolated UV and four times at uv.y + scale*k for runtime constants, each
output lane the same-lane weighted sum of all five samples in the
microcode's own order. Every tap is the driver's LINEAR sample of the binary16
source, bounded separately by the connected GPU test.
"""
from inline_shader_translation import translate_inline_pair
from pixel_sample_dataflow import analyze_pixel_sample_dataflow
from expanded16_max_reduce import expanded16_binding
from expanded16_copy import validate_rectangle_vertex
from inline_shader_translation import COMMON
from postfx_blur import PIXEL_OPTIONS, TAPS, _copied, _constant
from raw_shader_analysis import RawShaderError


def _tap_offset(nodes, i):
    """uv.y itself (None) or ('add', mul(scale, k), uv.y) -> (scale, k)."""
    n = _copied(nodes, i)
    if n == ('initial', 0, 1):
        return None
    if n[0] != 'add' or _copied(nodes, n[2]) != ('initial', 0, 1):
        raise RawShaderError('tap y must be uv.y or uv.y plus a constant product')
    m = _copied(nodes, n[1])
    if m[0] != 'mul':
        raise RawShaderError('tap offset must be scale*k')
    return _constant(nodes, m[1]), _constant(nodes, m[2])


def validate_vertical_blur_pixel(proof):
    nodes, fetches = proof['nodes'], proof['fetches']
    if proof['kills'] or set(proof['exports']) != {'0'} or len(proof['exports']['0']) != 4 or len(fetches) != TAPS:
        raise RawShaderError('five unconditional samples and one colour export required')
    offsets = []
    for f in fetches:
        if (f['slot'] != 5 or not f['computed_lod_requested'] or not f['fetch_valid_only'] or
                tuple(f.get('texel_offset', (0, 0))) != (0, 0) or len(f['coordinates']) != 2 or
                _copied(nodes, f['coordinates'][0]) != ('initial', 0, 0)):
            raise RawShaderError('slot5 unoffset computed-LOD samples on the interpolated column required')
        offsets.append(_tap_offset(nodes, f['coordinates'][1]))
    centre = [i for i, o in enumerate(offsets) if o is None]
    scaled = [o for o in offsets if o is not None]
    if len(centre) != 1 or len({s for s, _ in scaled}) != 1 or len({k for _, k in scaled}) != TAPS - 1:
        raise RawShaderError('one centre tap and four distinct offsets of one scale required')
    weights = {}
    for lane, root in enumerate(proof['exports']['0']):
        seen, i = [], root
        while True:
            n = _copied(nodes, i)
            if n[0] == 'add':
                term, i = n[1], n[2]
            elif n[0] == 'mul':
                term, i = i, None
            else:
                raise RawShaderError('weighted sum of samples required')
            t = _copied(nodes, term)
            if t[0] != 'mul':
                raise RawShaderError('each term must be sample*weight')
            sample, weight = _copied(nodes, t[1]), _constant(nodes, t[2])
            if sample[0] != 'sample' or sample[2] != lane:
                raise RawShaderError('each lane must weight the same lane of a sample')
            seen.append(sample[1])
            if weights.setdefault((lane, sample[1]), weight) != weight:
                raise RawShaderError('inconsistent tap weight')
            if i is None:
                break
        if sorted(seen) != list(range(TAPS)):
            raise RawShaderError('every lane must use each sample exactly once')
    for tap in range(TAPS):
        if len({weights[(lane, tap)] for lane in range(4)}) != 1:
            raise RawShaderError('one weight per tap across lanes required')
    for n in nodes:
        if n[0] not in ('initial', 'constant', 'sample', 'maximum', 'mul', 'add'):
            raise RawShaderError('operation outside the blur graph: ' + n[0])
    return dict(taps=TAPS, axis='y', centre_fetch=centre[0], scale=scaled[0][0],
                offsets=[None if o is None else o[1] for o in offsets],
                weights=[weights[(0, tap)] for tap in range(TAPS)])


def vertical_blur_fragment(pixel):
    """Node-by-node emission, identical in form to postfx_blur.blur_fragment."""
    validate_vertical_blur_pixel(pixel)
    nodes = pixel['nodes']; lines = []; done = {}
    def expression(i):
        if i in done:
            return done[i]
        n = nodes[i]; op = n[0]
        if op == 'initial':
            value = {(0, 0): 'r0x', (0, 1): 'r0y'}[n[1:]]
        elif op == 'constant':
            value = f'uintBitsToFloat(readBE({1028 + n[1] * 4 + n[2]}u))'
        elif op == 'sample':
            value = f's{n[1]}.' + 'xyzw'[n[2]]
        else:
            a, b = [expression(j) for j in n[1:]]
            if op == 'mul':
                rhs = f'(({a}==0.0 || {b}==0.0)?0.0:({a}*{b}))'
            elif op == 'add':
                rhs = f'({a}+{b})'
            elif op == 'maximum':
                rhs = f'({a}>={b}?{a}:{b})'
            else:
                raise RawShaderError('untranslated blur operation ' + op)
            value = f'n{i}'; lines.append(f'precise float {value}={rhs};')
        done[i] = value
        return value
    coordinates = [(expression(f['coordinates'][0]), expression(f['coordinates'][1])) for f in pixel['fetches']]
    pre = list(lines); lines.clear()
    samples = [f'vec4 s{i}=textureLod(atlas0,vec2({x},{y}),0.0);' for i, (x, y) in enumerate(coordinates)]
    outputs = [expression(i) for i in pixel['exports']['0']]
    return COMMON + '''layout(set=0,binding=1) uniform sampler2D atlas0;
layout(location=0) in vec2 interpolator0;
layout(location=0) out vec4 color;
void main(){
    float r0x=interpolator0.x,r0y=interpolator0.y;
''' + '\n'.join(pre + samples + lines) + '\ncolor=vec4(' + ','.join(outputs) + ');\n}\n'


def translate_postfx_vertical_blur(vertex_code, pixel_code, fetch):
    binding = expanded16_binding(fetch)
    pixel = analyze_pixel_sample_dataflow(pixel_code, **PIXEL_OPTIONS)
    contract = validate_vertical_blur_pixel(pixel)
    vertex_text, _, pair = translate_inline_pair(vertex_code, pixel_code,
        extended_profile=True, loading_profile=True, rectangle_list=True, offset_sample_profile=True)
    validate_rectangle_vertex(pair['vertex'], pair['stride'])
    return vertex_text, vertical_blur_fragment(pixel), dict(vertex=pair['vertex'], pixel=pixel,
        pixel_contract=contract, binding=binding, texture_slots=[5], stride=24, abi=43, base_lod_only=True,
        original_vertex_translation=True, native_game_pipeline=False,
        runtime_state_verified=False, gpu_executed=False, gameplay=False)
