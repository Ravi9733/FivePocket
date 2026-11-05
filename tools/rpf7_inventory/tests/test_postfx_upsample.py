"""Authored graphs only, never embedded game shaders."""
import copy
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from postfx_upsample import validate_upsample_pixel, FRAGMENT
from expanded16_copy import validate_copy_pixel
from raw_shader_analysis import RawShaderError


def pixel_proof():
    nodes = [('initial', r, c) for r in range(64) for c in range(4)]
    samples = []
    for c in range(4):
        nodes.append(('sample', 0, c)); samples.append(len(nodes) - 1)
    out = []
    for s in samples[:3]:
        nodes.append(('maximum', s, s)); out.append(len(nodes) - 1)
    nodes.append(('literal', 0)); out.append(len(nodes) - 1)
    return dict(nodes=nodes, exports={'0': out}, kills=[],
                fetches=[dict(slot=5, coordinates=[0, 1], computed_lod_requested=True, fetch_valid_only=True)])


class PostfxUpsampleTests(unittest.TestCase):
    def test_rgb_copy_with_zero_alpha(self):
        self.assertEqual(validate_upsample_pixel(pixel_proof()), dict(copy_rgb=True, alpha=0.0, slot=5))
        with self.assertRaises(RawShaderError): validate_copy_pixel(pixel_proof())  # Not the ABI41 full copy.

    def test_near_misses_refused(self):
        changes = []
        p = pixel_proof(); p['kills'] = [{}]; changes.append(p)
        p = pixel_proof(); p['fetches'][0]['slot'] = 4; changes.append(p)
        p = pixel_proof(); p['fetches'][0]['texel_offset'] = (.5, 0); changes.append(p)
        p = pixel_proof(); p['fetches'][0]['coordinates'] = [1, 0]; changes.append(p)
        p = pixel_proof(); p['fetches'].append(copy.deepcopy(p['fetches'][0])); changes.append(p)
        p = pixel_proof(); p['exports']['0'][2] = p['exports']['0'][0]; changes.append(p)  # swizzle
        p = pixel_proof(); p['nodes'][p['exports']['0'][3]] = ('literal', 1); changes.append(p)  # alpha 1
        p = pixel_proof(); p['exports']['0'][3] = 256 + 3; p['nodes'].append(('sample', 0, 3)); changes.append(p)
        p = pixel_proof(); p['nodes'].append(('mul', 256, 257)); p['exports']['0'][0] = len(p['nodes']) - 1; changes.append(p)
        p = pixel_proof(); p['exports']['1'] = p['exports']['0']; changes.append(p)
        for q in changes:
            with self.assertRaises(RawShaderError): validate_upsample_pixel(q)

    def test_fragment_single_sample_zero_alpha(self):
        self.assertEqual(FRAGMENT.count('textureLod('), 1)
        self.assertIn('color=vec4(textureLod(atlas0,interpolator0,0.0).rgb,0.0);', FRAGMENT)


if __name__ == '__main__': unittest.main()
