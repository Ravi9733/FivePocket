from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from scalar_constant import decode_scalar_constant
from raw_shader_analysis import RawShaderError
from vertex_arithmetic_dataflow import analyze_vertex_dataflow, evaluate_vertex_dataflow
from pixel_sample_dataflow import analyze_pixel_sample_dataflow, evaluate_pixel_sample_dataflow
from test_raw_shader_analysis import program, cf_alloc, cf_exec, alu
from test_raw_vertex_fetch_analysis import fetch
from test_vertex_arithmetic_dataflow import math_alu
from test_scalar_mul_const import scalar_instruction
from test_pixel_sample_dataflow import pixel


def scalar(opcode=44, **kwargs):
    a,b,c=scalar_instruction(**kwargs)
    return (a & ((1<<26)-1)) | ((opcode+(a>>26)%2)<<26), b, c


def coissued(opcode=44, *, smask=2, sdest=0):
    a,b,c=math_alu(op=1,dest=0,mask=1,sources=(0,0,0),temps=(True,False,False),export=False)
    sa,sb,sc=scalar(opcode,constant=1,ta=1,destination=sdest,mask=smask)
    return (a&0x000F003F)|sa,b|sb,(c&0xDFFFFF00)|(sc&0x200000FF)


def vertex_code(instruction, tail=None):
    tail=tail or [alu(dest=62,source=0),alu(source=0)]
    return program([cf_exec(address=3,sequence=1,opcode=1),cf_alloc(1),
                    cf_alloc(2),cf_exec(address=4,count=1+len(tail)),0,0],
                   [fetch(dst=0,slot=95,fmt=38,stride=4,swizzle=0x688),instruction,*tail])


class ExtendedShaderMathTests(unittest.TestCase):
    def test_all_scalar_constant_register_axes_and_operations(self):
        for opcode,operation in ((42,'mul'),(44,'add'),(46,'sub')):
            for register in range(64):
                for ca in range(4):
                    for ta in range(4):
                        p=decode_scalar_constant(scalar(opcode,register=register,constant=255,ca=ca,ta=ta))
                        self.assertEqual((p['operation'],p['temporary_register'],p['constant_register'],
                                          p['constant_component'],p['temporary_component']),
                                         (operation,register,255,ca,ta))
                        self.assertFalse(p['gpu_executed'])

    def test_scalar_constant_modifiers_remain_strict(self):
        base=scalar()
        for word,bit in ((0,6),(0,7),(0,14),(0,15),(0,25),
                         (1,27),(1,28),(1,29),(1,30),(1,31)):
            changed=list(base);changed[word]|=1<<bit
            with self.assertRaises(RawShaderError):decode_scalar_constant(changed,coissued=True)
        for bit in (24,25,26):
            changed=list(base);changed[1]|=1<<bit
            with self.assertRaises(RawShaderError):decode_scalar_constant(changed)
            p=decode_scalar_constant(changed,coissued=True)
            self.assertEqual(p['negate_both'],bit==24)
        for bad in ([True,0,0],[-1,0,0],[1<<32,0,0],[],[0,0]):
            with self.assertRaises(RawShaderError):decode_scalar_constant(bad)

    def test_vertex_coissue_reads_prestate_and_preserves_other_lanes(self):
        constants={0:struct.pack('>4f',11,13,17,19),1:struct.pack('>4f',17,0,0,0)}
        for opcode,expected in ((42,51),(44,20),(46,14)):
            code=vertex_code(coissued(opcode))
            with self.assertRaises(RawShaderError):analyze_vertex_dataflow(code)
            out=evaluate_vertex_dataflow(code,{95:struct.pack('>4f',2,3,5,7)},constants,0,scalar_constants=True)
            self.assertEqual(out['62'],[22,expected,5,7])
            self.assertEqual(out['0'],out['62'])
        with self.assertRaises(RawShaderError):
            analyze_vertex_dataflow(vertex_code(coissued(smask=1)),scalar_constants=True)

    def test_vertex_zero_mask_updates_scalar_and_retained_export(self):
        code=vertex_code(scalar(46,constant=1,ta=1,mask=0),
                         [alu(dest=62,vector=0,scalar=15),alu()])
        out=evaluate_vertex_dataflow(code,{95:struct.pack('>4f',2,3,5,7)},
             {1:struct.pack('>4f',17,0,0,0)},0,scalar_constants=True)
        self.assertEqual(out['62'],[14]*4)
        self.assertEqual(out['0'],[2,3,5,7])

    def test_pixel_constant_negation_affects_both_operands_only(self):
        constants={1:struct.pack('>4f',17,0,0,0)}
        for opcode,expected in ((42,51),(44,-20),(46,-14)):
            instruction=list(scalar(opcode,constant=1,ta=1,mask=0))
            instruction[1]|=1<<24
            code=pixel([instruction,alu(vector=0,scalar=15)])
            out=evaluate_pixel_sample_dataflow(code,{0:[0]*4},constants,{0:[2,3,5,7]},
                                              ui_operations=True,extended_math=True)
            self.assertEqual(out['0'],[expected]*4)
            with self.assertRaises(ValueError):analyze_pixel_sample_dataflow(code,ui_operations=True)

    def test_dot2add_uses_xy_and_third_source_x_broadcast_with_prestate(self):
        code=pixel([math_alu(op=17,dest=0,sources=(1,2,3),temps=(True,True,True))])
        initial={0:[0]*4,1:[2,3,101,103],2:[5,7,107,109],3:[11,113,127,131]}
        out=evaluate_pixel_sample_dataflow(code,initial,{}, {0:[0]*4},ui_operations=True,extended_math=True)
        self.assertEqual(out['0'],[42]*4)
        with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(code,ui_operations=True)

    def test_reciprocal_policy_and_negative_edge_cases(self):
        instruction=((19<<26)|(1<<15)|(15<<20),0,(2<<24)|(1<<29)|1)
        code=pixel([instruction])
        for value,expected in ((1,1),(-1,-1),(2,.5),(-2,-.5),(2**-126,2**126)):
            out=evaluate_pixel_sample_dataflow(code,{0:[0]*4,1:[31,37,41,value]}, {},{0:[0]*4},
                                              ui_operations=True,extended_math=True)
            self.assertEqual(out['0'],[expected]*4) # Scalar W, not X.
        for value in (0.,-0.,2**-149,2**127,float('inf'),float('nan')):
            with self.assertRaises(RawShaderError):
                evaluate_pixel_sample_dataflow(code,{0:[0]*4,1:[0,0,0,value]}, {},{0:[0]*4},
                                               ui_operations=True,extended_math=True)
        # Even an overwritten, zero-write-mask RCP must validate.
        no_write=((19<<26),0,(2<<24)|(1<<29)|1)
        with self.assertRaises(RawShaderError):
            evaluate_pixel_sample_dataflow(pixel([no_write,alu()]),{0:[0]*4,1:[0]*4},{},
                                            {0:[1]*4},ui_operations=True,extended_math=True)
        with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(code,extended_math=True)


if __name__=='__main__':unittest.main()
