"""Offline ABI40 half-resolution bright-pass translation; never draw admission.

The original vertex shader copies float3 position and float2 UV, samples the
1x1 EXPAND exposure texel (slot17) at the dynamic constant c251.x and exports
(u, v, s, 1/s). The pixel shader samples that texel again (slot1) at a
coordinate its own graph proves is 0 or 1, samples the 640x360 EXPAND scene
(slot10) at the interpolated UV and exports bright-pass RGB with alpha 0.

The vertex texel sample depends on no per-vertex input: all three vertices
evaluate the same value, so its interpolation is that value. The fragment
stage evaluates the SAME texture fetch instead of interpolating a constant.
The unused 1/s lane is proven absent from every pixel dependency. State,
target format, ownership and GPU execution require separate connected
evidence. Arithmetic follows Xenia's legacy multiply (0 * anything = 0).
"""
from inline_shader_translation import COMMON
from vertex_arithmetic_dataflow import analyze_vertex_dataflow
from pixel_sample_dataflow import analyze_pixel_sample_dataflow
from raw_shader_analysis import RawShaderError


VERTEX_OPTIONS = dict(dot4=True, scalar_constants=True, loading_math=True,
                      scalar_gt_zero=True, base_texture_profile=True)
PIXEL_OPTIONS = dict(ui_operations=True, extended_math=True, loading_math=True,
                     bright_pass_profile=True)
TEXTURE_SLOTS = (1, 10, 17)
PIXEL_OPERATIONS = {'initial', 'constant', 'literal', 'sample', 'absolute', 'negate', 'greater',
                    'mul', 'add', 'maximum', 'saturate', 'reciprocal'}


def _words(fetch):
    if (not isinstance(fetch, (list, tuple)) or len(fetch) != 6 or
            any(type(w) is not int or not 0 <= w <= 0xFFFFFFFF for w in fetch)):
        raise RawShaderError('six unsigned submitted fetch words required')
    return fetch


def exposure_texel_binding(fetch):
    """1x1 EXPAND (format29) POINT clamp-edge base-only texel, as copied."""
    a, b, c, d, e, f = _words(fetch)
    if ((a, c, d, e, f) != (0x80404956, 0, 0xD11, 0, 0x210) or b & 4095 != 0x5D or
            not 0 < (b & 0xFFFFF000) < 0x20000000):
        raise RawShaderError('one-texel EXPAND POINT clamp-edge base-only binding required')
    return dict(width=1, height=1, point=True, expand16=True, base=b & 0xFFFFF000, source_bytes=8192)


def half_scene_binding(fetch):
    """640x360 EXPAND (format29) bilinear clamp-edge base-only scene."""
    a, b, c, d, e, f = _words(fetch)
    if ((a, c, d, e, f) != (0x85004956, 0x2CE27F, 0x280D11, 3, 0x210) or b & 4095 != 0x5D or
            not 0 < (b & 0xFFFFF000) < 0x20000000):
        raise RawShaderError('640x360 EXPAND bilinear clamp-edge base-only binding required')
    return dict(width=640, height=360, point=False, bilinear=True, expand16=True,
                base=b & 0xFFFFF000)


def _copied(nodes, i):
    """Follow MAX(x,x) register copies; any other arithmetic stays visible."""
    if type(i) is not int or not 0 <= i < len(nodes):
        raise RawShaderError('defined lane required')
    n = nodes[i]
    while n[0] == 'maximum' and n[1] == n[2]:
        if not n[1] < i:
            raise RawShaderError('acyclic copy required')
        i = n[1]; n = nodes[i]
    return i, tuple(n)


