"""Explicit ABI32 RECIP_IEEE -> SM3 zero-product reference, not GPU admission."""
import math
import struct
import unittest
from test_geometry_lod_translation import OPTIONS
from test_geometry_shader_translation import vertex
from test_pixel_sample_dataflow import pixel,texture
from test_vertex_arithmetic_dataflow import math_alu
from inline_shader_translation import translate_inline_pair
from pixel_sample_dataflow import evaluate_analyzed_pixel_dataflow
from raw_shader_analysis import RawShaderError


class GeometryLodNumericTests(unittest.TestCase):
    def shader(self,opcode=19,final_reciprocal=False):
        reciprocal=((opcode<<26)|(5<<8)|(15<<20),0,(2<<24)|(1<<29)|4)
        product=math_alu(op=2 if final_reciprocal else 1,dest=0,
            sources=(5,5 if final_reciprocal else 0,0),temps=(True,final_reciprocal,False))
        return translate_inline_pair(vertex(),pixel([reciprocal,product],texture(dst=4,slot=0)),**OPTIONS)

    def test_connected_signed_zero_chain(self):
        _,glsl,p=self.shader()
        self.assertIn('(1.0/sampled0[3])',glsl)
        for zero in (0.,-0.):
            args=(p['pixel'],{0:[0.]*4},{0:struct.pack('>4f',0,0,0,0)},{0:[zero]*4})
            with self.assertRaises(RawShaderError):evaluate_analyzed_pixel_dataflow(*args)
            result=evaluate_analyzed_pixel_dataflow(*args,geometry_lod_ieee=True)['0']
            self.assertEqual(result,[0.]*4)
            self.assertTrue(all(math.copysign(1,x)==1 for x in result))

    def test_opt_in_does_not_admit_nan_subnormal_or_nonzero_times_infinity(self):
        _,_,p=self.shader()
        for sample,const in ((math.nan,0.),(math.inf,0.),(2**-149,0.),(0.,1.)):
            with self.assertRaises(RawShaderError):
                evaluate_analyzed_pixel_dataflow(p['pixel'],{0:[0.]*4},
                    {0:struct.pack('>4f',const,const,const,const)},{0:[sample]*4},geometry_lod_ieee=True)

    def test_wrong_profile_and_nonfinite_final_output_still_reject(self):
        _,_,p=self.shader(final_reciprocal=True)
        args=(p['pixel'],{0:[0.]*4},{},{0:[0.]*4})
        with self.assertRaises(RawShaderError):evaluate_analyzed_pixel_dataflow(*args,geometry_lod_ieee=True)
        with self.assertRaises(RawShaderError):evaluate_analyzed_pixel_dataflow(*args,geometry_lod_ieee=1)
        p['pixel']['geometry_lod_profile']=False
        with self.assertRaises(RawShaderError):evaluate_analyzed_pixel_dataflow(*args,geometry_lod_ieee=True)

    def test_clamp_reciprocal_opcodes_are_not_silently_treated_as_ieee(self):
        for opcode in (17,18):
            with self.assertRaises(RawShaderError):self.shader(opcode=opcode)

    def power_shader(self):
        logarithm=((16<<26)|(5<<8)|(15<<20),0,(2<<24)|(1<<29)|4)
        product=math_alu(op=1,dest=6,sources=(5,0,0),temps=(True,False,False),export=False)
        exponent=((14<<26)|(7<<8)|(15<<20),0,(2<<24)|(1<<29)|6)
        final=math_alu(op=2,dest=0,sources=(7,7,0),temps=(True,True,False))
        return translate_inline_pair(vertex(),pixel([logarithm,product,exponent,final],texture(dst=4,slot=0)),**OPTIONS)[2]['pixel']

    def test_connected_zero_log_product_exp(self):
        p=self.power_shader()
        for zero in (0.,-0.):
            for power,expected in ((.25,0.),(2.,0.),(0.,1.)):
                args=(p,{0:[0.]*4},{0:struct.pack('>4f',power,power,power,power)},{0:[zero]*4})
                with self.assertRaises(RawShaderError):evaluate_analyzed_pixel_dataflow(*args)
                self.assertEqual(evaluate_analyzed_pixel_dataflow(*args,geometry_lod_ieee=True)['0'],[expected]*4)

    def test_negative_log_is_not_clamped(self):
        p=self.power_shader()
        for sample in (-1.,-2.,math.nan,math.inf):
            with self.assertRaises(RawShaderError):evaluate_analyzed_pixel_dataflow(p,{0:[0.]*4},
                {0:struct.pack('>4f',2,2,2,2)},{0:[sample]*4},geometry_lod_ieee=True)

    def test_zero_to_negative_power_still_rejects_nonfinite_output(self):
        with self.assertRaises(RawShaderError):evaluate_analyzed_pixel_dataflow(self.power_shader(),{0:[0.]*4},
            {0:struct.pack('>4f',-1,-1,-1,-1)},{0:[0.]*4},geometry_lod_ieee=True)


if __name__=='__main__':unittest.main()
