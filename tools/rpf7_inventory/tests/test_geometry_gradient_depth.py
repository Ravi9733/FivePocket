"""Explicit quad derivatives and depth exports, not invented texture samples."""
import unittest
from test_pixel_sample_dataflow import pixel
from test_extended_shader_math import vertex_code
from test_raw_shader_analysis import alu
from test_vertex_arithmetic_dataflow import math_alu
from raw_shader_analysis import RawShaderError
from pixel_sample_dataflow import analyze_pixel_sample_dataflow,evaluate_pixel_sample_dataflow
from inline_shader_translation import translate_inline_pair

OPTIONS=dict(ui_operations=True,extended_math=True,loading_math=True,geometry_profile=True,
    geometry_gradient_depth_profile=True)


def gradient(src=0,dst=0,swizzle=0x688,coordinates=(0,1)):
    return 18|(1<<19)|(src<<5)|(dst<<12)|(coordinates[0]<<26)|(coordinates[1]<<28),0x1F1FF000|swizzle,0


def depth(mask=1):
    return math_alu(op=2,dest=61,mask=mask,sources=(1,1,1),temps=(True,True,True))


def quad():
    return [{0:v,1:(0.25,0.5,0.75,1.)} for v in
        ((2.,10.,101.,103.),(5.,17.,107.,109.),(7.,21.,113.,127.),(29.,47.,131.,137.))]


