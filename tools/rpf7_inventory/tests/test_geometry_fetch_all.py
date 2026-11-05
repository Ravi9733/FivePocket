"""Explicit straight-line fetch-all metadata and producer/consumer connection.

Samples are authored values. These tests do not prove hardware helper-lane,
derivative/LOD accuracy or captured game texture ownership.
"""
import unittest
from test_pixel_sample_dataflow import texture,pixel
from test_raw_shader_analysis import program,cf_alloc,cf_exec,alu
from test_geometry_shader_translation import vertex,OPTIONS
from pixel_sample_dataflow import analyze_pixel_sample_dataflow,evaluate_pixel_sample_dataflow
from inline_shader_translation import translate_inline_pair
from raw_shader_analysis import RawShaderError

ANALYZE=dict(ui_operations=True,extended_math=True,loading_math=True,geometry_profile=True)
def all_fetch(**kw):
    a,b,c=texture(**kw);return a&~(1<<19),b,c

class GeometryFetchAllTests(unittest.TestCase):
    def test_metadata_preserves_bit_and_explicit_samples(self):
        for valid in (False,True):
            code=pixel(fetch=texture(slot=2) if valid else all_fetch(slot=2))
            proof=analyze_pixel_sample_dataflow(code,**ANALYZE)
            self.assertEqual(proof['fetches'][0]['fetch_valid_only'],valid)
            self.assertTrue(proof['fetches'][0]['computed_lod_requested'])
            self.assertEqual(evaluate_pixel_sample_dataflow(code,{0:[.25,.75,0,0],1:[2,3,4,5]},
                {},{0:[7,11,13,17]},**ANALYZE)['0'],[14,33,52,85])
            if not valid:
                with self.assertRaises(RawShaderError):
                    analyze_pixel_sample_dataflow(code,**{**ANALYZE,'geometry_profile':False})

    def test_mixed_fetch_modes_emit_unconditional_implicit_sampling(self):
        code=program([cf_exec(address=2,count=4,sequence=85,opcode=1),cf_alloc(),cf_exec(address=6),0],
            [texture(src=0,dst=4,slot=0),all_fetch(src=0,dst=5,slot=2),
             all_fetch(src=0,dst=6,slot=3),texture(src=0,dst=7,slot=4),alu(source=7)])
        _,ps,proof=translate_inline_pair(vertex(),code,**OPTIONS,geometry_mrt_formats=(0,None,None,None))
        self.assertEqual([f['fetch_valid_only'] for f in proof['pixel']['fetches']],[True,False,False,True])
        self.assertEqual(proof['texture_slots'],[0,2,3,4])
        self.assertNotIn('textureLod(',ps)
        self.assertNotIn('gl_HelperInvocation',ps)
        self.assertFalse(proof['pixel']['sampling_executed'])

    def test_other_modifiers_and_control_modes_are_not_relaxed(self):
        base=all_fetch()
        for word,bit in ((0,11),(0,18),(0,25),(1,28),(1,29),(1,31),(2,0),(2,15),(2,16)):
            changed=list(base);changed[word]^=1<<bit
            with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(pixel(fetch=tuple(changed)),**ANALYZE)
        for option in ('cube_profile','explicit_lod_profile','predicate_profile'):
            with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(pixel(fetch=base),**ANALYZE,**{option:True})

if __name__=='__main__':unittest.main()
