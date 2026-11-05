"""Composite full-screen pass: FRAC plus texel-addressed point-filtered fetch.

The capture that motivated this profile is a 3-vertex non-indexed rectangle
covering the whole screen whose pixel stage computes its own sampling
coordinates in texels. These tests pin the exact encoding admitted, prove no
other profile admits it, and prove the denormalized coordinate is converted by
dividing by the bound texture size rather than by an unnormalized sampler.
"""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from pixel_sample_dataflow import analyze_pixel_sample_dataflow, pixel_supported_opcodes
from inline_shader_translation import translate_inline_pair
from raw_shader_analysis import RawShaderError
from test_raw_shader_analysis import program, cf_alloc, cf_exec, alu
from test_inline_shader_translation import vertex, pixel as translatable_pixel
from test_pixel_sample_dataflow import texture, pixel as pixel_program
from test_vertex_arithmetic_dataflow import math_alu


BASE = dict(ui_operations=True, extended_math=True, loading_math=True)
# word0 bit25 = TX_COORD_DENORM; word1 bits 12-15 = 0 = explicit POINT mag/min.
DENORM = 1 << 25
POINT_FILTER_MASK = 0xF000


def composite_fetch(**kwargs):
    words = list(texture(**kwargs))
    words[0] |= DENORM
    words[1] &= ~POINT_FILTER_MASK
    return words


def translatable_composite_pixel(slots=(2, 3), second_source=1):
    """The translation fixture's shape, with composite-encoded fetches."""
    instruction = list(math_alu(op=0, dest=0, sources=(4, 5, 0), temps=(True, True, True)))
    instruction[1] |= 1 << 25
    return program([cf_exec(address=2, count=2, sequence=5, opcode=1), cf_alloc(), cf_exec(address=4), 0],
                   [composite_fetch(src=0, dst=4, slot=slots[0]),
                    composite_fetch(src=second_source, dst=5, slot=slots[1]), instruction])


class CompositeSamplingTests(unittest.TestCase):
    def analyze(self, code, **options):
        return analyze_pixel_sample_dataflow(code, **BASE, **options)

    def test_frac_is_admitted_only_by_the_composite_and_geometry_scalar_sets(self):
        _, without = pixel_supported_opcodes(ui_operations=True, extended_math=True, loading_math=True)
        _, with_composite = pixel_supported_opcodes(ui_operations=True, extended_math=True,
                                                    loading_math=True, composite_profile=True)
        self.assertNotIn(11, without)
        self.assertIn(11, with_composite)
        # The composite profile takes FRAC and nothing else from geometry.
        self.assertEqual(set(with_composite) - set(without), {11})

    def test_denormalized_point_fetch_is_rejected_without_the_profile(self):
        code = pixel_program(fetch=composite_fetch())
        with self.assertRaises(RawShaderError):
            self.analyze(code)
        with self.assertRaises(RawShaderError):
            self.analyze(code, explicit_lod_profile=True)
        proof = self.analyze(code, composite_profile=True)
        self.assertTrue(proof['fetches'][0]['denormalized_coordinates'])
        self.assertTrue(proof['fetches'][0]['explicit_point_filter'])

    def test_only_those_five_bits_are_admitted(self):
        # Any OTHER modified addressing/filter bit must still be rejected, so
        # the profile cannot become a general relaxation of fetch metadata.
        for extra_word, extra_bit in ((0, 1 << 18), (0, 1 << 19), (1, 1 << 16),
                                      (1, 1 << 24), (1, 1 << 28), (2, 1)):
            words = composite_fetch()
            words[extra_word] ^= extra_bit
            code = pixel_program(fetch=words)
            with self.assertRaises(RawShaderError):
                self.analyze(code, composite_profile=True)

    def test_an_ordinary_inherited_fetch_keeps_normalized_coordinates(self):
        proof = self.analyze(pixel_program(), composite_profile=True)
        self.assertNotIn('denormalized_coordinates', proof['fetches'][0])

    def test_profile_is_mutually_exclusive_with_cube_and_geometry(self):
        code = program([cf_alloc(), cf_exec(count=1)], [alu()])
        for conflicting in ('cube_profile', 'geometry_profile'):
            with self.assertRaises(RawShaderError):
                self.analyze(code, composite_profile=True, **{conflicting: True})
        with self.assertRaises(RawShaderError):
            analyze_pixel_sample_dataflow(code, ui_operations=True, extended_math=True,
                                          loading_math=False, composite_profile=True)

    def test_translation_requires_nonindexed_rectangle_geometry(self):
        for bad in (dict(indexed=True, rectangle_list=True), dict(indexed=False, rectangle_list=False)):
            with self.assertRaises(RawShaderError):
                translate_inline_pair(vertex(), translatable_pixel(), extended_profile=True, loading_profile=True,
                                      composite_profile=True, **bad)

    def test_denormalized_coordinate_divides_by_the_bound_texture_size(self):
        # The conversion must happen in the shader. A Vulkan
        # unnormalizedCoordinates sampler is not usable here: it forbids REPEAT
        # addressing and multiple mip levels, which real bindings use.
        _, source, _ = translate_inline_pair(vertex(), translatable_composite_pixel(), indexed=False, extended_profile=True,
                                             loading_profile=True, rectangle_list=True, composite_profile=True)
        self.assertIn('textureSize(atlas0,0)', source)
        self.assertNotIn('unnormalizedCoordinates', source)

        _, normalized, _ = translate_inline_pair(vertex(), translatable_pixel(), indexed=False, extended_profile=True,
                                                 loading_profile=True, rectangle_list=True)
        self.assertNotIn('textureSize', normalized)


if __name__ == '__main__':
    unittest.main()
