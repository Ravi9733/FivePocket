"""Authored graphs and fetch words, never embedded game shaders."""
import copy
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from bright_pass import (exposure_texel_binding, half_scene_binding, validate_vertex,
    validate_pixel, bright_pass_fragment)
from pixel_sample_dataflow import analyze_pixel_sample_dataflow
from raw_shader_analysis import RawShaderError

TEXEL = (0x80404956, 0x1F06305D, 0, 0xD11, 0, 0x210)
SCENE = (0x85004956, 0x0623A05D, 0x2CE27F, 0x280D11, 3, 0x210)


def vertex_proof():
    nodes = [('initial', r, c) for r in range(64) for c in range(4)]
    def add(n):
        nodes.append(n); return len(nodes) - 1
    px, py, pz, w = add(('fetch', 0, 0)), add(('fetch', 0, 1)), add(('fetch', 0, 2)), add(('literal', 1))
    u, v = add(('fetch', 1, 0)), add(('fetch', 1, 1))
    c = add(('constant', 251, 0)); cc = add(('maximum', c, c))
    position = [add(('maximum', i, i)) for i in (px, py, pz, w)]
    s = add(('texture_sample', 0, 0)); r = add(('reciprocal', s))
    parameter = [add(('maximum', i, i)) for i in (u, v, s, r)]
    return dict(nodes=nodes, exports={'0': parameter, '62': position}, predicate=None,
        address_writes=[], dynamic_constants=False,
        fetches=[dict(index_expression=0, fetch_slot=95, stride_words=6, format=57, components=3, offset_words=0),
                 dict(index_expression=0, fetch_slot=95, stride_words=6, format=37, components=2, offset_words=4)],
        texture_fetches=[dict(slot=17, coordinates=[cc, cc], explicit_base_lod=True)])


def pixel_proof():
    nodes = [('initial', r, c) for r in range(64) for c in range(4)]
    def add(n):
        nodes.append(n); return len(nodes) - 1
    a = add(('absolute', 0)); n = add(('negate', a)); z = add(('literal', 0)); g = add(('greater', n, z))
    t = add(('sample', 0, 0)); rgb = [add(('sample', 1, i)) for i in range(3)]
    k = add(('constant', 18, 1)); e = add(('initial', 0, 2)) if False else 2
    scaled = [add(('mul', x, k)) for x in rgb]
    lit = [add(('mul', x, t)) for x in scaled]
    l0 = add(('mul', lit[0], add(('constant', 136, 3))))
    l1 = add(('add', l0, add(('mul', lit[1], add(('constant', 136, 3))))))
    m = add(('maximum', l1, add(('constant', 255, 0))))
    d = add(('add', add(('negate', add(('constant', 135, 0)))), m))
    sat = add(('saturate', add(('mul', add(('constant', 135, 2)), d))))
    f = add(('mul', add(('mul', sat, m)), add(('reciprocal', m))))
    out = [add(('mul', add(('mul', x, f)), e)) for x in scaled]
    return dict(nodes=nodes, exports={'0': out + [z]}, kills=[],
        fetches=[dict(slot=1, coordinates=[g, g], computed_lod_requested=True, fetch_valid_only=True),
                 dict(slot=10, coordinates=[0, 1], computed_lod_requested=True, fetch_valid_only=True)])


