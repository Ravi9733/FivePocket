"""Authored mixed shortXY/floatZ inline stream, not a game fixture."""
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from inline_shader_translation import translate_inline_pair
from raw_shader_analysis import RawShaderError
from vertex_arithmetic_dataflow import evaluate_vertex_dataflow
from test_raw_shader_analysis import program,cf_exec,cf_alloc,alu
from test_raw_vertex_fetch_analysis import fetch
from test_pixel_sample_dataflow import pixel

def mixed_vertex(offset=1):
    return program([cf_exec(address=2,count=2,sequence=5,opcode=1),cf_alloc(1),
                    cf_alloc(2),cf_exec(address=4,count=2)],
        [fetch(dst=1,slot=95,stride=2,fmt=25,swizzle=0xA48),
         fetch(dst=1,slot=0,stride=0,offset=offset,fmt=36,swizzle=0xE3F,mini=True),
         alu(source=1,dest=62),alu(source=1)])

class InlineFloat1Tests(unittest.TestCase):
    def test_mixed_stream_swizzle_stride_bounds_and_extended_translation(self):
        vs=mixed_vertex();ps=pixel([alu()])
        packed=b''.join(struct.pack('>If',((x&65535)<<16)|(y&65535),z)
            for x,y,z in ((-12,23,.25),(32767,-32768,-.5)))
        for i,expected in enumerate(([23,-12,.25,1],[-32768,32767,-.5,1])):
            result=evaluate_vertex_dataflow(vs,{95:packed},{},i,dot4=True,scalar_constants=True)
            self.assertEqual(result['62'],expected)
        vertex,_,proof=translate_inline_pair(vs,ps,extended_profile=True)
        self.assertEqual(proof['stride'],8)
        self.assertEqual([f['format'] for f in proof['vertex']['fetches']],[25,36])
        self.assertIn('fetch1_0=uintBitsToFloat(readBE(2052u+index*2u+1u+0u))',vertex)
        with self.assertRaises(RawShaderError):translate_inline_pair(vs,ps)
        with self.assertRaises(RawShaderError):translate_inline_pair(mixed_vertex(2),ps,extended_profile=True)
        with self.assertRaises(RawShaderError):
            evaluate_vertex_dataflow(vs,{95:packed[:4]},{},0,dot4=True)

if __name__=='__main__':unittest.main()