def validate_vertex(proof):
    """Exact position/UV copies plus the constant-coordinate texel lanes."""
    nodes = proof['nodes']
    if (set(proof['exports']) != {'0', '62'} or len(proof['fetches']) != 2 or
            proof.get('predicate') is not None or proof.get('address_writes') or
            proof.get('dynamic_constants') or len(proof.get('texture_fetches', [])) != 1):
        raise RawShaderError('position and one parameter export, one texel read, no predicate/addressing required')
    for f in proof['fetches']:
        if (f['fetch_slot'] != 95 or f['stride_words'] != 6 or
                tuple(nodes[f['index_expression']]) != ('initial', 0, 0)):
            raise RawShaderError('sequential slot95 six-word original vertices required')
    if [(f['format'], f['components'], f['offset_words']) for f in proof['fetches']] != [(57, 3, 0), (37, 2, 4)]:
        raise RawShaderError('float3 position and float2 UV inputs required')
    if [_copied(nodes, i)[1] for i in proof['exports']['62']] != [
            ('fetch', 0, 0), ('fetch', 0, 1), ('fetch', 0, 2), ('literal', 1)]:
        raise RawShaderError('original float3 position and literal W required')
    texture = proof['texture_fetches'][0]
    if (texture['slot'] != 17 or not texture.get('explicit_base_lod') or
            [_copied(nodes, i)[1] for i in texture['coordinates']] != [('constant', 251, 0)] * 2):
        raise RawShaderError('slot17 explicit-base texel at dynamic c251.x required')
    lanes = [_copied(nodes, i) for i in proof['exports']['0']]
    sample = ('texture_sample', 0, 0)
    if ([n for _, n in lanes[:3]] != [('fetch', 1, 0), ('fetch', 1, 1), sample] or
            lanes[3][1][0] != 'reciprocal' or _copied(nodes, lanes[3][1][1])[1] != sample):
        raise RawShaderError('parameter must be (u, v, texel.x, 1/texel.x)')
    return dict(texel_coordinate=('constant', 251, 0), texel_component=0,
                reciprocal_lane=3, per_vertex_inputs_in_texel=False)


def validate_pixel(proof):
    """Bright-pass graph: two unconditional samples and bounded arithmetic."""
    nodes = proof['nodes']
    if (proof['kills'] or set(proof['exports']) != {'0'} or len(proof['fetches']) != 2 or
            any(n[0] not in PIXEL_OPERATIONS for n in nodes)):
        raise RawShaderError('two unconditional samples and bounded bright-pass arithmetic required')
    texel, scene = proof['fetches']
    degenerate = ('greater', 'negate(absolute(r0.x))', 0)
    for f, slot in ((texel, 1), (scene, 10)):
        if (f['slot'] != slot or not f['computed_lod_requested'] or not f['fetch_valid_only'] or
                tuple(f.get('texel_offset', (0, 0))) != (0, 0)):
            raise RawShaderError('slot1/slot10 unoffset computed-LOD samples required')
    # Slot1 coordinates: SETGTs(-|r0.x|) is 0 for every finite or NaN input
    # and 0 or 1 in general; either way a clamped 1x1 POINT sample is the texel.
    for i in texel['coordinates']:
        n = nodes[i]
        if (n[0] != 'greater' or nodes[n[2]] != ('literal', 0) or nodes[n[1]][0] != 'negate' or
                nodes[nodes[n[1]][1]][0] != 'absolute' or
                nodes[nodes[nodes[n[1]][1]][1]] != ('initial', 0, 0)):
            raise RawShaderError('slot1 degenerate coordinate ' + repr(degenerate) + ' required')
    if [tuple(nodes[i]) for i in scene['coordinates']] != [('initial', 0, 0), ('initial', 0, 1)]:
        raise RawShaderError('slot10 at interpolated UV required')
    used, initial, samples = set(), set(), set()
    def visit(i):
        if i in used:
            return
        used.add(i); n = nodes[i]
        if n[0] == 'initial':
            initial.add(n[1:]); return
        if n[0] == 'sample':
            samples.add(n[1:]); return
        if n[0] in ('constant', 'literal'):
            return
        for child in n[1:]:
            if type(child) is not int or not 0 <= child < i:
                raise RawShaderError('acyclic defined pixel computation required')
            visit(child)
    for f in proof['fetches']:
        for i in f['coordinates']:
            visit(i)
    out = proof['exports']['0']
    if len(out) != 4 or any(type(i) is not int or not 0 <= i < len(nodes) for i in out):
        raise RawShaderError('four defined output lanes required')
    for i in out:
        visit(i)
    if initial != {(0, 0), (0, 1), (0, 2)}:
        raise RawShaderError('only interpolated u, v and texel lanes may be read')
    if samples != {(0, 0), (1, 0), (1, 1), (1, 2)}:
        raise RawShaderError('texel red and scene RGB only')
    if tuple(nodes[out[3]]) != ('literal', 0):
        raise RawShaderError('literal zero alpha required')
    return dict(initial_lanes=sorted(initial), sample_lanes=sorted(samples),
                reciprocal_parameter_consumed=False)


