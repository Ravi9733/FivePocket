from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from pixel_sample_dataflow import (analyze_pixel_sample_dataflow,evaluate_pixel_sample_dataflow,
    translate_pixel_sample_compute)
from raw_shader_analysis import RawShaderError,analyze_copy_shader
from test_raw_shader_analysis import cf_alloc,cf_exec,program,alu
from test_vertex_arithmetic_dataflow import math_alu
from test_scalar_mul_const import scalar_instruction


def texture(src=0,dst=0,slot=2,swizzle=0x688):
    return 1|(src<<5)|(dst<<12)|(1<<19)|(slot<<20)|(1<<28),0x1F1FF000|swizzle,0x4000


def pixel(instructions=None,fetch=None):
    instructions=instructions or [math_alu(op=1,dest=0,sources=(0,1,0),temps=(True,True,True))]
    return program([cf_exec(address=2,sequence=1,opcode=1),cf_alloc(),cf_exec(address=3,count=len(instructions)),0],
        [fetch or texture(),*instructions])


def coissue(vdest=0,sdest=0,vmask=3,smask=4):
    a,b,c=math_alu(op=1,dest=vdest,mask=vmask,sources=(0,0,0),temps=(True,False,False),export=False)
    sa,sb,sc=scalar_instruction(constant=1,ta=1,destination=sdest,mask=smask)
    return (a&0x000F003F)|sa,b|sb,(c&0xDFFFFF00)|sc&0x200000FF


