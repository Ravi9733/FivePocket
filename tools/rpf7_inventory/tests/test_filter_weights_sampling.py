from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from filter_weights_sampling import raw10_base_binding,fetch_reference,translate_filter_weights_compute,translate_filter_weights_fragment
from pixel_sample_dataflow import texture_fetch_metadata
from test_filter_weights_dataflow import fetch
from test_pixel_sample_dataflow import pixel
from test_raw_shader_analysis import alu
from raw_shader_analysis import RawShaderError

FETCH=(0x80424802,0x1000B6,31|(31<<13),0xA80C14,3,0x210)

class FilterWeightsSamplingTests(unittest.TestCase):
    def metadata(self,weights=False,x=0,y=0):
        swizzle=0xE78 if weights else 0x688 # Weight XY only, destination XZ.
        return texture_fetch_metadata(fetch(weights=weights,x=x,y=y,swizzle=swizzle),
            [[None]*4 for _ in range(64)],5,filter_weights_profile=True)

    def test_weights_use_fractional_texel_position_not_sample_colour(self):
        b=raw10_base_binding(FETCH)
        def forbidden(*args):raise AssertionError('getWeights must not read color')
        result=fetch_reference(self.metadata(True),[1/32,1/16],b,forbidden)
        self.assertEqual(result,[.50146484375,.50146484375,0.,0.])
        result=fetch_reference(self.metadata(True,x=1,y=-1),[1/32,1/16],b,forbidden)
        self.assertEqual(result,[.00146484375,.00146484375,0.,0.])

    def test_point_offsets_select_neighbours_and_exponent_follows_swizzle(self):
        changed=list(FETCH);changed[3]|=3<<13;b=raw10_base_binding(changed)
        seen=[]
        def read(x,y):seen.append((x,y));return 1023|(512<<10)|(256<<20)|(3<<30)
        for x,y,wanted in ((1,1,(1,1)),(-1,1,(0,1)),(1,-1,(1,0)),(-1,-1,(0,0))):
            result=fetch_reference(self.metadata(x=x,y=y),[1/32,1/32],b,read)
            self.assertEqual(seen[-1],wanted)
            self.assertEqual(result[2],8.) # BGR swizzle.
            self.assertEqual(result[3],8.)
        fetch_reference(self.metadata(x=-1,y=-1),[0.,0.],b,read)
        self.assertEqual(seen[-1],(0,0)) # Clamp2.
        changed[0]&=~(7<<10|7<<13);b=raw10_base_binding(changed)
        fetch_reference(self.metadata(x=-1,y=-1),[0.,0.],b,read)
        self.assertEqual(seen[-1],(31,31))

    def test_no_mip_integer_sign_or_bias_guessing(self):
        for word,bit in ((0,2),(1,10),(1,0),(2,26),(3,0),(3,25),(4,2),(4,6),(4,10),(5,11),(5,12)):
            bad=list(FETCH);bad[word]^=1<<bit
            with self.assertRaises(RawShaderError):raw10_base_binding(bad)
        b=raw10_base_binding(FETCH)
        for xy in ([.1,0.],[float('inf'),0.],[65537.,0.]):
            with self.assertRaises(RawShaderError):fetch_reference(self.metadata(),xy,b,lambda *_:0)
        with self.assertRaises(RawShaderError):fetch_reference(self.metadata(),[0.,0.],b,lambda *_:2**32)

    def test_diagnostic_translation_retains_offsets_policy_and_no_game_claim(self):
        code=pixel([alu()],fetch=fetch(x=1,y=-1))
        text,proof=translate_filter_weights_compute(code,{},FETCH)
        self.assertIn('0.50146484375,-0.49853515625',text)
        self.assertIn('textureLod(sourceTexture',text)
        self.assertFalse(proof['native_game_pipeline'])
        self.assertFalse(proof['hardware_filter_conformance'])

    def test_fragment_reads_current_constants_and_poison_on_failure(self):
        from test_vertex_arithmetic_dataflow import math_alu
        code=pixel([math_alu(op=1,dest=0,sources=(0,18,0),temps=(True,False,False))],fetch=fetch())
        text,proof=translate_filter_weights_fragment(code,FETCH)
        self.assertIn('readBE(1100u)',text)
        self.assertIn('readBE(1103u)',text)
        self.assertIn('out vec4 colour',text)
        self.assertIn('colour=vec4(uintBitsToFloat(0x7FC00000u))',text)
        self.assertNotIn('queries[',text)
        self.assertNotIn('local_size_x',text)
        self.assertTrue(proof['runtime_constants'])
        self.assertFalse(proof['native_game_pipeline'])

if __name__=='__main__':unittest.main()
