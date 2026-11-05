"""Authored SM3 finite denormal-MUL -> real addressed-constant consumers.

ReX c94f5ebdcb3c9d1a460ca48e04f9758448f8d518 dxbc_translator_alu.cpp
kMul/kMad: zero or denormal multiplicand produces +0. Not a full FTZ profile.
"""
import math
import struct
import unittest
from test_vertex_address_register import evaluate,code,relative_copy,scalar_alu,TRANSLATE
from test_vertex_arithmetic_dataflow import math_alu
from test_geometry_gradient_depth import gradient
from test_pixel_sample_dataflow import pixel
from test_raw_shader_analysis import alu
from vertex_arithmetic_dataflow import arithmetic
from inline_shader_translation import translate_inline_pair
from raw_shader_analysis import RawShaderError

def addressed_multiply():
    # c5[a0] is consumed by MUL itself. A preceding MAX-copy would introduce a
    # separate, deliberately unsupported denormal operation before this test.
    a,b,c=math_alu(op=1,dest=2,sources=(5,0,0),temps=(False,True,True),export=False)
    return a,b|(1<<31)|(1<<29),c

class VertexDenormalMultiplyTests(unittest.TestCase):
    def test_signed_subnormal_input_produces_positive_zero(self):
        for bits in (1,0x180,0x7fffff,0x80000001,0x807fffff):
            tiny=struct.unpack('>f',struct.pack('>I',bits))[0]
            for normal in (1.,-1.,2.**100):
                for a,b in ((tiny,normal),(normal,tiny)):
                    value=arithmetic('mul',a,b,denormal_multiply=True)
                    self.assertEqual(struct.pack('>f',value),b'\0\0\0\0')
                    with self.assertRaises(RawShaderError):arithmetic('mul',a,b)

    def test_no_broader_nonfinite_or_underflow_acceptance(self):
        for invalid in (math.inf,-math.inf,math.nan):
            with self.assertRaises(RawShaderError):arithmetic('mul',0.,invalid,denormal_multiply=True)
        with self.assertRaises(RawShaderError):arithmetic('add',2.**-149,1.,denormal_multiply=True)
        with self.assertRaises(RawShaderError):arithmetic('mul',2.**-126,.5,denormal_multiply=True)
        with self.assertRaises(RawShaderError):arithmetic('mul',1.,2.,denormal_multiply=1)
        self.assertEqual(arithmetic('mul',2.**-126,1.,denormal_multiply=True),2.**-126)
        for tiny in (2.**-149,-2.**-149):
            with self.assertRaises(RawShaderError):
                evaluate([scalar_alu(23,mask=0),relative_copy(5)],(2.,4.,8.,0.),
                         constants={5:(tiny,1.,2.,1.)},denormal_multiply=True)

    def test_relative_constant_multiply_connection(self):
        ops=[scalar_alu(23,mask=0),addressed_multiply()]
        for tiny in (2.**-149,-2.**-149):
            kwargs=dict(export_source=2,constants={5:(tiny,1.,2.,1.)})
            with self.assertRaises(RawShaderError):evaluate(ops,(2.,4.,8.,0.),**kwargs)
            result=evaluate(ops,(2.,4.,8.,0.),**kwargs,denormal_multiply=True,include_state=True)
            self.assertEqual(result['exports']['62'],[0.,4.,16.,0.])
            self.assertEqual(result['resolved_relative_constants'],[5])

    def test_glsl_explicit_bits_and_nonfinite_fault_before_raster(self):
        shader=code([scalar_alu(23,mask=0),addressed_multiply()],export_source=2)
        ps=pixel([math_alu(op=2,dest=61,mask=1,sources=(0,0,0),temps=(True,True,True)),alu()],fetch=gradient())
        original,pixel_original,proof_original=translate_inline_pair(shader,ps,**TRANSLATE)
        vertex,pixel_new,proof=translate_inline_pair(shader,ps,**TRANSLATE,vertex_denormal_multiply_profile=True)
        self.assertNotIn('&0x7f800000u',original)
        self.assertNotIn('vertex_denormal_multiply_profile',proof_original)
        self.assertEqual(pixel_original,pixel_new)
        self.assertIn('&0x7f800000u',vertex)
        self.assertIn('atomicOr(geometryCompletion.words[4],8u)',vertex)
        self.assertTrue(proof['vertex_denormal_multiply_profile'])
        compute,_,_=translate_inline_pair(shader,ps,**TRANSLATE,vertex_denormal_multiply_profile=True,geometry_position_compute=True)
        self.assertIn('&0x7f800000u',compute);self.assertIn('0x7fc00000u',compute)
        for changes in ({'vertex_address_profile':False},{'vertex_denormal_multiply_profile':1}):
            with self.assertRaises(RawShaderError):translate_inline_pair(shader,ps,**{**TRANSLATE,'vertex_denormal_multiply_profile':True,**changes})

if __name__=='__main__':unittest.main()