class GeometryGradientDepthTests(unittest.TestCase):
    def evaluate(self,code,inputs=None,**options):
        return evaluate_pixel_sample_dataflow(code,{}, {}, {},quad_initial=quad() if inputs is None else inputs,
            **{**OPTIONS,**options})

    def test_coarse_xy_derivatives_not_fine_and_explicit_depth_x(self):
        code=pixel([depth(),alu()],fetch=gradient())
        proof=analyze_pixel_sample_dataflow(code,**OPTIONS)
        self.assertEqual(proof['fetches'],[])
        self.assertFalse(proof['gradients'][0]['texture_resource_required'])
        self.assertFalse(proof['gradients'][0]['hardware_quad_verified'])
        for lane in range(4):
            self.assertEqual(self.evaluate(code,quad_lane=lane),{'0':[3.,5.,7.,11.],'61':[0.25,None,None,None]})

    def test_partial_destination_read_before_write_and_unused_source_lane(self):
        # Captured shape __ / ddx(src.x) / ddy(src.x) / __; r0.x is preserved.
        code=pixel([depth(),alu()],fetch=gradient(swizzle=0xE47,coordinates=(0,3)))
        inputs=quad()
        for v in inputs:v[0]=(v[0][0],None,None,0.)
        self.assertEqual(self.evaluate(code,inputs)['0'],[2.,3.,5.,0.])
        # The unused second gradient source is not evaluated, even if undefined.
        for v in inputs:v[0]=(v[0][0],None,None,None)
        code=pixel([depth(),math_alu(op=2,dest=0,mask=7,sources=(0,0,0),temps=(True,True,True))],
            fetch=gradient(swizzle=0xE47,coordinates=(0,3)))
        self.assertEqual(self.evaluate(code,inputs)['0'],[2.,3.,5.,None])

    def test_constant_gradient_sources_are_computed_from_all_lanes(self):
        values=[{0:(4.,-0.,7.,8.),1:(0.25,0.,0.,0.)} for _ in range(4)]
        code=pixel([depth(),alu()],fetch=gradient())
        self.assertEqual(self.evaluate(code,values)['0'],[0.,0.,0.,0.])
        with self.assertRaisesRegex(RawShaderError,'explicit quad'):
            evaluate_pixel_sample_dataflow(code,values[0],{}, {},**OPTIONS)

    def test_old_profiles_unsupported_modifiers_and_memory_exports_reject(self):
        code=pixel([depth(),alu()],fetch=gradient())
        for options in ({**OPTIONS,'geometry_gradient_depth_profile':False},
                        {**OPTIONS,'geometry_profile':False},
                        {**OPTIONS,'geometry_predicate_profile':True}):
            with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(code,**options)
        for word,bit in ((0,11),(0,18),(0,20),(0,25),(0,30),(1,31),(2,14),(2,0)):
            fetch=list(gradient());fetch[word]|=1<<bit
            with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(pixel([depth(),alu()],fetch=fetch),**OPTIONS)
        for mask in (0,2,3,15):
            with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(pixel([depth(mask),alu()],fetch=gradient()),**OPTIONS)
        a,b,c=depth()
        for dest in (4,60,62,63):
            with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(pixel([((a&~63)|dest,b,c),alu()],fetch=gradient()),**OPTIONS)

    def test_missing_invalid_quad_and_undefined_used_lane_reject(self):
        code=pixel([depth(),alu()],fetch=gradient())
        for inputs in ([{}],(None,)*4):
            with self.assertRaises(RawShaderError):self.evaluate(code,inputs)
        values=quad();values[1][0]=(None,17.,0.,0.)
        with self.assertRaises(RawShaderError):self.evaluate(code,values)
        with self.assertRaises(RawShaderError):self.evaluate(code,quad_lane=True)

    def test_distinct_raster_abi_has_resource_free_derivatives_and_explicit_depth(self):
        vertex=vertex_code(alu(dest=0))
        d=math_alu(op=2,dest=61,mask=1,sources=(1,1,1),temps=(False,False,False))
        fragment=pixel([d,alu()],fetch=gradient())
        options=dict(indexed=True,extended_profile=True,loading_profile=True,geometry_profile=True,
            geometry_gradient_depth_profile=True)
        vs,ps,proof=translate_inline_pair(vertex,fragment,**options)
        self.assertEqual(proof['abi_version'],17)
        self.assertEqual(proof['texture_slots'],[])
        self.assertEqual(proof['geometry_fixed16_format'],4)
        self.assertTrue(proof['geometry_depth20e4'])
        self.assertTrue(proof['geometry_explicit_depth'])
        self.assertFalse(proof['geometry_sample_depth'])
        self.assertFalse(proof['geometry_resources_verified'])
        self.assertFalse(proof['geometry_gradient_hardware_verified'])
        self.assertIn('dFdxCoarse(',ps);self.assertIn('dFdyCoarse(',ps)
        self.assertNotIn('sampler',ps);self.assertNotIn('gl_SampleID',ps)
        self.assertNotIn('geometryNdcZ',ps)
        self.assertIn('geometryStoreFixed16ExplicitDepth(color,',ps)
        self.assertIn('gl_FragDepth=geometryExpand20e4(',ps)
        self.assertIn('shifted=product+32768.0',ps)
        self.assertIn('roundEven(shifted)-32768.0',ps)
        self.assertIn('geometryCheckFixed16Position();',vs)
        main=ps.split('void main() {',1)[1]
        self.assertLess(main.index('dFdxCoarse('),main.index('geometryStoreFixed16ExplicitDepth('))
        for extra in (dict(geometry_gradient_depth_profile=False),dict(geometry_color8_format=0),
                      dict(geometry_mrt_formats=(0,None,None,None)),dict(geometry_centroid_mask=1),
                      dict(geometry_compute=True),dict(geometry_predicate_profile=True)):
            with self.assertRaises(RawShaderError):translate_inline_pair(vertex,fragment,**{**options,**extra})

    def test_position_observation_keeps_actual_pair_analysis_but_no_pixel_execution(self):
        vertex=vertex_code(alu(dest=0))
        d=math_alu(op=2,dest=61,mask=1,sources=(1,1,1),temps=(False,False,False))
        fragment=pixel([d,alu()],fetch=gradient())
        options=dict(indexed=True,extended_profile=True,loading_profile=True,geometry_profile=True,
            geometry_gradient_depth_profile=True)
        _,_,raster=translate_inline_pair(vertex,fragment,**options)
        compute,_,proof=translate_inline_pair(vertex,fragment,**options,geometry_position_compute=True)
        self.assertEqual(proof['vertex'],raster['vertex'])
        self.assertEqual(proof['pixel'],raster['pixel'])
        self.assertTrue(proof['geometry_position_compute'])
        self.assertFalse(proof['geometry_resources_verified'])
        self.assertIn('gl_GlobalInvocationID.x',compute)
        self.assertIn('floatBitsToUint(position[lane])',compute)
        for forbidden in ('gl_Position','dFdx','dFdy','gl_FragDepth','geometryCheckFixed16Position',
                          'interpolator0_', 'atomicAdd'):
            self.assertNotIn(forbidden,compute)
        # Position observation still analyzes/rejects unsupported original PS
        # side effects; it never supplies a dummy shader to obtain positions.
        with self.assertRaises(RawShaderError):
            translate_inline_pair(vertex,pixel([depth(mask=3),alu()],fetch=gradient()),
                **options,geometry_position_compute=True)


if __name__=='__main__':unittest.main()
