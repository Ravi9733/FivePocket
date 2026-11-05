"""Original RECIP_IEEE -> SM3 zero-multiply chain; finite output is mandatory."""
import math
import struct
import unittest
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from inline_shader_translation import translate_inline_pair
from pixel_sample_dataflow import evaluate_analyzed_pixel_dataflow
from screen_strip_shader_policy import screen_strip_zero_reciprocal,screen_strip_zero_infinity_product
from raw_shader_analysis import RawShaderError
from test_screen_strip_translation import vertex,OPTIONS
from test_pixel_sample_dataflow import pixel,texture
from test_vertex_arithmetic_dataflow import math_alu

class ScreenStripNumericTests(unittest.TestCase):
    def shader(self,final_reciprocal=False):
        reciprocal=((19<<26)|(5<<8)|(15<<20),0,(2<<24)|(1<<29)|4)
        product=math_alu(op=2 if final_reciprocal else 1,dest=0,
            sources=(5,5 if final_reciprocal else 0,0),temps=(True,final_reciprocal,False))
        return translate_inline_pair(vertex(),pixel([reciprocal,product],texture(dst=4,slot=3)),**OPTIONS)

    def test_signed_zero_is_infinity_not_clamped_zero(self):
        for z in (0.,-0.):
            result=screen_strip_zero_reciprocal(z)
            self.assertTrue(math.isinf(result));self.assertEqual(math.copysign(1,result),math.copysign(1,z))
        for bad in (1.,-1.,math.inf,math.nan,2**-149):
            with self.assertRaises(RawShaderError):screen_strip_zero_reciprocal(bad)

    def test_legacy_zero_infinity_product_is_positive_zero(self):
        for zero in (0.,-0.):
            for infinity in (math.inf,-math.inf):
                for a,b in ((zero,infinity),(infinity,zero)):
                    result=screen_strip_zero_infinity_product(a,b)
                    self.assertEqual(result,0);self.assertEqual(math.copysign(1,result),1)
        for operands in ((1.,math.inf),(0.,math.nan),(0.,1.),(2**-149,math.inf)):
            with self.assertRaises(RawShaderError):screen_strip_zero_infinity_product(*operands)

    def test_connected_reciprocal_then_multiply_preserves_finite_result(self):
        _,glsl,proof=self.shader()
        self.assertIn('screenStripReciprocal(sampled0[3])',glsl)
        for zero in (0.,-0.):
            args=(proof['pixel'],{0:[0.]*4,1:[0.]*4},{0:struct.pack('>4f',0,0,0,0)},{0:[zero]*4})
            with self.assertRaises(RawShaderError):evaluate_analyzed_pixel_dataflow(*args)
            self.assertEqual(evaluate_analyzed_pixel_dataflow(*args,screen_strip_ieee=True)['0'],[0.]*4)

    def test_profile_isolation_and_final_infinity_rejection(self):
        _,_,proof=self.shader(True)
        args=(proof['pixel'],{0:[0.]*4,1:[0.]*4},{},{0:[0.]*4})
        with self.assertRaises(RawShaderError):evaluate_analyzed_pixel_dataflow(*args,screen_strip_ieee=True)
        proof['pixel']['screen_strip_profile']=False
        with self.assertRaises(RawShaderError):evaluate_analyzed_pixel_dataflow(*args,screen_strip_ieee=True)
        with self.assertRaises(RawShaderError):evaluate_analyzed_pixel_dataflow(*args,screen_strip_ieee=1)

if __name__=='__main__':unittest.main()
