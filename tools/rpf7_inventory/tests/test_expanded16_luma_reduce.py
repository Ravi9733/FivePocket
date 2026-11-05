"""Authored shader/pixels only; not a live-game or hardware oracle."""
import math
import struct
import unittest
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from expanded16_luma_reduce import (OFFSETS, analyze_luma_reduce,
    expanded16_point_binding, point_sample_reference, translate_luma_reduce)
from pixel_sample_dataflow import evaluate_analyzed_pixel_dataflow
from raw_shader_analysis import RawShaderError
from test_expanded16_max_reduce import vertex
from test_raw_shader_analysis import program, cf_exec, cf_alloc
from test_pixel_sample_dataflow import texture
from test_vertex_arithmetic_dataflow import math_alu

FETCH = (0x82804956, 0x0100005D, 0x16613F, 0xD11, 0, 0x210)


def pixel(offsets=None, slot=0, export_scalar=True):
    instructions = []
    for i, (x, y) in enumerate(sorted(OFFSETS) if offsets is None else offsets):
        a, b, c = texture(src=0, dst=i+1, slot=slot, swizzle=0xE88)
        instructions.append((a, b, c | ((int(x*2)&31)<<16) | ((int(y*2)&31)<<21)))
    for i in range(2, 9):
        instructions.append(math_alu(op=0, dest=1, mask=7, sources=(1,i,0), temps=(True,True,True), export=False))
    instructions.append(math_alu(op=16 if export_scalar else 1, dest=0,
        sources=(1,255,0), temps=(True,False,True)))
    return program([cf_exec(address=3,count=6,sequence=1365,opcode=1),
        cf_exec(address=9,count=2,sequence=5,opcode=1), cf_alloc(),
        cf_exec(address=11,count=6,opcode=1), cf_exec(address=17,count=2),0], instructions)


class Expanded16LumaTests(unittest.TestCase):
    def test_eight_taps_reach_scalar_with_dynamic_constants(self):
        proof=analyze_luma_reduce(pixel())
        samples={i:[float(i+1),2.,4.,99.] for i in range(8)}
        for rgb in ((1.,0.,0.),(0.,1.,0.),(0.,0.,1.)):
            result=evaluate_analyzed_pixel_dataflow(proof,{0:[.5,.5,None,None]},
                {255:struct.pack('>4f',*rgb,1.)},samples)
            expected=36. if rgb[0] else 16. if rgb[1] else 32.
            self.assertEqual(result['0'],[expected]*4)
        self.assertFalse(proof['sampling_executed'])

    def test_shader_preserves_offsets_and_runtime_constants(self):
        vs,ps,proof=translate_luma_reduce(vertex(),pixel(),FETCH)
        self.assertIn('originalVertex',vs)
        self.assertEqual(ps.count('textureLod('),8)
        self.assertIn('1.50146484375',ps)
        self.assertIn('-1.49853515625',ps)
        self.assertIn('uintBitsToFloat(readBE(2048u))',ps)
        self.assertFalse(proof['native_game_pipeline'])
        self.assertFalse(proof['gpu_executed'])

    def test_wrong_taps_slot_and_output_refuse(self):
        for data in (pixel(offsets=[(-.5,.5)]*8),pixel(slot=1),pixel(export_scalar=False)):
            with self.assertRaises(RawShaderError): analyze_luma_reduce(data)

    def test_exact_submitted_point_storage_profile(self):
        self.assertTrue(expanded16_point_binding(FETCH)['point'])
        for index,bit in ((0,2),(0,22),(1,0),(1,6),(2,0),(3,19),(4,0),(5,4)):
            changed=list(FETCH);changed[index]^=1<<bit
            with self.assertRaises(RawShaderError):expanded16_point_binding(changed)
        for base in (0,0x20000000,0xA0000000):
            changed=list(FETCH);changed[1]=base|0x5D
            with self.assertRaises(RawShaderError):expanded16_point_binding(changed)

    def test_point_reference_clamps_and_never_blends(self):
        b=expanded16_point_binding(FETCH)
        read=lambda x,y:[float(x),float(y),1.,1.]
        self.assertEqual(point_sample_reference([.5,.5],[-.5,-.5],b,read),[159.,89.,1.,1.])
        self.assertEqual(point_sample_reference([.5,.5],[1.5,.5],b,read),[161.,90.,1.,1.])
        self.assertEqual(point_sample_reference([-8.,-8.],[-1.5,-.5],b,read),[0.,0.,1.,1.])
        self.assertEqual(point_sample_reference([8.,8.],[1.5,.5],b,read),[319.,179.,1.,1.])
        for value in (math.inf,math.nan,.1):
            with self.assertRaises(RawShaderError):point_sample_reference([value,0.],[.5,.5],b,read)


if __name__=='__main__':unittest.main()
