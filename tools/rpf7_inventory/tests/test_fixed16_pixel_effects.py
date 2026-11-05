"""Connected authored sampling -> predicate/discard -> gradient/depth contracts.

No game payloads or invented live samples. Reference scalar30 is PRED_SETGTE;
vector25 KILLGT has a side effect even with a zero destination mask.
"""
import math
import struct
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from raw_shader_analysis import RawShaderError
from pixel_sample_dataflow import analyze_pixel_sample_dataflow, evaluate_pixel_sample_dataflow
from inline_shader_translation import translate_inline_pair
from test_raw_shader_analysis import alu, program, cf_alloc, cf_exec
from test_loading_shader_math import scalar_alu
from test_predicated_shader import predicated, shader
from test_vertex_arithmetic_dataflow import math_alu
from test_geometry_shader_translation import vertex
from test_geometry_gradient_depth import gradient
from test_pixel_sample_dataflow import texture

OPTIONS=dict(ui_operations=True, extended_math=True, loading_math=True,
    geometry_profile=True, geometry_predicate_profile=True,
    geometry_gradient_depth_profile=True, geometry_fixed16_effects_profile=True)
TRANSLATE=dict(indexed=True, extended_profile=True, loading_profile=True,
    geometry_profile=True, geometry_predicate_profile=True,
    geometry_gradient_depth_profile=True, geometry_fixed16_effects_profile=True)


def pixel(condition=True):
    kill=math_alu(op=25, dest=0, mask=0, sources=(2,16,0),
        temps=(True,False,True), export=False)
    depth=math_alu(op=0, dest=61, mask=1, sources=(4,17,0), temps=(True,False,True))
    return program([cf_alloc(), cf_exec(count=6, sequence=1|(1<<6))],
        [texture(src=0,dst=2,slot=0), scalar_alu(30,source=2,mask=0), predicated(kill,condition),
         gradient(src=1,dst=4), depth, alu(source=2)])


class Fixed16PixelEffectsTests(unittest.TestCase):
    def evaluate(self, code, initial, constants=None, samples=None, **extra):
        return evaluate_pixel_sample_dataflow(code,initial,constants or {},samples or {},
            **OPTIONS,**extra)

    def test_predicate_ge_zero_mask_updates_predicate_and_previous_scalar(self):
        # The vector copy consumes the scalar previous result via a later ADD_PREV.
        code=shader([scalar_alu(30,mask=0), scalar_alu(1,source=1), alu()])
        for value in (-2.,-0.,0.,3.):
            result=self.evaluate(code,{0:[0.,0.,0.,value],1:[0.,0.,0.,2.]})
            self.assertEqual(result['0'],[3. if value<0 else 2.]*4)
        code=shader([scalar_alu(30),alu()])
        for value in (-2.,-0.,0.,3.):
            result=self.evaluate(code,{0:[0.,0.,0.,value]})
            self.assertEqual(result['0'],[float(value<0)]*4)
        for value in (math.nan,math.inf,-math.inf,2**-149):
            with self.assertRaises(RawShaderError):self.evaluate(code,{0:[0.,0.,0.,value]})

    def test_predicate_ge_uses_precoissue_swizzle_abs_and_negation(self):
        # Scalar sees -abs(original r0.x), while a co-issued vector replaces r0.x.
        a,b,c=math_alu(op=2,dest=0,mask=1,sources=(1,1,128),
            temps=(True,True,True),export=False)
        instruction=((a&((1<<26)-1))|(30<<26)|(2<<20),b|64|(1<<24),c)
        code=shader([instruction,alu()])
        self.assertEqual(self.evaluate(code,{0:[-2.,8.,9.,10.],1:[4.]*4})['0'],[4.,1.,9.,10.])

    def test_connected_sampling_predicate_discard_and_explicit_depth(self):
        quad=[{0:(.25,.5,0.,0.),1:v} for v in
              ((0.,0.,0.,0.),(.25,0.,0.,0.),(0.,.5,0.,0.),(.25,.5,0.,0.))]
        constants={16:struct.pack('>4f',.5,.5,.5,.5),17:bytes(16)}
        for condition in (False,True):
            for alpha in (-1.,-0.,0.,1.):
                for red in (.25,1.):
                    samples=[{0:(red,.25,.25,alpha)} for _ in range(4)]
                    for lane in range(4):
                        result=self.evaluate(pixel(condition),{},constants,
                            quad_initial=quad,quad_samples=samples,quad_lane=lane,include_state=True)
                        killed=((alpha>=0)==condition) and (red>.5 or alpha>.5)
                        self.assertEqual(result['discarded'],killed)
                        if killed:self.assertEqual(result['exports'],{})
                        else:
                            self.assertEqual(result['exports']['0'],[red,.25,.25,alpha])
                            self.assertEqual(result['exports']['61'],[.25,None,None,None])
        # A helper lane is not optional just because the tested lane survives.
        with self.assertRaises(RawShaderError):
            self.evaluate(pixel(),{},constants,{0:(.25,)*4},quad_initial=quad)

    def test_glsl_samples_and_derivatives_precede_discard_and_stores(self):
        vs,ps,proof=translate_inline_pair(vertex(mask0=15,mask1=15),pixel(),**TRANSLATE)
        self.assertEqual(proof['abi_version'],18)
        self.assertEqual(proof['texture_slots'],[0])
        self.assertTrue(proof['geometry_fixed16_effects_profile'])
        self.assertFalse(proof['geometry_resources_verified'])
        self.assertTrue(proof['pixel']['deferred_discard'])
        main=ps.split('void main() {',1)[1]
        for before in ('=texture(', 'dFdxCoarse(', 'dFdyCoarse('):
            self.assertLess(main.index(before),main.index('if (pixelKilled) discard;'))
        for after in ('color=vec4(', 'geometryStoreFixed16ExplicitDepth('):
            self.assertLess(main.index('if (pixelKilled) discard;'),main.index(after))
        self.assertNotIn('early_fragment_tests',ps)
        self.assertNotIn('pixelKilled',vs)
        compute,same_ps,position=translate_inline_pair(vertex(mask0=15,mask1=15),pixel(),
            **TRANSLATE,geometry_position_compute=True)
        self.assertEqual(ps,same_ps)
        self.assertEqual(position['pixel'],proof['pixel'])
        self.assertNotIn('pixelKilled',compute)

    def test_old_profiles_unknown_initial_predicate_and_mutation_reject(self):
        for extra in (dict(geometry_fixed16_effects_profile=False),
                      dict(geometry_gradient_depth_profile=False),
                      dict(geometry_predicate_profile=False),dict(geometry_fixed16_effects_profile=1)):
            with self.assertRaises(RawShaderError):
                analyze_pixel_sample_dataflow(pixel(),**(OPTIONS|extra))
        for instruction in (predicated(scalar_alu(30)),scalar_alu(29),scalar_alu(31)):
            with self.assertRaises(RawShaderError):
                analyze_pixel_sample_dataflow(shader([instruction,alu()]),**OPTIONS)
        for extra in (dict(geometry_fixed16_effects_profile=False),dict(geometry_mrt_formats=(0,None,None,None)),
                      dict(geometry_depth_color_witness=True),dict(geometry_parameter_register=0),
                      dict(geometry_blend_controls=(0x10001,)*4),dict(geometry_centroid_mask=1)):
            with self.assertRaises(RawShaderError):
                translate_inline_pair(vertex(),pixel(),**(TRANSLATE|extra))


if __name__=='__main__':unittest.main()
