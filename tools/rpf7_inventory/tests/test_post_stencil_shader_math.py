"""Authored arithmetic only: never native draw, texture sampling or gameplay."""
from pathlib import Path
import math
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from pixel_sample_dataflow import analyze_pixel_sample_dataflow,evaluate_analyzed_pixel_dataflow
from raw_shader_analysis import RawShaderError
from test_pixel_sample_dataflow import pixel,coissue
from test_vertex_arithmetic_dataflow import math_alu

OPTIONS=dict(ui_operations=True,extended_math=True,loading_math=True,post_stencil_profile=True)


class PostStencilShaderMathTests(unittest.TestCase):
    def instruction(self,mask=15):
        # Operand1 encodes an otherwise invalid relative temporary. It is not
        # read by the unary opcode; operand0's consumed lanes still validate.
        return list(math_alu(op=8,dest=0,mask=mask,sources=(1,64,0),temps=(True,True,True)))

    def evaluate(self,inst,value):
        proof=analyze_pixel_sample_dataflow(pixel([inst]),**OPTIONS)
        return evaluate_analyzed_pixel_dataflow(proof,{0:[0]*4,1:value},{},{0:[0]*4})['0']

    def test_fraction_floors_negative_values_not_truncates(self):
        # Independent explicit expectations, not the production math helper.
        self.assertEqual(self.evaluate(self.instruction(),[-1.75,-.25,.25,2.75]),[.25,.75,.25,.75])
        self.assertEqual(self.evaluate(self.instruction(),[-2.,-0.,0.,16777216.]),[0.,0.,0.,0.])

    def test_masked_lanes_and_second_operand_are_not_read(self):
        self.assertEqual(self.evaluate(self.instruction(1),[-.25,None,None,None]),[.75,None,None,None])

    def test_source_modifiers_and_swizzle_precede_fraction(self):
        inst=self.instruction();inst[1]|=1<<26  # Source0 negation.
        self.assertEqual(self.evaluate(inst,[.25,.5,.75,1.]),[.75,.5,.25,0.])
        inst=self.instruction();inst[2]|=128<<16  # Absolute temporary source0.
        self.assertEqual(self.evaluate(inst,[-.25,-.5,-.75,-1.]),[.25,.5,.75,0.])
        inst=self.instruction();inst[1]|=0x55<<16  # Relative +1 per lane: Y,Z,W,X.
        self.assertEqual(self.evaluate(inst,[.25,.5,.75,1.]),[.5,.75,0.,.25])

    def test_coissue_reads_pre_fraction_input_and_preserves_disjoint_alpha(self):
        import struct
        inst=list(coissue(vmask=7,smask=8));inst[2]=(inst[2]&~(31<<24))|(8<<24)
        proof=analyze_pixel_sample_dataflow(pixel([inst,math_alu(op=2,dest=0,sources=(0,0,0),temps=(True,True,True))]),**OPTIONS)
        result=evaluate_analyzed_pixel_dataflow(proof,{0:[0]*4},
            {1:struct.pack('>4f',8,0,0,0)},{0:[-.25,2.5,-2.,4.]})
        self.assertEqual(result['0'],[.75,.5,0.,20.])

    def test_scalar_log_is_separately_opted_in_and_has_no_zero_fallback(self):
        inst=((16<<26)|(1<<15)|(15<<20),0,(2<<24)|(1<<29)|1)
        self.assertEqual(self.evaluate(inst,[0.,0.,0.,16.]),[4.]*4)
        for value in (0.,-1.):
            with self.assertRaises(RawShaderError):self.evaluate(inst,[0.,0.,0.,value])

    def test_unsupported_domains_and_consumed_relative_operand_reject(self):
        for value in (float('nan'),float('inf'),2**-149):
            with self.assertRaises(RawShaderError):self.evaluate(self.instruction(1),[value,None,None,None])
        inst=self.instruction();inst[2]|=64<<16
        with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(pixel([inst]),**OPTIONS)

    def test_legacy_profiles_stay_closed_and_no_pipeline_claim(self):
        for old in ({},dict(ui_operations=True),dict(ui_operations=True,extended_math=True,loading_math=True),
                    dict(ui_operations=True,extended_math=True,loading_math=True,rectangle_mrt_profile=True,tonemap_profile=True)):
            with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(pixel([self.instruction()]),**old)
        proof=analyze_pixel_sample_dataflow(pixel([self.instruction()]),**OPTIONS)
        self.assertTrue(proof['post_stencil_profile'])
        for field in ('native_fragment_pipeline','gpu_executed','sampling_executed','hardware_float_edge_cases_validated'):
            self.assertFalse(proof[field])

    def test_profile_combinations_and_unrelated_opcodes_reject(self):
        for other in ('geometry_profile','predicate_profile','rectangle_mrt_profile','tonemap_profile',
                      'cube_profile','composite_profile','explicit_lod_profile','rectangle_depth_profile'):
            with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(pixel([self.instruction()]),**{**OPTIONS,other:True})
        for op in (9,10,13,18,25):
            inst=math_alu(op=op,dest=0,sources=(1,1,1),temps=(True,True,True))
            with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(pixel([inst]),**OPTIONS)


if __name__=='__main__':unittest.main()