class PixelSampleDataflowTests(unittest.TestCase):
    def test_ui_vector_source_negation_uses_independent_source_bits(self):
        # MAD's three operands must each negate independently, after swizzling.
        for bit,expected in ((26,-1),(25,-1),(24,1)):
            instruction=list(math_alu(op=11,dest=0,sources=(1,2,3),temps=(True,True,True)))
            instruction[1]|=1<<bit
            code=pixel([instruction])
            initial={0:[0,0,None,None],1:[2]*4,2:[3]*4,3:[5]*4}
            self.assertEqual(evaluate_pixel_sample_dataflow(code,initial,{}, {0:[0]*4},ui_operations=True)['0'],[expected]*4)
            with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(code)

    def test_ui_scalar_negation_and_unsupported_modifier_guards(self):
        # Scalar MAX-copy selects the same source W twice with swizzle 3.
        instruction=((5<<26)|(1<<15)|(15<<20),(1<<24)|3,(2<<24)|(1<<29)|1)
        result=evaluate_pixel_sample_dataflow(pixel([instruction]),{0:[0]*4,1:[2,3,4,10]},
            {},{0:[0]*4},ui_operations=True)
        self.assertEqual(result['0'],[-10]*4)
        for bit in range(27,32):
            changed=list(math_alu(op=0,dest=0));changed[1]|=1<<bit
            with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(pixel([changed]),ui_operations=True)

    def test_ui_mad_saturation_and_comparison(self):
        instruction=list(math_alu(op=11,dest=0,sources=(1,0,1),temps=(True,False,False)))
        instruction[0]|=1<<24
        constants={0:struct.pack('>4f',2,.25,4,0),1:struct.pack('>4f',-1,.5,2,0)}
        initial={0:[0,0,None,None],1:[2,3,-1,4]}
        code=pixel([instruction])
        self.assertEqual(evaluate_pixel_sample_dataflow(code,initial,constants,{0:[0]*4},ui_operations=True)['0'],[1,1,0,0])
        with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(code)
        code=pixel([math_alu(op=7,dest=0,sources=(1,0,0),temps=(True,False,False))])
        self.assertEqual(evaluate_pixel_sample_dataflow(code,initial,constants,{0:[0]*4},ui_operations=True)['0'],[0,1,1,1])

    def test_ui_scalar_multiply_uses_source_w_and_x(self):
        instruction=((2<<26)|(1<<15)|(15<<20),0,(2<<24)|(1<<29)|1)
        result=evaluate_pixel_sample_dataflow(pixel([instruction]),{0:[0,0,None,None],1:[2,3,4,10]},
            {},{0:[0]*4},ui_operations=True)
        self.assertEqual(result['0'],[20]*4)

    def test_sample_swizzle_and_tint(self):
        code=pixel(fetch=texture(swizzle=0x458))  # X,W,Y,Z
        result=evaluate_pixel_sample_dataflow(code,{0:[.25,.75,None,None],1:[2,3,4,5]}, {},{0:[7,11,13,17]})
        self.assertEqual(result,{'0':[14,51,44,65]})
        proof=analyze_pixel_sample_dataflow(code)
        self.assertFalse(proof['sampling_executed']);self.assertFalse(proof['interpolation_verified'])
        with self.assertRaises(RawShaderError):analyze_copy_shader(code,'pixel')

    def test_coissue_reads_prestate_and_combines_disjoint_masks(self):
        code=pixel([coissue(),alu()])
        constants={0:struct.pack('>4f',2,3,4,5),1:struct.pack('>4f',7,0,0,0)}
        result=evaluate_pixel_sample_dataflow(code,{0:[0,0,None,None]},constants,{0:[11,13,17,19]})
        self.assertEqual(result['0'],[22,39,91,19])  # Scalar reads old13, not vector39.
        code=pixel([coissue(vdest=2,sdest=3),alu(source=3)])
        result=evaluate_pixel_sample_dataflow(code,{0:[0,0,None,None],3:[1,2,3,4]},constants,{0:[11,13,17,19]})
        self.assertEqual(result['0'],[1,2,91,4])

    def test_zero_mask_scalar_updates_previous_and_keep_unknown_lanes(self):
        scalar=scalar_instruction(constant=1,mask=0)
        code=pixel([scalar,alu(vector=0,scalar=15)])
        self.assertEqual(evaluate_pixel_sample_dataflow(code,{0:[0,0,None,None]},
            {1:struct.pack('>4f',7,0,0,0)},{0:[11,13,17,19]})['0'],[91]*4)
        partial=pixel([alu(vector=3)])
        self.assertEqual(evaluate_pixel_sample_dataflow(partial,{0:[0,0,None,None]}, {},{0:[11,13,17,19]})['0'],
            [11,13,None,None])

    def test_rejection_guards_and_overwritten_bad_math(self):
        for changed in (coissue(smask=1),math_alu(op=11,dest=0),alu(vector=0,scalar=1)):
            with self.assertRaises(ValueError):analyze_pixel_sample_dataflow(pixel([changed]))
        base=texture()
        for word,bit in ((0,11),(0,18),(0,19),(0,25),(1,12),(1,21),(1,28),(1,29),(1,31),(2,0),(2,16)):
            changed=list(base);changed[word]^=1<<bit
            with self.assertRaises(ValueError):analyze_pixel_sample_dataflow(pixel(fetch=changed))
        for initial,samples in (({}, {0:[1]*4}),({0:[0,0,0,0]},{}),
            ({0:[float('nan'),0,0,0]}, {0:[1]*4}),({0:[0,0,0,0]}, {0:[float('inf')]*4}),
            ({0:[0,0,0,0],1:[1]*4}, {0:[0.1]*4})):
            with self.assertRaises(ValueError):evaluate_pixel_sample_dataflow(pixel(),initial,{},samples)
        code=pixel([scalar_instruction(constant=1,mask=0),alu()])
        with self.assertRaises(ValueError):evaluate_pixel_sample_dataflow(code,{0:[0,0,0,0]},
            {1:struct.pack('>4f',2**127,0,0,0)},{0:[4]*4})

    def test_compute_is_explicit_sample_only_and_guarded(self):
        source,proof=translate_pixel_sample_compute(pixel())
        self.assertEqual((proof['input_bytes'],proof['output_bytes']),(192,96))
        self.assertTrue(proof['sample_result_override']);self.assertFalse(proof['sampling_executed'])
        self.assertIn('index>=4u',source);self.assertIn('precise float',source)
        self.assertNotIn('sampler2D',source);self.assertNotIn('texture(',source)
        with self.assertRaises(ValueError):translate_pixel_sample_compute(pixel([alu(vector=3)]))


if __name__=='__main__':unittest.main()
