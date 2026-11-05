"""Authored graphs only, never embedded game shaders."""
import copy
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from expanded16_copy import validate_copy_pixel, validate_rectangle_vertex, FRAGMENT
from raw_shader_analysis import RawShaderError


def pixel_proof():
    nodes = [('initial', r, c) for r in range(64) for c in range(4)]
    samples = []
    for c in range(4):
        nodes.append(('sample', 0, c)); samples.append(len(nodes) - 1)
    out = []
    for s in samples:
        nodes.append(('maximum', s, s)); out.append(len(nodes) - 1)
    return dict(nodes=nodes, exports={'0': out}, kills=[],
                fetches=[dict(slot=5, coordinates=[0, 1], computed_lod_requested=True, fetch_valid_only=True)])


def vertex_proof():
    nodes = [('initial', r, c) for r in range(64) for c in range(4)]
    def add(n):
        nodes.append(n); return len(nodes) - 1
    p = [add(('fetch', 0, 0)), add(('fetch', 0, 1)), add(('fetch', 0, 2)), add(('literal', 1))]
    uv = [add(('fetch', 1, 0)), add(('fetch', 1, 1))]
    return dict(nodes=nodes, exports={'62': [add(('maximum', i, i)) for i in p],
                                      '0': [add(('maximum', i, i)) for i in uv] + [None, None]},
        fetches=[dict(fetch_slot=95, stride_words=6, index_expression=0, format=57, components=3, offset_words=0),
                 dict(fetch_slot=95, stride_words=6, index_expression=0, format=37, components=2, offset_words=4)])


class Expanded16CopyTests(unittest.TestCase):
    def test_copy_accepted_and_arithmetic_refused(self):
        self.assertTrue(validate_copy_pixel(pixel_proof())['copy'])
        changes = []
        p = pixel_proof(); p['kills'] = [{}]; changes.append(p)
        p = pixel_proof(); p['fetches'][0]['slot'] = 4; changes.append(p)
        p = pixel_proof(); p['fetches'][0]['texel_offset'] = (.5, 0); changes.append(p)
        p = pixel_proof(); p['fetches'][0]['coordinates'] = [1, 0]; changes.append(p)
        p = pixel_proof(); p['fetches'].append(copy.deepcopy(p['fetches'][0])); changes.append(p)
        p = pixel_proof(); p['exports']['0'][3] = p['exports']['0'][0]; changes.append(p)  # swizzle
        p = pixel_proof(); p['nodes'].append(('mul', 256, 257)); p['exports']['0'][0] = len(p['nodes']) - 1; changes.append(p)
        p = pixel_proof(); p['nodes'].append(('maximum', 256, 257)); changes.append(p)
        p = pixel_proof(); p['exports']['1'] = p['exports']['0']; changes.append(p)
        for q in changes:
            with self.assertRaises(RawShaderError): validate_copy_pixel(q)

    def test_rectangle_vertex_contract(self):
        validate_rectangle_vertex(vertex_proof(), 24)
        changes = []
        v = vertex_proof(); v['texture_fetches'] = [{}]; changes.append((v, 24))
        v = vertex_proof(); changes.append((v, 28))
        v = vertex_proof(); v['exports']['62'][3] = v['exports']['0'][0]; changes.append((v, 24))
        v = vertex_proof(); v['exports']['0'][0] = None; changes.append((v, 24))
        v = vertex_proof(); v['fetches'][1]['format'] = 38; changes.append((v, 24))
        v = vertex_proof(); v['fetches'][0]['fetch_slot'] = 94; changes.append((v, 24))
        for q, stride in changes:
            with self.assertRaises(RawShaderError): validate_rectangle_vertex(q, stride)

    def test_fragment_is_a_single_base_level_sample(self):
        self.assertEqual(FRAGMENT.count('textureLod('), 1)
        self.assertIn('color=textureLod(atlas0,interpolator0,0.0);', FRAGMENT)


if __name__ == '__main__': unittest.main()
