"""Authored texture -> color/depth rectangle contract; no game payloads."""
import struct
import unittest
from test_extended_shader_math import vertex_code
from test_raw_shader_analysis import alu
from test_vertex_arithmetic_dataflow import math_alu
from test_pixel_sample_dataflow import pixel, texture
from test_geometry_gradient_depth import gradient
from raw_shader_analysis import RawShaderError
from pixel_sample_dataflow import analyze_pixel_sample_dataflow, evaluate_pixel_sample_dataflow
from inline_shader_translation import translate_inline_pair

PIXEL = dict(ui_operations=True, extended_math=True, loading_math=True, rectangle_depth_profile=True)
TRANSLATE = dict(extended_profile=True, loading_profile=True, rectangle_list=True,
                 fixed16_rectangle_depth_profile=True)


def fragment(*, depth=True, mask=1, destination=61):
    # Fetch only R, preserving the other temporary lanes. The dot uses RRRR,
    # not those preserved/possibly undefined lanes. Constants are live inputs.
    color = math_alu(op=15, dest=0, sources=(0,216,0), temps=(True,False,True), swizzles=(0x6C,0,0))
    instructions = [color]
    if depth:
        a,b,c = math_alu(op=15, dest=destination, mask=mask, sources=(0,216,0),
                         temps=(True,False,True), swizzles=(0x6C,0,0))
        instructions.append((a | (1<<24), b, c))
    a,b,c = texture(swizzle=0xFF8)
    return pixel(instructions, fetch=(a & ~(1<<19), b, c))


class Fixed16RectangleDepthTests(unittest.TestCase):
    def test_sample_constant_expression_color_and_clamped_depth(self):
        code=fragment()
        proof=analyze_pixel_sample_dataflow(code, **PIXEL)
        self.assertEqual(proof['constant_registers'], [216])
        self.assertEqual(proof['gradients'], [])
        self.assertTrue(proof['rectangle_depth_profile'])
        self.assertFalse(proof['geometry_gradient_depth_profile'])
        self.assertFalse(proof['gpu_executed'])
        for constants,scale in (((.125,.25,.5,.125),1.), ((.25,.5,1.,.25),2.)):
            for red in (-2.,-0.,0.,.125,.5,1.,4.):
                result=evaluate_pixel_sample_dataflow(code,{0:(.25,.75,None,None)},
                    {216:struct.pack('>4f',*constants)}, {0:(red,99.,101.,103.)}, **PIXEL)
                self.assertEqual(result['0'], [red*scale]*4)
                self.assertEqual(result['61'], [max(0.,min(1.,red*scale)),None,None,None])

    def test_distinct_raster_abi_keeps_original_vertex_rectangle_and_depth(self):
        vs,ps,proof=translate_inline_pair(vertex_code(alu(dest=0)),fragment(), **TRANSLATE)
        self.assertEqual(proof['abi_version'],19)
        self.assertEqual(proof['texture_slots'],[2])
        self.assertFalse(proof['indexed'])
        self.assertFalse(proof['geometry_gradient_depth_profile'])
        self.assertFalse(proof['geometry_resources_verified'])
        self.assertTrue(proof['geometry_explicit_depth'])
        self.assertTrue(proof['geometry_depth20e4'])
        self.assertIn('originalVertex(base+i,positions[i]',vs)
        self.assertIn('(positions[b]-positions[a])+positions[c]',vs)
        self.assertIn('geometryCheckFixed16Position();',vs)
        self.assertGreater(vs.index('geometryCheckFixed16Position();'),vs.index('gl_Position.xy=gl_Position.xy*'))
        self.assertIn('=texture(',ps)
        self.assertIn('geometryStoreFixed16ExplicitDepth(color,',ps)
        self.assertIn('gl_FragDepth=geometryExpand20e4(',ps)
        for forbidden in ('dFdx','dFdy','geometryNdcZ','early_fragment_tests','gl_SampleID'):
            self.assertNotIn(forbidden,ps)

    def test_old_profiles_and_unrelated_effects_remain_rejected(self):
        vertex=vertex_code(alu(dest=0))
        for extra in ({'fixed16_rectangle_depth_profile':False}, {'fixed16_rectangle_depth_profile':1},
                      {'rectangle_list':False}, {'indexed':True}, {'geometry_profile':True},
                      {'geometry_gradient_depth_profile':True}, {'geometry_predicate_profile':True},
                      {'predicate_profile':True}, {'explicit_lod_profile':True}, {'cube_profile':True},
                      {'position_compute':True}, {'geometry_color8_format':0}, {'geometry_centroid_mask':1}):
            with self.assertRaises(RawShaderError):
                translate_inline_pair(vertex,fragment(), **{**TRANSLATE,**extra})
        with self.assertRaises(RawShaderError):translate_inline_pair(vertex,fragment(depth=False), **TRANSLATE)
        for mask in (0,2,3,15):
            with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(fragment(mask=mask), **PIXEL)
        for destination in (4,60,62,63):
            with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(fragment(destination=destination), **PIXEL)
        with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(pixel([alu()],fetch=gradient()), **PIXEL)
        for extra in ({'rectangle_depth_profile':1},{'geometry_profile':True},{'geometry_gradient_depth_profile':True}):
            with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(fragment(), **{**PIXEL,**extra})


if __name__=='__main__':unittest.main()
