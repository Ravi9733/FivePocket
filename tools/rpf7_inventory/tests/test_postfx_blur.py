"""Authored graphs only, never embedded game shaders."""
import copy
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from postfx_blur import validate_blur_pixel, blur_fragment
from raw_shader_analysis import RawShaderError

OFFSETS = [(254, 2), (255, 0), None, (255, 2), (255, 1)]
WEIGHTS = [(255, 3), (254, 0), (254, 1), (254, 0), (255, 3)]


def pixel_proof():
    nodes = [('initial', r, c) for r in range(64) for c in range(4)]
    def add(n):
        nodes.append(n); return len(nodes) - 1
    scale = add(('constant', 189, 0))
    y = add(('maximum', 1, 1))
    fetches = []
    for tap, k in enumerate(OFFSETS):
        x = 0 if k is None else add(('add', add(('mul', scale, add(('constant',) + k))), 0))
        fetches.append(dict(slot=5, coordinates=[x, y if tap else 1], computed_lod_requested=True, fetch_valid_only=True))
    samples = [[add(('sample', tap, lane)) for lane in range(4)] for tap in range(5)]
    weights = [add(('constant',) + w) for w in WEIGHTS]
    out = []
    for lane in range(4):
        total = add(('mul', samples[4][lane], weights[4]))
        for tap in (3, 2, 1, 0):
            total = add(('add', add(('mul', samples[tap][lane], weights[tap])), total))
        out.append(total)
    return dict(nodes=nodes, exports={'0': out}, kills=[], fetches=fetches)


class PostfxBlurTests(unittest.TestCase):
    def test_blur_accepted(self):
        c = validate_blur_pixel(pixel_proof())
        self.assertEqual((c['taps'], c['centre_fetch'], c['scale']), (5, 2, (189, 0)))
        self.assertEqual(c['offsets'], [None if k is None else k for k in OFFSETS])
        self.assertEqual(c['weights'], WEIGHTS)

    def test_near_misses_refused(self):
        changes = []
        p = pixel_proof(); p['kills'] = [{}]; changes.append(p)
        p = pixel_proof(); p['fetches'][0]['slot'] = 4; changes.append(p)
        p = pixel_proof(); p['fetches'][1]['texel_offset'] = (.5, 0); changes.append(p)
        p = pixel_proof(); p['fetches'][3]['coordinates'][1] = 2; changes.append(p)  # not the interpolated row
        p = pixel_proof(); p['fetches'][0]['coordinates'] = p['fetches'][0]['coordinates'][::-1]; changes.append(p)  # vertical
        p = pixel_proof(); p['fetches'].pop(); changes.append(p)
        p = pixel_proof(); p['fetches'].append(copy.deepcopy(p['fetches'][0])); changes.append(p)
        p = pixel_proof(); p['fetches'][0]['coordinates'][0] = 0; changes.append(p)  # two centre taps
        p = pixel_proof(); p['fetches'][4]['coordinates'][0] = p['fetches'][3]['coordinates'][0]; changes.append(p)  # repeated k
        p = pixel_proof(); p['exports']['0'][3] = p['exports']['0'][0]; changes.append(p)  # lane swizzle
        p = pixel_proof(); p['exports']['1'] = p['exports']['0']; changes.append(p)
        p = pixel_proof(); p['nodes'].append(('reciprocal', 256)); changes.append(p)
        p = pixel_proof()  # Different scale constant for one tap.
        n = p['nodes']; n.append(('constant', 188, 0)); other = len(n) - 1
        x = n[p['fetches'][0]['coordinates'][0]]; m = n[x[1]]; n[x[1]] = ('mul', other, m[2]); changes.append(p)
        p = pixel_proof()  # One lane uses a sample of another lane.
        n = p['nodes']; root = n[p['exports']['0'][1]]; term = n[root[1]]; s = n[term[1]]
        n.append(('sample', s[1], 0)); n[root[1]] = ('mul', len(n) - 1, term[2]); changes.append(p)
        p = pixel_proof()  # A weight that is not a constant.
        n = p['nodes']; root = n[p['exports']['0'][0]]; term = n[root[1]]
        n[root[1]] = ('mul', term[1], 0); changes.append(p)
        p = pixel_proof()  # Lane weights differ for one tap.
        n = p['nodes']; root = n[p['exports']['0'][2]]; term = n[root[1]]
        n.append(('constant', 250, 0)); n[root[1]] = ('mul', term[1], len(n) - 1); changes.append(p)
        for q in changes:
            with self.assertRaises(RawShaderError): validate_blur_pixel(q)

    def test_fragment_samples_five_times_in_order(self):
        text = blur_fragment(pixel_proof())
        self.assertEqual(text.count('textureLod('), 5)
        self.assertIn('vec4 s2=textureLod(atlas0,vec2(r0x,', text)  # Centre tap at the interpolated x.
        self.assertEqual(text.count('precise float'), 4 * 2 + 1 + 4 * 9)  # 4 taps x (mul, add), row copy, lanes.
        self.assertTrue(text.rstrip().endswith('}'))


if __name__ == '__main__': unittest.main()
