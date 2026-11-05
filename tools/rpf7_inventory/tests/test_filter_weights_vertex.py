from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from filter_weights_vertex import translate_filter_weights_vertex
from raw_shader_analysis import RawShaderError
from test_raw_shader_analysis import cf_alloc,cf_exec,program,alu
from test_raw_vertex_fetch_analysis import fetch as vf
from test_filter_weights_dataflow import fetch as pf
from test_pixel_sample_dataflow import pixel
from test_vertex_texture_dataflow import TEXTURE

LITERAL=(0x02000002,2,0,0x01001249,0,0x200)
def vertex(stride=6,position_source=1):
    return program([cf_alloc(1),cf_alloc(2),cf_exec(address=2,count=6,sequence=69),0],
        [vf(dst=1,slot=95,stride=stride),vf(dst=0,fmt=37,mini=True,offset=4,swizzle=0xFC8),
         alu(source=position_source,dest=62),TEXTURE,((19<<26)|(8<<20),0xC6,(2<<24)|(1<<29)),alu()])

class FilterWeightsVertexTests(unittest.TestCase):
    def test_original_vertex_bytes_and_synthetic_corner_after_arithmetic(self):
        text,proof=translate_filter_weights_vertex(vertex(),pixel([alu()],fetch=pf()),LITERAL)
        self.assertIn('readBE(2052u+index*6u+4u)',text)
        self.assertIn('0x7F800000u',text) # Preserve executed IEEE reciprocal.
        self.assertIn('(positions[b]-positions[a])+positions[c]',text)
        self.assertIn('out vec2 interpolatorXY',text)
        self.assertEqual(proof['pixel_initial_components'],[(0,0),(0,1)])
        self.assertEqual(proof['omitted_varying_lanes'],[2,3])
        self.assertFalse(proof['rectangle_coverage_verified'])
        self.assertFalse(proof['native_game_pipeline'])

    def test_different_storage_and_null_descriptor_are_not_fallbacks(self):
        ps=pixel([alu()],fetch=pf())
        with self.assertRaises(RawShaderError):translate_filter_weights_vertex(vertex(7),ps,LITERAL)
        for i in range(6):
            bad=list(LITERAL);bad[i]^=1
            with self.assertRaises(RawShaderError):translate_filter_weights_vertex(vertex(),ps,bad)

    def test_pixel_zw_read_must_not_be_silently_pruned(self):
        from test_vertex_arithmetic_dataflow import math_alu
        # Original r0 is preserved by sampling into r1. Exporting r0 consumes
        # the vertex infinity W; this must NOT get the safe XY-only interface.
        f=list(pf());f[0]|=1<<12
        ps=pixel([math_alu(op=2,dest=0,sources=(0,0,0),temps=(True,True,True))],fetch=f)
        with self.assertRaises(RawShaderError):translate_filter_weights_vertex(vertex(),ps,LITERAL)

    def test_coverage_certificate_cannot_use_different_position_dataflow(self):
        with self.assertRaises(RawShaderError):
            translate_filter_weights_vertex(vertex(position_source=0),pixel([alu()],fetch=pf()),LITERAL)

if __name__=='__main__':unittest.main()
