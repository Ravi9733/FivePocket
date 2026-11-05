"""Offline one-texel R32 exposure translation; never native draw admission.

Analyze all original instructions before proving the vertex exports depend only
on position. A dead texture result is not replaced by a made-up sample. Its
absence from every export is the explicit reason it can be eliminated.
State, target format, ownership, finite arithmetic and GPU execution require
separate connected evidence. Transcendental approximation is not an Xbox oracle.
"""
from inline_shader_translation import COMMON
from vertex_arithmetic_dataflow import analyze_vertex_dataflow
from pixel_sample_dataflow import analyze_pixel_sample_dataflow
from raw_shader_analysis import RawShaderError


VERTEX_OPTIONS = dict(dot4=True, scalar_constants=True, loading_math=True,
                      scalar_gt_zero=True, base_texture_profile=True)
PIXEL_OPTIONS = dict(ui_operations=True, extended_math=True, loading_math=True,
                    rectangle_mrt_profile=True, tonemap_profile=True)


def r32_single_texel_binding(fetch):
    if (not isinstance(fetch, (list, tuple)) or len(fetch) != 6 or
            any(type(w) is not int or not 0 <= w <= 0xFFFFFFFF for w in fetch)):
        raise RawShaderError('six unsigned submitted fetch words required')
    a, b, c, d, e, f = fetch
    if ((a, c, d, e, f) != (0x80404956, 0, 0x16D1, 0, 0x210) or
            b & 4095 != 0xA4 or not 0 < (b & 0xFFFFF000) < 0x20000000):
        raise RawShaderError('one-texel numeric R32 POINT clamp-edge base-only binding required')
    return dict(width=1, height=1, point=True, numeric_r32=True,
                base=b & 0xFFFFF000, source_bytes=4096)


def validate_position_exports(proof):
    """Prove every exported lane; never assume a texture read is dead by name."""
    nodes = proof['nodes']
    if (set(proof['exports']) != {'62'} or len(proof['fetches']) != 2 or
            proof.get('predicate') is not None or proof.get('address_writes') or
            proof.get('dynamic_constants')):
        raise RawShaderError('position-only unpredicated exports required')
    reachable = set()
    def copied(i):
        if type(i) is not int or not 0 <= i < len(nodes):
            raise RawShaderError('defined position lane required')
        reachable.add(i)
        n = nodes[i]
        if n[0] == 'maximum' and n[1] == n[2] and n[1] < i:
            return copied(n[1])
        if n[0] not in ('fetch', 'literal'):
            raise RawShaderError('position export depends on additional arithmetic or texture')
        return tuple(n)
    if [copied(i) for i in proof['exports']['62']] != [
            ('fetch', 0, 0), ('fetch', 0, 1), ('fetch', 0, 2), ('literal', 1)]:
        raise RawShaderError('original float3 position and literal W required')
    for f in proof['fetches']:
        if (f['fetch_slot'] != 95 or f['stride_words'] != 6 or
                tuple(nodes[f['index_expression']]) != ('initial', 0, 0)):
            raise RawShaderError('sequential slot95 six-word original vertices required')
    if [(f['format'], f['components'], f['offset_words']) for f in proof['fetches']] != [
            (57, 3, 0), (37, 2, 4)]:
        raise RawShaderError('float3 position and float2 auxiliary input required')
    return dict(export_nodes=sorted(reachable),
                eliminated_texture_reads=len(proof.get('texture_fetches', [])),
                reason='no vertex texture result reaches any exported lane')


def validate_pixel_graph(proof):
    allowed = {'initial', 'constant', 'sample', 'absolute', 'negate', 'log2',
               'exp2', 'mul', 'add', 'maximum', 'minimum', 'greater'}
    if (proof['kills'] or set(proof['exports']) != {'0'} or
            len(proof['fetches']) != 1 or any(n[0] not in allowed for n in proof['nodes'])):
        raise RawShaderError('one unconditional sample and bounded exposure arithmetic required')
    nodes = proof['nodes']
    def copied(i):
        if type(i) is not int or not 0 <= i < len(nodes):
            raise RawShaderError('defined coordinate required')
        n = nodes[i]
        if n[0] == 'maximum' and n[1] == n[2] and n[1] < i:
            return copied(n[1])
        return tuple(n)
    f = proof['fetches'][0]
    if (f['slot'] != 0 or not f['computed_lod_requested'] or not f['fetch_valid_only'] or
            f.get('texel_offset', (0, 0)) != (0, 0) or
            [copied(i) for i in f['coordinates']] != [('constant', 255, 0)] * 2):
        raise RawShaderError('slot0 with dynamic c255.x coordinates required')
    # Check all computations, including overwritten ones: no interpolator or
    # undeclared channel can be accidentally introduced by a different shader.
    for i, n in enumerate(nodes):
        if n[0] in ('initial', 'constant'):
            continue
        if n[0] == 'sample':
            if tuple(n) != ('sample', 0, 0):
                raise RawShaderError('red sample only required')
            continue
        for child in n[1:]:
            if type(child) is not int or not 0 <= child < i or nodes[child][0] == 'initial':
                raise RawShaderError('acyclic defined pixel computation required')
    out = proof['exports']['0']
    if len(out) != 4 or any(type(i) is not int or not 0 <= i < len(nodes) or nodes[i][0] == 'initial' for i in out):
        raise RawShaderError('four defined output lanes required')


