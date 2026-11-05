"""Offline ABI44 PostFX pass83 (additive upsample) translation; never admission.

The original pixel shader samples slot5 once at the interpolated UV and exports
the sample's RGB unchanged with a literal 0 alpha. The destination blend
(RB_BLENDCONTROL ONE/ONE add, all channels) is NOT part of the shader: the
native consumer adds this output to the known destination words. The vertex
stage is the shared post rectangle; the LINEAR sample of the binary16 source
is bounded separately by the connected GPU test.
"""
from inline_shader_translation import translate_inline_pair, COMMON
from pixel_sample_dataflow import analyze_pixel_sample_dataflow
from expanded16_max_reduce import expanded16_binding
from expanded16_copy import validate_rectangle_vertex
from postfx_blur import _copied
from raw_shader_analysis import RawShaderError

PIXEL_OPTIONS = dict(ui_operations=True, extended_math=True, loading_math=True)


def validate_upsample_pixel(proof):
    nodes = proof['nodes']
    if proof['kills'] or set(proof['exports']) != {'0'} or len(proof['exports']['0']) != 4 or len(proof['fetches']) != 1:
        raise RawShaderError('one unconditional sample and one colour export required')
    f = proof['fetches'][0]
    if (f['slot'] != 5 or not f['computed_lod_requested'] or not f['fetch_valid_only'] or
            tuple(f.get('texel_offset', (0, 0))) != (0, 0) or
            [_copied(nodes, i) for i in f['coordinates']] != [('initial', 0, 0), ('initial', 0, 1)]):
        raise RawShaderError('slot5 unoffset computed-LOD sample at the interpolated UV required')
    lanes = [_copied(nodes, i) for i in proof['exports']['0']]
    if lanes[:3] != [('sample', 0, c) for c in range(3)] or lanes[3] != ('literal', 0):
        raise RawShaderError('exported lanes must be the unmodified sample RGB and a literal 0 alpha')
    for n in nodes:
        if n[0] not in ('initial', 'constant', 'literal', 'sample', 'maximum'):
            raise RawShaderError('no arithmetic beyond register moves is admitted')
        if n[0] == 'maximum' and n[1] != n[2]:
            raise RawShaderError('MAX of distinct values is arithmetic, not a copy')
        if n[0] == 'literal' and n[1] != 0:
            raise RawShaderError('only the literal 0 alpha is admitted')
    return dict(copy_rgb=True, alpha=0.0, slot=5)


FRAGMENT = COMMON + '''layout(set=0,binding=1) uniform sampler2D atlas0;
layout(location=0) in vec2 interpolator0;
layout(location=0) out vec4 color;
void main(){
    color=vec4(textureLod(atlas0,interpolator0,0.0).rgb,0.0);
}
'''


def translate_postfx_upsample(vertex_code, pixel_code, fetch):
    binding = expanded16_binding(fetch)
    pixel = analyze_pixel_sample_dataflow(pixel_code, **PIXEL_OPTIONS)
    contract = validate_upsample_pixel(pixel)
    vertex_text, _, pair = translate_inline_pair(vertex_code, pixel_code,
        extended_profile=True, loading_profile=True, rectangle_list=True, offset_sample_profile=True)
    validate_rectangle_vertex(pair['vertex'], pair['stride'])
    return vertex_text, FRAGMENT, dict(vertex=pair['vertex'], pixel=pixel, pixel_contract=contract,
        binding=binding, texture_slots=[5], stride=24, abi=44, base_lod_only=True, destination_blend='add-native',
        original_vertex_translation=True, native_game_pipeline=False,
        runtime_state_verified=False, gpu_executed=False, gameplay=False)
