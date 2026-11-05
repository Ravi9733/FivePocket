"""Separate ABI34: original direct-triangle shaders plus alpha<=0 stencil kill.

This is OFFLINE translation, not execution admission. The native consumer must
prove RB_COLORCONTROL low bits 0xB, RB_ALPHA_REF=0, colour/depth writes disabled,
stencil ALWAYS/REPLACE, current attachments, texture storage and sampler state.
Xenia's spirv_shader_translator_rb.cc performs this alpha test independently of
colour write masks; a fragment failing it must not update stencil.
"""
from inline_shader_translation import translate_inline_pair
from raw_shader_analysis import RawShaderError


def translate_sampled_stencil(vertex_code, pixel_code):
    vertex, pixel, proof = translate_inline_pair(
        vertex_code, pixel_code, extended_profile=True, direct_triangles=True)
    ps = proof['pixel']
    nodes = ps['nodes']
    output = ps['exports'].get('0')
    fetches = ps['fetches']
    if (proof['abi_version'] != 5 or proof['stride'] != 36 or
            proof['interpolators'] != 1 or proof['texture_slots'] != [1] or
            len(fetches) != 1 or set(ps['exports']) != {'0'} or not output or
            any(nodes[i] != ('literal', 0) for i in output[:3])):
        raise RawShaderError('sampled stencil requires single slot1 alpha-only export')
    alpha = nodes[output[3]]
    if alpha[0] != 'regular_copy' or nodes[alpha[1]] != ('sample', 0, 3):
        raise RawShaderError('sampled stencil requires unmodified sampled alpha')
    # Reject an override even if a future extended translator admits one.
    fetch = fetches[0]
    if (not fetch.get('computed_lod_requested') or not fetch.get('fetch_valid_only') or
            fetch['destination_swizzle'] != [3, 7, 7, 7] or
            [nodes[i] for i in fetch['coordinates']] != [('initial', 0, 0), ('initial', 0, 1)]):
        raise RawShaderError('sampled stencil requires inherited normalized UV sampling')
    if not pixel.endswith('\n}\n') or 'discard' in pixel or 'early_fragment_tests' in pixel:
        raise RawShaderError('unexpected sampled stencil fragment control flow')
    # Ordered <= is intentional: !(a > 0) would incorrectly pass NaN.
    pixel = pixel[:-3] + '\n    if (!(color.a <= 0.0)) discard;\n}\n'
    proof = dict(proof, abi_version=34, sampled_stencil=True,
                 alpha_test_function=3, alpha_reference_bits=0,
                 native_admission=False)
    return vertex, pixel, proof
