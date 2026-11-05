"""Authored ABI26 interface tests; no game payload or runtime admission."""
import unittest
from test_inline_shader_translation import vertex
from test_raw_vertex_fetch_analysis import fetch
from test_composite_sampling import composite_fetch
from test_raw_shader_analysis import program, cf_alloc, cf_exec, alu
from inline_shader_translation import translate_inline_pair
from raw_shader_analysis import RawShaderError

OPTIONS = dict(extended_profile=True, loading_profile=True, rectangle_list=True,
               composite_profile=True, screen_space_profile=True)

def position_vertex():
    return program([cf_exec(address=2, sequence=1, opcode=1), cf_alloc(1),
                    cf_exec(address=3), 0],
                   [fetch(dst=1, slot=95, fmt=38, stride=4, swizzle=0x688),
                    alu(dest=62, source=1)])


def pixel(source=0, export_source=4):
    return program([cf_exec(address=2, sequence=1, opcode=1), cf_alloc(),
                    cf_exec(address=3), 0],
                   [composite_fetch(src=source, dst=4, slot=2), alu(source=export_source)])


class ScreenSpaceRectangleTests(unittest.TestCase):
    def test_position_only_interface_reads_generated_xy(self):
        vs, ps, meta = translate_inline_pair(position_vertex(), pixel(), **OPTIONS)
        self.assertEqual(meta['abi_version'], 26)
        self.assertEqual(meta['interpolators'], 0)
        self.assertEqual(meta['screen_space_parameter_register'], 0)
        self.assertFalse(meta['screen_space_native_state_verified'])
        self.assertNotIn('interpolator', ps)
        self.assertIn('abs(floor(gl_FragCoord.xy))', ps)
        self.assertIn('gl_FrontFacing', ps)
        self.assertIn('generatedPixelXY[0]', ps)
        self.assertIn('generatedPixelXY[1]', ps)
        self.assertIn('+vec2(0.00146484375))/vec2(textureSize(atlas0,0))', ps)
        self.assertIn('originalVertex', vs)
        compute, _, _ = translate_inline_pair(position_vertex(), pixel(), **OPTIONS, position_compute=True)
        self.assertIn('gl_GlobalInvocationID.x', compute)

    def test_existing_profile_still_rejects_missing_interpolators(self):
        options = dict(OPTIONS, screen_space_profile=False)
        with self.assertRaises(RawShaderError):
            translate_inline_pair(position_vertex(), pixel(), **options)

    def test_other_composite_profile_keeps_existing_coordinate_policy(self):
        _, ps, meta = translate_inline_pair(vertex(1), pixel(),
                                            **dict(OPTIONS, screen_space_profile=False))
        self.assertEqual(meta['abi_version'], 20)
        self.assertNotIn('0.00146484375', ps)
        self.assertNotIn('screen_space_profile', meta)

    def test_unprovided_registers_and_generated_zw_stay_unknown(self):
        for ps in (pixel(source=1), pixel(export_source=0)):
            with self.assertRaises(RawShaderError):
                translate_inline_pair(position_vertex(), ps, **OPTIONS)

    def test_varyings_are_not_silently_relabelled_as_screen_xy(self):
        with self.assertRaises(RawShaderError):
            translate_inline_pair(vertex(1), pixel(), **OPTIONS)

    def test_conflicting_profiles_remain_forbidden(self):
        for options in (dict(OPTIONS, composite_profile=False), dict(OPTIONS, indexed=True),
                        dict(OPTIONS, explicit_lod_profile=True), dict(OPTIONS, offset_sample_profile=True),
                        dict(OPTIONS, geometry_parameter_register=1), dict(OPTIONS, screen_space_profile=1)):
            with self.assertRaises(RawShaderError):
                translate_inline_pair(position_vertex(), pixel(), **options)


if __name__ == '__main__':
    unittest.main()
