"""Authored bytecode; exact arithmetic and sampling checks, not gameplay proof."""
import math
import struct
import unittest
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from r32_scalar_reduce import (OFFSETS, analyze_scalar_reduce,
    r32_point_binding, point_sample_reference, translate_scalar_reduce)
from pixel_sample_dataflow import evaluate_analyzed_pixel_dataflow
from raw_shader_analysis import RawShaderError
from test_expanded16_max_reduce import vertex
from test_raw_shader_analysis import program, cf_exec, cf_alloc
from test_pixel_sample_dataflow import texture
from test_vertex_arithmetic_dataflow import math_alu

FETCH = (0x80804956, 0x010000A4, 0x7E03F, 0x16D1, 0, 0x210)


def pixel(offsets=None, slot=0, constant=255, repeated=False):
    instructions = []
    for i, (x, y) in enumerate(sorted(OFFSETS) if offsets is None else offsets):
        a, b, c = texture(src=0, dst=i+1, slot=slot, swizzle=0)
        instructions.append((a, b, c | ((int(x*2)&31)<<16) | ((int(y*2)&31)<<21)))
    for i in range(2, 17):
        instructions.append(math_alu(op=0, dest=1, sources=(1, 1 if repeated else i, 0),
            temps=(True, True, True), export=False))
    # Relative swizzle maps all four lanes to x, including constant x.
    instructions.append(math_alu(op=1, dest=0, sources=(1,constant,0),
        temps=(True,False,True), swizzles=(0x6C,0x6C,0)))
    return program([cf_exec(address=4,count=6,sequence=1365,opcode=1),
        cf_exec(address=10,count=6,sequence=1365,opcode=1),
        cf_exec(address=16,count=4,sequence=85,opcode=1),cf_alloc(),
        cf_exec(address=20,count=6,opcode=1),cf_exec(address=26,count=6,opcode=1),
        cf_exec(address=32,count=4),0], instructions)


class R32ScalarTests(unittest.TestCase):
    def test_sixteen_red_taps_and_dynamic_scale(self):
        proof = analyze_scalar_reduce(pixel())
        samples = {i:[float(i+1),500.,1000.,2000.] for i in range(16)}
        for scale in (0.,.0625,.125,1.,-1.):
            result = evaluate_analyzed_pixel_dataflow(proof,{0:[.5,.5,None,None]},
                {255:struct.pack('>4f',scale,32.,64.,128.)},samples)
            self.assertEqual(result['0'],[136*scale]*4)

    def test_original_vertex_runtime_constants_and_offsets(self):
        vs, ps, proof = translate_scalar_reduce(vertex(), pixel(), FETCH)
        self.assertIn('originalVertex',vs)
        self.assertEqual(ps.count('textureLod('),16)
        self.assertIn('-1.49853515625',ps)
        self.assertIn('1.50146484375',ps)
        self.assertIn('uintBitsToFloat(readBE(2048u))',ps)
        self.assertFalse(proof['native_game_pipeline'])
        self.assertFalse(proof['gpu_executed'])

    def test_other_profiles_and_missing_taps_rejected(self):
        for data in (pixel(offsets=[(-.5,.5)]*16),pixel(slot=1),
                     pixel(constant=254),pixel(repeated=True)):
            with self.assertRaises(RawShaderError):analyze_scalar_reduce(data)

    def test_numeric_format_filter_and_extent_are_not_interchangeable(self):
        self.assertTrue(r32_point_binding(FETCH)['point'])
        for index,bit in ((0,2),(0,22),(1,0),(1,6),(2,0),(3,19),(4,0),(5,4)):
            changed=list(FETCH);changed[index]^=1<<bit
            with self.assertRaises(RawShaderError):r32_point_binding(changed)
        for base in (0,0x20000000,0xA0000000):
            changed=list(FETCH);changed[1]=base|0xA4
            with self.assertRaises(RawShaderError):r32_point_binding(changed)

    def test_point_reference_and_edges(self):
        b=r32_point_binding(FETCH)
        read=lambda x,y:[float(x),float(y),1.,1.]
        self.assertEqual(point_sample_reference([.5,.5],[-.5,-.5],b,read),[31.,31.,1.,1.])
        self.assertEqual(point_sample_reference([.5,.5],[1.5,1.5],b,read),[33.,33.,1.,1.])
        self.assertEqual(point_sample_reference([-8.,-8.],[-1.5,-1.5],b,read),[0.,0.,1.,1.])
        self.assertEqual(point_sample_reference([8.,8.],[1.5,1.5],b,read),[63.,63.,1.,1.])
        for value in (math.inf,math.nan,.1):
            with self.assertRaises(RawShaderError):point_sample_reference([value,0.],[.5,.5],b,read)

    def test_second_scalar_stage_uses_runtime_texture_extent(self):
        fetch=(0x80404956,0x010000A4,0x1E00F,0x16D1,0,0x210)
        vs,ps,proof=translate_scalar_reduce(vertex(),pixel(),fetch)
        old_vs,old_ps,_=translate_scalar_reduce(vertex(),pixel(),FETCH)
        self.assertEqual((vs,ps),(old_vs,old_ps))
        b=proof['binding'];self.assertEqual((b['width'],b['height']),(16,16))
        read=lambda x,y:[float(x),float(y),1.,1.]
        self.assertEqual(point_sample_reference([.5,.5],[-.5,-.5],b,read),[7.,7.,1.,1.])
        self.assertEqual(point_sample_reference([.5,.5],[1.5,1.5],b,read),[9.,9.,1.,1.])
        self.assertEqual(point_sample_reference([8.,8.],[1.5,1.5],b,read),[15.,15.,1.,1.])
        for mixed in ((FETCH[0],*fetch[1:]),(fetch[0],fetch[1],FETCH[2],*fetch[3:])):
            with self.assertRaises(RawShaderError):r32_point_binding(mixed)

    def test_final_scalar_stage_covers_every_source_pixel_once(self):
        fetch=(0x80404956,0x0656B0A4,0x6003,0x16D1,0,0x210)
        vs,ps,proof=translate_scalar_reduce(vertex(),pixel(),fetch)
        self.assertEqual((vs,ps),translate_scalar_reduce(vertex(),pixel(),FETCH)[:2])
        b=proof['binding'];self.assertEqual((b['width'],b['height']),(4,4))
        read=lambda x,y:[float(x),float(y),1.,1.]
        taps=[point_sample_reference([.5,.5],o,b,read) for o in OFFSETS]
        self.assertEqual({tuple(t[:2]) for t in taps},{(float(x),float(y)) for y in range(4) for x in range(4)})
        self.assertEqual(point_sample_reference([-8.,-8.],[-1.5,-1.5],b,read),[0.,0.,1.,1.])
        self.assertEqual(point_sample_reference([8.,8.],[1.5,1.5],b,read),[3.,3.,1.,1.])
        for extent in (0,0x2001,0xE007,0x3E01F):
            bad=list(fetch);bad[2]=extent
            with self.assertRaises(RawShaderError):r32_point_binding(bad)


if __name__=='__main__':unittest.main()
