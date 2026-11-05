from pathlib import Path
import struct
import sys
import unittest
sys.path[:0]=[str(Path(__file__).resolve().parents[1]),str(Path(__file__).resolve().parents[2]/'vulkan_probe')]
from pixel_sample_dataflow import evaluate_pixel_sample_dataflow,translate_pixel_sample_compute
from generate_pixel_sample_probe import make_inputs
from test_pixel_sample_dataflow import pixel,texture
from test_raw_shader_analysis import cf_alloc,cf_exec,program
from test_vertex_arithmetic_dataflow import math_alu


def scalar(op,constant=0,mask=0,export=False,swizzle=0x1B):
    return op<<26|mask<<20|int(export)<<15,swizzle,2<<24|constant


def triple():
    return program([cf_exec(address=2,count=3,sequence=21,opcode=1),cf_alloc(),cf_exec(address=5,count=2),0],
        [texture(dst=2,slot=0),texture(dst=3,slot=1),texture(dst=4,slot=2),
         math_alu(op=0,dest=5,sources=(2,3,0),temps=(True,True,True),export=False),
         math_alu(op=0,dest=0,sources=(5,4,0),temps=(True,True,True))])


class PixelDotScalarTests(unittest.TestCase):
    def test_dp3_dp4_unfused_left_fold(self):
        for op,expected in ((16,38),(15,115)):
            code=pixel([math_alu(op=op,dest=0,sources=(0,0,0),temps=(True,False,False))])
            result=evaluate_pixel_sample_dataflow(code,{0:[0,0,None,None]},
                {0:struct.pack('>4f',2,3,5,7)},{0:[1,2,6,11]})['0']
            self.assertEqual(result,[expected]*4)
        code=pixel([math_alu(op=15,dest=0,sources=(0,0,0),temps=(True,False,False))])
        self.assertEqual(evaluate_pixel_sample_dataflow(code,{0:[0,0,None,None]},
            {0:struct.pack('>4f',1,1,1,1)},{0:[2**24,1,-2**24,1]})['0'],[1]*4)
        self.assertEqual(evaluate_pixel_sample_dataflow(code,{0:[0,0,None,None]},
            {0:struct.pack('>4f',1-2**-23,-1,1,-1)},{0:[1+2**-23,1,1,1]})['0'],[0]*4)

    def test_conditional_signed_zero_and_both_choices_guarded(self):
        code=pixel([math_alu(op=12,dest=0,sources=(0,0,1),temps=(True,False,False))])
        constants={0:struct.pack('>4f',2,3,5,7),1:struct.pack('>4f',11,13,17,19)}
        self.assertEqual(evaluate_pixel_sample_dataflow(code,{0:[0,0,None,None]},constants,
            {0:[0,-0.0,1,-1]})['0'],[2,3,17,19])
        constants[1]=struct.pack('>4f',float('nan'),0,0,0)
        with self.assertRaises(ValueError):evaluate_pixel_sample_dataflow(code,{0:[0,0,None,None]},constants,{0:[0]*4})

    def test_scalar_previous_updates_with_zero_mask_and_exports(self):
        code=pixel([scalar(5),scalar(3,constant=1,mask=15,export=True)])
        self.assertEqual(evaluate_pixel_sample_dataflow(code,{0:[0,0,None,None]},
            {0:struct.pack('>4f',1,1,1,3),1:struct.pack('>4f',1,1,1,2)},{0:[1]*4})['0'],[6]*4)
        for instructions in ([scalar(3,mask=15,export=True)],[scalar(5,swizzle=0),scalar(3,mask=15,export=True)]):
            with self.assertRaises(ValueError):translate_pixel_sample_compute(pixel(instructions))

    def test_three_explicit_samples_have_distinct_record_offsets(self):
        code=triple();source,proof=translate_pixel_sample_compute(code)
        self.assertEqual((proof['sample_count'],proof['record_bytes'],proof['input_bytes']),(3,80,320))
        self.assertIn('index*20u+16u',source)
        records=struct.pack('>20f',0,0,0,0,1,1,1,1,1,2,3,4,5,6,7,8,9,10,11,12)*4
        _,_,combined,expected=make_inputs(code,records,b'')
        self.assertEqual(combined,records)
        for i in range(4):self.assertEqual(struct.unpack_from('<4f',expected,16+i*16),(15,18,21,24))
        with self.assertRaises(ValueError):make_inputs(code,records[:-4],b'')


if __name__=='__main__':unittest.main()