def exposure_fragment(proof):
    validate_pixel_graph(proof)
    nodes = proof['nodes']; lines = []; done = {}
    def expression(i):
        if i in done:
            return done[i]
        n = nodes[i]; op = n[0]
        if op == 'constant':
            value = f'uintBitsToFloat(readBE({1028+n[1]*4+n[2]}u))'
        elif op == 'sample':
            value = 'sampled.x'
        else:
            values = [expression(j) for j in n[1:]]
            a = values[0]
            if op in ('mul', 'add'):
                rhs = f'({a}{"*" if op == "mul" else "+"}{values[1]})'
            elif op in ('minimum', 'maximum', 'greater'):
                b = values[1]
                rhs = (f'({a}>{b}?1.0:0.0)' if op == 'greater' else
                       f'({a}{"<" if op == "minimum" else ">="}{b}?{a}:{b})')
            elif op == 'negate':
                rhs = f'(-{a})'
            elif op in ('absolute', 'log2', 'exp2'):
                rhs = f'{"abs" if op == "absolute" else op}({a})'
            else:
                raise RawShaderError('untranslated pixel operation')
            value = f'n{i}'; lines.append(f'precise float {value}={rhs};')
        done[i] = value
        return value
    # The fetch coordinate graph was proven to be exactly dynamic c255.x.
    outputs = [expression(i) for i in proof['exports']['0']]
    return COMMON + '''layout(set=0,binding=1) uniform sampler2D atlas0;
layout(location=0) out vec4 color;
void main(){
    float coordinate=uintBitsToFloat(readBE(2048u));
    vec4 sampled=textureLod(atlas0,vec2(coordinate),0.0);
''' + '\n'.join(lines) + '\ncolor=vec4(' + ','.join(outputs) + ');\n}\n'


POSITION_RECTANGLE = COMMON + '''layout(push_constant) uniform Viewport { vec4 scaleOffset; } viewport;
vec4 originalPosition(uint index){
    uint at=2052u+index*6u;
    return vec4(uintBitsToFloat(readBE(at)),uintBitsToFloat(readBE(at+1u)),
                uintBitsToFloat(readBE(at+2u)),1.0);
}
void main(){
    vec4 positions[3];
    uint base=(uint(gl_VertexIndex)/6u)*3u;
    for(uint i=0u;i<3u;++i) positions[i]=originalPosition(base+i);
    precise vec3 edgeSquared;
    for(int i=0;i<3;++i){
        precise vec2 edge=positions[(i+2)%3].xy-positions[(i+1)%3].xy;
        edgeSquared[i]=edge.x*edge.x+edge.y*edge.y;
    }
    int a=(edgeSquared.x>edgeSquared.y && edgeSquared.x>edgeSquared.z)?0:(edgeSquared.y>edgeSquared.z?1:2);
    int b=(a+1)%3,c=(a+2)%3;
    const uint corners[6]=uint[6](0u,1u,2u,2u,1u,3u);
    uint corner=corners[uint(gl_VertexIndex)%6u];
    int chosen=corner==0u?a:corner==1u?b:c;
    precise vec4 fourth=(positions[b]-positions[a])+positions[c];
    gl_Position=corner==3u?fourth:positions[chosen];
    gl_Position.xy=gl_Position.xy*viewport.scaleOffset.xy+viewport.scaleOffset.zw*gl_Position.w;
}
'''


def translate_exposure_scalar(vertex_code, pixel_code, fetch):
    binding = r32_single_texel_binding(fetch)
    vertex = analyze_vertex_dataflow(vertex_code, **VERTEX_OPTIONS)
    liveness = validate_position_exports(vertex)
    pixel = analyze_pixel_sample_dataflow(pixel_code, **PIXEL_OPTIONS)
    fragment = exposure_fragment(pixel)
    return POSITION_RECTANGLE, fragment, dict(vertex=vertex, pixel=pixel, binding=binding,
        vertex_liveness=liveness, texture_slots=[0], stride=24, output_components=4,
        original_vertex_translation=True, native_game_pipeline=False,
        runtime_state_verified=False, gpu_executed=False, gameplay=False)
