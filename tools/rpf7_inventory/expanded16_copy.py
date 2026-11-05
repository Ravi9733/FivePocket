"""Offline ABI41 EXPAND LINEAR copy (bloom downsample) translation; never admission.

The original pixel shader samples slot5 once at the interpolated UV and exports
the four sampled lanes unchanged (MAX(x,x) register moves). The vertex stage is
the shared post-processing rectangle already validated for the max band.
Filtering is the driver's LINEAR sample of the binary16 source; its rounding
is bounded separately by the connected GPU test, never assumed exact here.
"""
from inline_shader_translation import translate_inline_pair, COMMON
from pixel_sample_dataflow import analyze_pixel_sample_dataflow
from expanded16_max_reduce import expanded16_binding
from raw_shader_analysis import RawShaderError

PIXEL_OPTIONS = dict(ui_operations=True, extended_math=True, loading_math=True)


def _copied(nodes, i):
    if type(i) is not int or not 0 <= i < len(nodes):
        raise RawShaderError('defined lane required')
    n = nodes[i]
    while n[0] == 'maximum' and n[1] == n[2]:
        if not n[1] < i:
            raise RawShaderError('acyclic copy required')
        i = n[1]; n = nodes[i]
    return tuple(n)


def validate_copy_pixel(proof):
    nodes = proof['nodes']
    if proof['kills'] or set(proof['exports']) != {'0'} or len(proof['fetches']) != 1:
        raise RawShaderError('one unconditional sample and one colour export required')
    f = proof['fetches'][0]
    if (f['slot'] != 5 or not f['computed_lod_requested'] or not f['fetch_valid_only'] or
            tuple(f.get('texel_offset', (0, 0))) != (0, 0) or
            [tuple(nodes[i]) for i in f['coordinates']] != [('initial', 0, 0), ('initial', 0, 1)]):
        raise RawShaderError('slot5 unoffset computed-LOD sample at interpolated UV required')
    if [_copied(nodes, i) for i in proof['exports']['0']] != [('sample', 0, c) for c in range(4)]:
        raise RawShaderError('exported lanes must be the unmodified sample')
    for n in nodes:
        if n[0] not in ('initial', 'constant', 'literal', 'sample', 'maximum'):
            raise RawShaderError('no arithmetic beyond register moves is admitted')
        if n[0] == 'maximum' and n[1] != n[2]:
            raise RawShaderError('MAX of distinct values is arithmetic, not a copy')
    return dict(copy=True, slot=5)


def validate_rectangle_vertex(vertex, stride):
    nodes = vertex['nodes']
    def copied(i):
        if i is None:
            raise RawShaderError('unwritten vertex output')
        return _copied(nodes, i)
    if (set(vertex['exports']) != {'0', '62'} or len(vertex['fetches']) != 2 or
            vertex.get('texture_fetches') or stride != 24 or
            [copied(i) for i in vertex['exports']['62']] !=
                [('fetch', 0, 0), ('fetch', 0, 1), ('fetch', 0, 2), ('literal', 1)] or
            [copied(i) for i in vertex['exports']['0'][:2]] != [('fetch', 1, 0), ('fetch', 1, 1)]):
        raise RawShaderError('unmodified original position and UV rectangle required')
    for f in vertex['fetches']:
        if f['fetch_slot'] != 95 or f['stride_words'] != 6 or tuple(nodes[f['index_expression']]) != ('initial', 0, 0):
            raise RawShaderError('original sequential inline vertices required')
    if [(f['format'], f['components'], f['offset_words']) for f in vertex['fetches']] != [(57, 3, 0), (37, 2, 4)]:
        raise RawShaderError('float3 position and float2 UV fetch layout required')


FRAGMENT = COMMON + '''layout(set=0,binding=1) uniform sampler2D atlas0;
layout(location=0) in vec2 interpolator0;
layout(location=0) out vec4 color;
void main(){
    color=textureLod(atlas0,interpolator0,0.0);
}
'''


def translate_expanded16_copy(vertex_code, pixel_code, fetch):
    binding = expanded16_binding(fetch)
    pixel = analyze_pixel_sample_dataflow(pixel_code, **PIXEL_OPTIONS)
    contract = validate_copy_pixel(pixel)
    vertex_text, _, pair = translate_inline_pair(vertex_code, pixel_code,
        extended_profile=True, loading_profile=True, rectangle_list=True, offset_sample_profile=True)
    validate_rectangle_vertex(pair['vertex'], pair['stride'])
    return vertex_text, FRAGMENT, dict(vertex=pair['vertex'], pixel=pixel, pixel_contract=contract,
        binding=binding, texture_slots=[5], stride=24, abi=41, base_lod_only=True,
        original_vertex_translation=True, native_game_pipeline=False,
        runtime_state_verified=False, gpu_executed=False, gameplay=False)