class BrightPassTests(unittest.TestCase):
    def test_bindings_accept_exact_and_reject_near_misses(self):
        self.assertEqual(exposure_texel_binding(TEXEL)['source_bytes'], 8192)
        self.assertTrue(half_scene_binding(SCENE)['bilinear'])
        for words, bind in ((TEXEL, exposure_texel_binding), (SCENE, half_scene_binding)):
            for index, bit in ((0, 2), (0, 26), (1, 0), (1, 5), (2, 0), (3, 19), (3, 21), (4, 0), (5, 4)):
                q = list(words); q[index] ^= 1 << bit
                with self.assertRaises(RawShaderError): bind(q)
            for address in (0, 0x20000000, 0xA0000000):
                q = list(words); q[1] = address | 0x5D
                with self.assertRaises(RawShaderError): bind(q)
        with self.assertRaises(RawShaderError): exposure_texel_binding(SCENE)
        with self.assertRaises(RawShaderError): half_scene_binding(TEXEL)

    def test_vertex_contract_accepts_constant_texel_and_refuses_changes(self):
        contract = validate_vertex(vertex_proof())
        self.assertFalse(contract['per_vertex_inputs_in_texel'])
        changes = []
        p = vertex_proof(); p['texture_fetches'][0]['coordinates'][0] = 256; changes.append(p)  # per-vertex coordinate
        p = vertex_proof(); p['texture_fetches'][0]['slot'] = 16; changes.append(p)
        p = vertex_proof(); p['texture_fetches'][0]['explicit_base_lod'] = False; changes.append(p)
        p = vertex_proof(); p['texture_fetches'].append(copy.deepcopy(p['texture_fetches'][0])); changes.append(p)
        p = vertex_proof(); p['exports']['1'] = p['exports']['0']; changes.append(p)
        p = vertex_proof(); p['exports']['62'][3] = p['exports']['0'][2]; changes.append(p)  # texel in position
        p = vertex_proof(); p['exports']['0'][3] = p['exports']['0'][2]; changes.append(p)  # no reciprocal
        p = vertex_proof(); p['fetches'][1]['format'] = 38; changes.append(p)
        p = vertex_proof(); p['fetches'][0]['stride_words'] = 7; changes.append(p)
        p = vertex_proof(); p['predicate'] = 1; changes.append(p)
        p = vertex_proof(); p['dynamic_constants'] = True; changes.append(p)
        for q in changes:
            with self.assertRaises(RawShaderError): validate_vertex(q)

    def test_pixel_contract_and_side_effects(self):
        contract = validate_pixel(pixel_proof())
        self.assertEqual(contract['initial_lanes'], [(0, 0), (0, 1), (0, 2)])
        self.assertFalse(contract['reciprocal_parameter_consumed'])
        changes = []
        p = pixel_proof(); p['kills'] = [{}]; changes.append(p)
        p = pixel_proof(); p['fetches'].append(copy.deepcopy(p['fetches'][1])); changes.append(p)
        p = pixel_proof(); p['fetches'][0]['slot'] = 10; p['fetches'][1]['slot'] = 1; changes.append(p)
        p = pixel_proof(); p['fetches'][1]['texel_offset'] = (0.5, 0); changes.append(p)
        p = pixel_proof(); p['fetches'][1]['coordinates'] = [1, 0]; changes.append(p)
        p = pixel_proof(); p['fetches'][0]['coordinates'] = [0, 0]; changes.append(p)
        p = pixel_proof(); p['exports']['0'][3] = p['exports']['0'][0]; changes.append(p)  # nonzero alpha
        p = pixel_proof(); p['nodes'].append(('mul', 3, 3)); p['exports']['0'][0] = len(p['nodes']) - 1; changes.append(p)  # r0.w
        p = pixel_proof(); p['nodes'].append(('sine', 256)); changes.append(p)
        p = pixel_proof(); p['nodes'].append(('mul', 257, 10**6)); p['exports']['0'][1] = len(p['nodes']) - 1; changes.append(p)
        p = pixel_proof(); p['exports']['1'] = p['exports']['0']; changes.append(p)
        for q in changes:
            with self.assertRaises(RawShaderError): validate_pixel(q)

    def test_fragment_uses_three_samplers_legacy_multiply_and_zero_alpha(self):
        text = bright_pass_fragment(pixel_proof())
        for binding in ('binding=1) uniform sampler2D atlas1', 'binding=2) uniform sampler2D atlas10',
                        'binding=3) uniform sampler2D atlas17'):
            self.assertIn(binding, text)
        self.assertIn('textureLod(atlas17,vec2(uintBitsToFloat(readBE(1008u))),0.0).x', text)
        self.assertIn('textureLod(atlas10,vec2(r0x,r0y),0.0)', text)
        self.assertIn('==0.0 || ', text)
        self.assertTrue(text.rstrip().endswith(',0.0);\n}') or ',0.0);' in text)
        self.assertLess(text.index('vec4 texel=textureLod(atlas1'), text.index('scene.x'))

    def test_pixel_profile_is_separate(self):
        base = dict(ui_operations=True, extended_math=True, loading_math=True, bright_pass_profile=True)
        for extra in (dict(rectangle_mrt_profile=True), dict(geometry_profile=True), dict(filter_weights_profile=True),
                      dict(composite_profile=True), dict(loading_math=False)):
            with self.assertRaises(RawShaderError):
                analyze_pixel_sample_dataflow(b'', **dict(base, **extra))


if __name__ == '__main__': unittest.main()
