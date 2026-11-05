"""Xenos DOT2ADD vertex producer -> expression -> evaluator/GLSL checks."""
import struct
import unittest
from test_vertex_predication import OPTIONS, TRANSLATE, pixel, guarded, setp
from test_extended_shader_math import vertex_code
from test_vertex_arithmetic_dataflow import math_alu
from test_raw_shader_analysis import alu
from raw_shader_analysis import RawShaderError
from vertex_arithmetic_dataflow import analyze_vertex_dataflow, evaluate_vertex_dataflow
from inline_shader_translation import translate_inline_pair

class VertexDot2AddTests(unittest.TestCase):
    def evaluate(self, code, source=(2.,3.,101.,103.), constants=None):
        return evaluate_vertex_dataflow(code,{95:struct.pack('>4f',*source)},
            {i:struct.pack('>4f',*v) for i,v in (constants or {0:(5.,7.,107.,109.),1:(11.,13.,113.,127.)}).items()},0,**OPTIONS)

    def test_xy_products_plus_swizzled_third_x_and_partial_exports(self):
        for mask in (1,2,5,15):
            for swizzle,addend in ((0,11),(1,13),(2,113),(3,127)):
                ins=math_alu(op=17,dest=62,mask=mask,swizzles=(0,0,swizzle))
                code=vertex_code(ins,tail=[alu()])
                out=self.evaluate(code)['62']
                self.assertEqual(out,[31.+addend if mask&(1<<i) else None for i in range(4)])

    def test_read_before_write_and_vector_mask(self):
        ins=math_alu(op=17,dest=0,mask=3,sources=(0,0,0),temps=(True,True,True),export=False)
        self.assertEqual(self.evaluate(vertex_code(ins))['62'],[15.,15.,101.,103.])

    def test_predicate_and_unfused_expression_reach_glsl(self):
        ins=math_alu(op=17,dest=0,mask=3,export=False)
        code=vertex_code(setp(),[guarded(ins),alu(dest=62),alu()])
        for w,expected in ((0.,[2.,3.,101.,0.]),(1.,[42.,42.,101.,1.])):
            self.assertEqual(self.evaluate(code,source=(2.,3.,101.,w))['62'],expected)
        vs,_,proof=translate_inline_pair(code,pixel(),**TRANSLATE,geometry_mrt_formats=(0,None,None,None))
        self.assertEqual(proof['abi_version'],14)
        self.assertIn('precise',vs)
        self.assertTrue(any(n[0]=='add' for n in proof['vertex']['nodes']))

    def test_default_profile_relative_sources_and_missing_constants_reject(self):
        code=vertex_code(math_alu(op=17,dest=0,mask=3,export=False))
        for options in ({},{**OPTIONS,'packed_geometry':False},{**OPTIONS,'loading_math':False}):
            with self.assertRaises(RawShaderError):analyze_vertex_dataflow(code,**options)
        with self.assertRaises(RawShaderError):
            self.evaluate(code,constants={0:(5.,7.,107.,109.)})
        bad=vertex_code(math_alu(op=17,sources=(64,0,1)))
        with self.assertRaises(RawShaderError):analyze_vertex_dataflow(bad,**OPTIONS)

if __name__=='__main__':unittest.main()
