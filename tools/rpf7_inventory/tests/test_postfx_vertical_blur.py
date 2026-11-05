"""Authored graphs only, never embedded game shaders."""
import copy
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from postfx_blur import validate_blur_pixel
from postfx_vertical_blur import validate_vertical_blur_pixel, vertical_blur_fragment
from raw_shader_analysis import RawShaderError

OFFSETS = [(254, 2), (255, 0), None, (255, 2), (255, 1)]
WEIGHTS = [(255, 3), (254, 0), (254, 1), (254, 0), (255, 3)]


def pixel_proof(vertical=True):
    nodes = [('initial', r, c) for r in range(64) for c in range(4)]
    def add(n):
        nodes.append(n); return len(nodes) - 1
    along, across = (1, 0) if vertical else (0, 1)
    scale = add(('constant', 189, along))
    fixed = add(('maximum', across, across))
    fetches = []
    for tap, k in enumerate(OFFSETS):
        moving = along if k is None else add(('add', add(('mul', scale, add(('constant',) + k))), along))
        still = fixed if tap else across
        coordinates = [still, moving] if vertical else [moving, still]
        fetches.append(dict(slot=5, coordinates=coordinates, computed_lod_requested=True, fetch_valid_only=True))
    samples = [[add(('sample', tap, lane)) for lane in range(4)] for tap in range(5)]
    weights = [add(('constant',) + w) for w in WEIGHTS]
    out = []
    for lane in range(4):
        total = add(('mul', samples[4][lane], weights[4]))
        for tap in (3, 2, 1, 0):
            total = add(('add', add(('mul', samples[tap][lane], weights[tap])), total))
        out.append(total)
    return dict(nodes=nodes, exports={'0': out}, kills=[], fetches=fetches)


class PostfxVerticalBlurTests(unittest.TestCase):
    def test_vertical_accepted_and_axes_exclusive(self):
        c = validate_vertical_blur_pixel(pixel_proof())
        self.assertEqual((c['taps'], c['axis'], c['centre_fetch'], c['scale']), (5, 'y', 2, (189, 1)))
        self.assertEqual(c['offsets'], OFFSETS)
        self.assertEqual(c['weights'], WEIGHTS)
        validate_blur_pixel(pixel_proof(vertical=False))  # The same builder authors a valid ABI42 graph.
        with self.assertRaises(RawShaderError): validate_vertical_blur_pixel(pixel_proof(vertical=False))
        with self.assertRaises(RawShaderError): validate_blur_pixel(pixel_proof())

    def test_near_misses_refused(self):
        changes = []
        p = pixel_proof(); p['kills'] = [{}]; changes.append(p)
        p = pixel_proof(); p['fetches'][0]['slot'] = 4; changes.append(p)
        p = pixel_proof(); p['fetches'][1]['texel_offset'] = (0, .5); changes.append(p)
        p = pixel_proof(); p['fetches'][3]['coordinates'][0] = 3; changes.append(p)  # not the interpolated column
        p = pixel_proof(); p['fetches'].pop(); changes.append(p)
        p = pixel_proof(); p['fetches'].append(copy.deepcopy(p['fetches'][0])); changes.append(p)
        p = pixel_proof(); p['fetches'][0]['coordinates'][1] = 1; changes.append(p)  # two centre taps
        p = pixel_proof(); p['fetches'][4]['coordinates'][1] = p['fetches'][3]['coordinates'][1]; changes.append(p)
        p = pixel_proof(); p['exports']['0'][3] = p['exports']['0'][0]; changes.append(p)
        p = pixel_proof(); p['nodes'].append(('reciprocal', 256)); changes.append(p)
        p = pixel_proof()  # One lane uses a sample of another lane.
        n = p['nodes']; root = n[p['exports']['0'][1]]; term = n[root[1]]; s = n[term[1]]
        n.append(('sample', s[1], 0)); n[root[1]] = ('mul', len(n) - 1, term[2]); changes.append(p)
        for q in changes:
            with self.assertRaises(RawShaderError): validate_vertical_blur_pixel(q)

    def test_fragment_samples_one_column(self):
        text = vertical_blur_fragment(pixel_proof())
        self.assertEqual(text.count('textureLod('), 5)
        self.assertIn('vec4 s2=textureLod(atlas0,vec2(', text)
        self.assertIn(',r0y),0.0);', text)  # The centre tap keeps the interpolated y.
        self.assertEqual(text.count('precise float'), 4 * 2 + 1 + 4 * 9)


if __name__ == '__main__': unittest.main()
