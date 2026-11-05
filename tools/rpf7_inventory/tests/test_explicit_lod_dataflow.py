from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from pixel_sample_dataflow import analyze_pixel_sample_dataflow, evaluate_pixel_sample_dataflow
from inline_shader_translation import translate_inline_pair
from raw_shader_analysis import RawShaderError
from test_raw_shader_analysis import program, cf_alloc, cf_exec, alu
from test_pixel_sample_dataflow import texture
from test_inline_shader_translation import vertex


OPTIONS=dict(ui_operations=True,extended_math=True,loading_math=True,explicit_lod_profile=True)


def set_lod(source=0,component=0):
    return 24|(1<<19)|(source<<5)|(component*21<<26),0x0F1FFFFF,0


def lod_fetch(source=0,destination=0,slot=5):
    a,b,c=texture(src=source,dst=destination,slot=slot)
    return a,(b&~(1<<28))|(1<<29),c


def code(items,sequence):
    return program([cf_alloc(),cf_exec(address=1,count=len(items),sequence=sequence)],items)


class ExplicitLodDataflowTests(unittest.TestCase):
    def test_set_is_state_not_sample_and_fetch_retains_prewrite_lod(self):
        for component in range(4):
            shader=code([set_lod(component=component),lod_fetch(),alu()],5)
            proof=analyze_pixel_sample_dataflow(shader,**OPTIONS)
            self.assertEqual(len(proof['fetches']),1)
            fetch=proof['fetches'][0]
            self.assertFalse(fetch['computed_lod_requested'])
            self.assertTrue(fetch['register_lod_requested'])
            node=proof['nodes'][fetch['lod_expression']]
            self.assertEqual(node[0],'regular_copy')
            self.assertEqual(proof['nodes'][node[1]],('initial',0,component))
            self.assertEqual(evaluate_pixel_sample_dataflow(shader,{0:[.25,.5,2.,3.]},{},
                {0:[7.,11.,13.,17.]},**OPTIONS),{'0':[7.,11.,13.,17.]})
            self.assertFalse(proof['sampling_executed'])
            values=[.25,.5,2.,3.]
            result=evaluate_pixel_sample_dataflow(shader,{0:values},{},
                {0:[7.,11.,13.,17.]},include_queries=True,**OPTIONS)
            self.assertEqual(result['queries'],[dict(slot=5,coordinates=[.25,.5],lod=values[component])])
            with self.assertRaises(RawShaderError):
                analyze_pixel_sample_dataflow(shader,ui_operations=True,extended_math=True,loading_math=True)

    def test_multiple_lod_writes_select_the_corresponding_snapshot(self):
        shader=code([set_lod(source=1,component=2),lod_fetch(),set_lod(source=2,component=3),lod_fetch(),alu()],85)
        proof=analyze_pixel_sample_dataflow(shader,**OPTIONS)
        self.assertEqual(len(proof['lod_sets']),2)
        self.assertEqual([proof['nodes'][proof['nodes'][f['lod_expression']][1]]
                          for f in proof['fetches']],[('initial',1,2),('initial',2,3)])

    def test_missing_lod_modifiers_and_invalid_sources_reject(self):
        with self.assertRaises(RawShaderError):
            analyze_pixel_sample_dataflow(code([lod_fetch(),alu()],1),**OPTIONS)
        for word,bit in ((0,11),(0,18),(0,20),(0,25),(1,31),(2,0),(2,2)):
            changed=list(set_lod());changed[word]^=1<<bit
            with self.assertRaises(RawShaderError):
                analyze_pixel_sample_dataflow(code([changed,lod_fetch(),alu()],5),**OPTIONS)
        for word,bit in ((0,11),(0,18),(0,25),(1,28),(1,31),(2,0),(2,2),(2,16)):
            changed=list(lod_fetch());changed[word]^=1<<bit
            with self.assertRaises(RawShaderError):
                analyze_pixel_sample_dataflow(code([set_lod(),changed,alu()],5),**OPTIONS)
        shader=code([set_lod(source=1),lod_fetch(),alu()],5)
        for bad in (None,float('inf'),float('nan'),2**-149):
            with self.assertRaises(RawShaderError):
                evaluate_pixel_sample_dataflow(shader,{0:[0.]*4,1:[bad,0.,0.,0.]},{},{0:[1.]*4},**OPTIONS)

    def test_translation_has_separate_abi_and_explicit_sampling(self):
        shader=code([set_lod(component=1),lod_fetch(),alu()],5)
        vs,ps,proof=translate_inline_pair(vertex(1),shader,extended_profile=True,
            loading_profile=True,rectangle_list=True,explicit_lod_profile=True)
        self.assertEqual(proof['abi_version'],9)
        self.assertFalse(proof['explicit_lod_sampling_verified'])
        self.assertEqual(proof['texture_slots'],[5])
        self.assertEqual(ps.count('textureLod('),1)
        self.assertNotIn('=texture(',ps)
        self.assertIn('originalVertex',vs)
        with self.assertRaises(RawShaderError):
            translate_inline_pair(vertex(1),shader,extended_profile=True,explicit_lod_profile=True)


if __name__=='__main__':unittest.main()