VERTEX = COMMON + '''layout(push_constant) uniform Viewport {vec4 scaleOffset;} viewport;
layout(location=0) out vec2 interpolatorXY;
void originalVertex(uint index,out vec4 position,out vec2 xy){
position=vec4(uintBitsToFloat(0x7FC00000u));xy=vec2(uintBitsToFloat(0x7FC00000u));
precise float fetch0_0=uintBitsToFloat(readBE(2052u+index*6u+0u));
if(isnan(fetch0_0) || isinf(fetch0_0)) return;
precise float fetch0_1=uintBitsToFloat(readBE(2052u+index*6u+1u));
if(isnan(fetch0_1) || isinf(fetch0_1)) return;
precise float fetch0_2=uintBitsToFloat(readBE(2052u+index*6u+2u));
if(isnan(fetch0_2) || isinf(fetch0_2)) return;
precise float fetch1_0=uintBitsToFloat(readBE(2052u+index*6u+4u));
if(isnan(fetch1_0) || isinf(fetch1_0)) return;
precise float fetch1_1=uintBitsToFloat(readBE(2052u+index*6u+5u));
if(isnan(fetch1_1) || isinf(fetch1_1)) return;
position=vec4(fetch0_0,fetch0_1,fetch0_2,1.0);
xy=vec2(fetch1_0,fetch1_1);
}
void main(){
vec4 positions[3];vec2 parameters[3];
for(uint i=0u;i<3u;++i)originalVertex(i,positions[i],parameters[i]);
precise vec3 edges;
for(int i=0;i<3;++i){precise vec2 e=positions[(i+2)%3].xy-positions[(i+1)%3].xy;edges[i]=e.x*e.x+e.y*e.y;}
int a=(edges.x>edges.y && edges.x>edges.z)?0:(edges.y>edges.z?1:2);
int b=(a+1)%3,c=(a+2)%3;
const uint corners[6]=uint[6](0u,1u,2u,2u,1u,3u);
uint corner=corners[uint(gl_VertexIndex)%6u];int chosen=corner==0u?a:corner==1u?b:c;
precise vec4 syntheticPosition=(positions[b]-positions[a])+positions[c];
precise vec2 syntheticParameter=(parameters[b]-parameters[a])+parameters[c];
gl_Position=corner==3u?syntheticPosition:positions[chosen];
interpolatorXY=corner==3u?syntheticParameter:parameters[chosen];
gl_Position.xy=gl_Position.xy*viewport.scaleOffset.xy+viewport.scaleOffset.zw*gl_Position.w;
}
'''


