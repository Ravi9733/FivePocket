"""Authored bytecode and pixels; no private game source or live GPU claim."""
from pathlib import Path
import math
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from expanded16_max_reduce import (expanded16_binding, analyze_max_reduce,
    translate_max_reduce, linear_sample_reference, ROUNDING_OFFSET)
from pixel_sample_dataflow import evaluate_analyzed_pixel_dataflow
from raw_shader_analysis import RawShaderError
from unorm8_vertex_fetch import f32
from test_raw_shader_analysis import program, cf_exec, cf_alloc, alu
from test_raw_vertex_fetch_analysis import fetch
from test_pixel_sample_dataflow import texture
from test_vertex_arithmetic_dataflow import math_alu

FETCH = (0x85004956, 0x0100005D, 0x2CE27F, 0xA80D11, 3, 0x210)


def vertex():
    return program([cf_exec(address=3, count=2, sequence=5, opcode=1), cf_alloc(1),
        cf_exec(address=5, opcode=1), cf_alloc(2), cf_exec(address=6), 0],
        [fetch(dst=1, slot=95, stride=6),
         fetch(dst=0, slot=95, stride=6, offset=4, fmt=37, swizzle=0xFC8, mini=True),
         alu(dest=62, source=1), alu(dest=0, source=0, vector=3)])


def pixel(offsets=((1, 1), (1, -1), (-1, -1), (-1, 1)), op=2):
    samples=[]
    for i, (x, y) in enumerate(offsets):
        a,b,c=texture(src=0, dst=3-i, slot=5, swizzle=0xE88)
        samples.append((a,b,c|((x&31)<<16)|((y&31)<<21)))
    instructions=[math_alu(op=op,dest=1,mask=7,sources=(1,0,0),temps=(True,True,True),export=False),
                  math_alu(op=op,dest=0,mask=7,sources=(3,2,0),temps=(True,True,True),export=False)]
    out=list(math_alu(op=op,dest=0,mask=15,sources=(1,0,0),temps=(True,True,True)))
    out[0]|=1<<14  # Export's unwritten/source-independent alpha is literal1.
    # Overlapping vector/scalar export masks select literal1 in the fourth
    # lane. A scalar-only mask would consume previous scalar (not initialized).
    out[0]|=8<<20
    return program([cf_exec(address=2,count=4,sequence=85,opcode=1),cf_alloc(),
                    cf_exec(address=6,count=3),0], samples+instructions+[out])


class Expanded16MaxReduceTests(unittest.TestCase):
    def test_measured_floor8_is_explicit_and_distinct_from_ideal(self):
        read=lambda x,y:[float(x),float(y),float(x+y),1.]
        binding=dict(width=2,height=2)
        ideal=linear_sample_reference([.5,.5],[-.5,-.5],binding,read)
        measured=linear_sample_reference([.5,.5],[-.5,-.5],binding,read,weight_policy='adreno-floor8')
        self.assertGreater(ideal[0],0)
        self.assertEqual(measured,[0,0,0,1])
        with self.assertRaises(RawShaderError):
            linear_sample_reference([.5,.5],[-.5,-.5],binding,read,weight_policy='guess')

    def test_original_dataflow_maxes_each_channel_not_average_or_luma(self):
        proof=analyze_max_reduce(pixel())
        value=evaluate_analyzed_pixel_dataflow(proof,{0:[.25,.5,None,None]}, {},
            {0:[1,9,2,100],1:[3,2,8,200],2:[7,1,4,300],3:[2,6,3,400]})
        self.assertEqual(value['0'],[7,9,8,1])
        self.assertFalse(proof['sampling_executed'])
        for op in (0,1,3):
            with self.assertRaises(RawShaderError):analyze_max_reduce(pixel(op=op))
        for offsets in (((1,1),)*4,((1,1),(1,-1),(-1,-1),(-1,3))):
            with self.assertRaises(RawShaderError):analyze_max_reduce(pixel(offsets))

    def test_original_vertex_and_offset_policy_are_explicit_offline(self):
        vs,ps,proof=translate_max_reduce(vertex(),pixel(),FETCH)
        self.assertIn('originalVertex',vs)
        self.assertEqual(ps.count('textureLod('),4)
        self.assertIn('0.50146484375',ps)
        self.assertIn('-0.49853515625',ps)
        self.assertEqual(proof['texture_slots'],[5])
        self.assertTrue(proof['base_lod_only'])
        self.assertFalse(proof['native_game_pipeline'])
        self.assertFalse(proof['gpu_executed'])

    def test_binding_rejects_semantic_and_extent_changes(self):
        binding=expanded16_binding(FETCH)
        self.assertEqual((binding['width'],binding['height']),(640,360))
        for index,bit in ((0,2),(0,10),(0,13),(1,0),(1,6),(2,26),
                          (3,0),(3,13),(3,19),(3,21),(4,6),(5,12)):
            edited=list(FETCH);edited[index]^=1<<bit
            with self.assertRaises(RawShaderError):expanded16_binding(edited)
        for base in (0,0x20000000,0xA1000000):
            edited=list(FETCH);edited[1]=base|0x5D
            with self.assertRaises(RawShaderError):expanded16_binding(edited)
        edited=list(FETCH);edited[0]&=~(511<<22)
        with self.assertRaises(RawShaderError):expanded16_binding(edited)

    def test_linear_sampling_clamps_and_does_not_become_point_sampling(self):
        binding=dict(width=2,height=2)
        calls=[]
        def read(x,y):
            calls.append((x,y));return [float(x),float(y),float(x+y),1.]
        value=linear_sample_reference([.5,.5],[-.5,-.5],binding,read)
        self.assertGreater(value[0],0.)
        self.assertLess(value[0],.01)
        self.assertAlmostEqual(value[0],ROUNDING_OFFSET)
        self.assertEqual(value[2],f32(value[0]*2))
        self.assertEqual(value[3],1.)
        self.assertEqual(set(calls),{(0,0),(0,1),(1,0),(1,1)})
        calls.clear()
        self.assertEqual(linear_sample_reference([-8.,-8.],[-.5,-.5],binding,read),[0,0,0,1])
        self.assertEqual(set(calls),{(0,0)})
        self.assertEqual(linear_sample_reference([8.,8.],[.5,.5],binding,read),[1,1,2,1])
        for value in (math.inf,math.nan,.1):
            with self.assertRaises(RawShaderError):linear_sample_reference([value,0.],[-.5,.5],binding,read)
        with self.assertRaises(RawShaderError):
            linear_sample_reference([0.,0.],[-.5,.5],binding,lambda x,y:[math.nan]*4)


if __name__=='__main__':unittest.main()
