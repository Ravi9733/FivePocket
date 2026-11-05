"""Xenos signed half-texel fields through metadata and generated GLSL.

Authored inputs; these are not filtering or Xbox hardware conformance tests.
"""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from pixel_sample_dataflow import texture_fetch_metadata
from inline_shader_translation import translate_inline_pair
from raw_shader_analysis import RawShaderError
from test_raw_shader_analysis import program, cf_exec, cf_alloc, alu
from test_inline_shader_translation import vertex
from test_pixel_sample_dataflow import texture


def offset_fetch(x, y):
    a, b, c = texture(src=0, dst=4, slot=2)
    return a, b, c | ((x & 31) << 16) | ((y & 31) << 21)


class TextureFetchOffsetTests(unittest.TestCase):
    def test_all_signed_fields_are_half_texel_units(self):
        registers = [[None] * 4 for _ in range(64)]
        for x in range(-16, 16):
            for y in range(-16, 16):
                metadata = texture_fetch_metadata(offset_fetch(x, y), registers, 1,
                                                   texel_offset_profile=True)
                self.assertEqual(metadata.get('texel_offset', (0., 0.)), (x / 2., y / 2.))
                self.assertEqual(metadata['slot'], 2)
                self.assertTrue(metadata['computed_lod_requested'])

    def test_units_reach_glsl_without_double_scaling_or_invalid_literals(self):
        for x, y in ((1, -1), (2, -2), (-16, 15), (0, 0)):
            pixel = program([cf_exec(address=2, sequence=1, opcode=1), cf_alloc(),
                             cf_exec(address=3), 0], [offset_fetch(x, y), alu(source=4)])
            _, text, proof = translate_inline_pair(vertex(1), pixel,
                extended_profile=True, loading_profile=True,
                rectangle_list=True, offset_sample_profile=True)
            self.assertEqual(proof['abi_version'], 23)
            if x or y:
                self.assertIn(f'vec2({x / 2.},{y / 2.})/vec2(textureSize(atlas0,0))', text)
            else:
                self.assertNotIn('/vec2(textureSize(atlas0,0))', text)
            self.assertNotIn('.0.0', text)

    def test_offsets_do_not_enable_unrelated_addressing_modes(self):
        registers = [[None] * 4 for _ in range(64)]
        with self.assertRaises(RawShaderError):
            texture_fetch_metadata(offset_fetch(1, 0), registers, 1)
        for word, bit in ((0, 11), (0, 18), (0, 25), (1, 31),
                          (2, 0), (2, 1), (2, 2), (2, 15), (2, 26), (2, 31)):
            changed = list(offset_fetch(1, -1))
            changed[word] ^= 1 << bit
            with self.assertRaises(RawShaderError):
                texture_fetch_metadata(changed, registers, 1, texel_offset_profile=True)


if __name__ == '__main__':
    unittest.main()