def bright_pass_fragment(pixel):
    validate_pixel(pixel)
    nodes = pixel['nodes']; lines = []; done = {}
    def expression(i):
        if i in done:
            return done[i]
        n = nodes[i]; op = n[0]
        if op == 'initial':
            value = {(0, 0): 'r0x', (0, 1): 'r0y', (0, 2): 'r0z'}[n[1:]]
        elif op == 'constant':
            value = f'uintBitsToFloat(readBE({1028 + n[1] * 4 + n[2]}u))'
        elif op == 'literal':
            value = f'{float(n[1])}'
        elif op == 'sample':
            value = ('texel' if n[1] == 0 else 'scene') + '.' + 'xyzw'[n[2]]
        else:
            args = [expression(j) for j in n[1:]]
            a = args[0]
            if op == 'mul':
                rhs = f'(({a}==0.0 || {args[1]}==0.0)?0.0:({a}*{args[1]}))'
            elif op == 'add':
                rhs = f'({a}+{args[1]})'
            elif op == 'maximum':
                rhs = f'({a}>={args[1]}?{a}:{args[1]})'
            elif op == 'greater':
                rhs = f'({a}>{args[1]}?1.0:0.0)'
            elif op == 'negate':
                rhs = f'(-{a})'
            elif op == 'absolute':
                rhs = f'abs({a})'
            elif op == 'saturate':
                rhs = f'clamp({a},0.0,1.0)'
            elif op == 'reciprocal':
                rhs = f'({a}==0.0?uintBitsToFloat((floatBitsToUint({a})&0x80000000u)|0x7F800000u):(1.0/{a}))'
            else:
                raise RawShaderError('untranslated bright-pass operation ' + op)
            value = f'n{i}'; lines.append(f'precise float {value}={rhs};')
        done[i] = value
        return value
    texel_fetch, scene_fetch = pixel['fetches']
    coordinate = expression(texel_fetch['coordinates'][0])
    coordinate_y = expression(texel_fetch['coordinates'][1])
    pre = list(lines); lines.clear()
    outputs = [expression(i) for i in pixel['exports']['0']]
    return COMMON + '''layout(set=0,binding=1) uniform sampler2D atlas1;
layout(set=0,binding=2) uniform sampler2D atlas10;
layout(set=0,binding=3) uniform sampler2D atlas17;
layout(location=0) in vec2 interpolatorXY;
layout(location=0) out vec4 color;
void main(){
    float r0x=interpolatorXY.x,r0y=interpolatorXY.y;
    // Vertex texel lane: the same slot17 fetch at c251.x for every vertex.
    float r0z=textureLod(atlas17,vec2(uintBitsToFloat(readBE(1008u))),0.0).x;
''' + '\n'.join(pre) + f'''
    vec4 texel=textureLod(atlas1,vec2({coordinate},{coordinate_y}),0.0);
    vec4 scene=textureLod(atlas10,vec2(r0x,r0y),0.0);
''' + '\n'.join(lines) + '\ncolor=vec4(' + ','.join(outputs) + ');\n}\n'


def translate_bright_pass(vertex_code, pixel_code, vertex_fetch, texel_fetch, scene_fetch):
    vertex_texel = exposure_texel_binding(vertex_fetch)
    texel = exposure_texel_binding(texel_fetch)
    if vertex_texel != texel:
        raise RawShaderError('vertex and pixel exposure texel bindings must be identical')
    scene = half_scene_binding(scene_fetch)
    vertex = analyze_vertex_dataflow(vertex_code, **VERTEX_OPTIONS)
    vertex_contract = validate_vertex(vertex)
    pixel = analyze_pixel_sample_dataflow(pixel_code, **PIXEL_OPTIONS)
    pixel_contract = validate_pixel(pixel)
    fragment = bright_pass_fragment(pixel)
    return VERTEX, fragment, dict(vertex=vertex, pixel=pixel, vertex_contract=vertex_contract,
        pixel_contract=pixel_contract, bindings=dict(slot1=texel, slot10=scene, slot17=vertex_texel),
        texture_slots=list(TEXTURE_SLOTS), stride=24, output_components=4, abi=40,
        vertex_texel_evaluated_in_fragment=True, original_vertex_translation=True,
        native_game_pipeline=False, runtime_state_verified=False, gpu_executed=False, gameplay=False)
