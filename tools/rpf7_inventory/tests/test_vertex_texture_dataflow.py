"""Explicit texture results only: no dummy resource or implicit zero samples."""
from pathlib import Path
import math
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from vertex_arithmetic_dataflow import analyze_vertex_dataflow,evaluate_vertex_dataflow
from raw_shader_analysis import RawShaderError
from test_raw_shader_analysis import cf_alloc,cf_exec,program,alu
from test_raw_vertex_fetch_analysis import fetch

OPTIONS=dict(dot4=True,scalar_constants=True,loading_math=True,base_texture_profile=True)
TEXTURE=(1|(1<<19)|(17<<20)|(1<<28),0x0F1FFE3F,0x4000) # R -> Z; XYW kept.

def shader(texture=TEXTURE,reciprocal=False):
    tail=[((19<<26)|(8<<20),0xC6,(2<<24)|(1<<29))] if reciprocal else []
    tail.append(alu(source=0))
    return program([cf_exec(address=3,sequence=1,opcode=1),cf_alloc(1),cf_alloc(2),
                    cf_exec(address=4,count=2,sequence=4,opcode=1),
                    cf_exec(address=6,count=len(tail)),0],
                   [fetch(dst=0,fmt=38,stride=4,swizzle=0x688),alu(dest=62),texture,*tail])

class VertexTextureDataflowTests(unittest.TestCase):
    def evaluate(self,data= None,**kwargs):
        return evaluate_vertex_dataflow(data or shader(),{0:struct.pack('>4f',.25,.75,3,4)}, {},0,
                                        **{**OPTIONS,**kwargs})

    def test_sample_preserves_other_lanes_and_prior_position(self):
        out=self.evaluate(texture_samples={0:[.5,None,None,None]})
        self.assertEqual(out,{'0':[.25,.75,.5,4.],'62':[.25,.75,3.,4.]})
        proof=analyze_vertex_dataflow(shader(),**OPTIONS)
        self.assertEqual(len(proof['fetches']),1)
        self.assertEqual(proof['texture_fetches'][0]['slot'],17)
        self.assertEqual([proof['nodes'][n] for n in proof['texture_fetches'][0]['coordinates']],
                         [('fetch',0,0),('fetch',0,1)])
        self.assertFalse(proof['sampling_executed'])
        self.assertFalse(proof['native_vertex_pipeline'])

    def test_no_missing_sample_is_invented_and_position_only_is_explicit(self):
        for value in (None,{}, {0:[None]*4},{0:[.1]*4},{0:[float('inf')]*4}):
            with self.assertRaises(RawShaderError):self.evaluate(texture_samples=value)
        self.assertEqual(self.evaluate(position_only=True),{'62':[.25,.75,3.,4.]})

    def test_zero_reciprocal_is_infinity_not_finite_fallback(self):
        code=shader(reciprocal=True)
        with self.assertRaises(RawShaderError):self.evaluate(code,texture_samples={0:[0.]*4})
        for zero in (0.,-0.):
            out=self.evaluate(code,texture_samples={0:[zero]*4},base_texture_ieee=True)
            self.assertTrue(math.isinf(out['0'][3]))
            self.assertEqual(math.copysign(1,out['0'][3]),math.copysign(1,zero))
        out=self.evaluate(code,texture_samples={0:[.5]*4})
        self.assertEqual(out['0'][3],2.)

    def test_modifiers_and_legacy_profiles_stay_closed(self):
        for word,bit in ((0,11),(0,18),(0,19),(0,25),(1,12),(1,16),(1,21),
                          (1,28),(1,29),(1,31),(2,0),(2,2),(2,15),(2,16)):
            changed=list(TEXTURE);changed[word]^=1<<bit
            with self.assertRaises(RawShaderError):analyze_vertex_dataflow(shader(changed),**OPTIONS)
        with self.assertRaises(RawShaderError):analyze_vertex_dataflow(shader(),**{**OPTIONS,'base_texture_profile':False})
        for option in ('tonemap','address_profile','predicate_profile','packed_geometry'):
            with self.assertRaises(RawShaderError):analyze_vertex_dataflow(shader(),**{**OPTIONS,option:True})

if __name__=='__main__':unittest.main()
