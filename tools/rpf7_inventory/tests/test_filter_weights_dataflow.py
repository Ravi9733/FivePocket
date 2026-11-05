"""Authored instruction semantics; no implicit sampler/LOD or draw admission."""
from pathlib import Path
import math
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from loading_shader_math import evaluate_loading_operation
from pixel_sample_dataflow import (analyze_pixel_sample_dataflow,
    evaluate_analyzed_pixel_dataflow,texture_fetch_metadata)
from raw_shader_analysis import RawShaderError
from test_pixel_sample_dataflow import pixel,texture,coissue
from test_vertex_arithmetic_dataflow import math_alu
from test_raw_shader_analysis import alu

OPTIONS=dict(ui_operations=True,extended_math=True,loading_math=True,filter_weights_profile=True)

def fetch(*,weights=False,x=0,y=0,swizzle=0x688):
    a,b,c=texture(slot=5,swizzle=swizzle)
    return ((a&~31)|(19 if weights else 1),
            (b&~0xF000)|(0x5000 if weights else 0),
            c|((x&31)<<16)|((y&31)<<21))

def analyze(instructions,**kwargs):
    return analyze_pixel_sample_dataflow(pixel(instructions,fetch=fetch()),**{**OPTIONS,**kwargs})

class FilterWeightsDataflowTests(unittest.TestCase):
    def test_weights_are_not_colour_or_sampling_execution(self):
        proof=analyze_pixel_sample_dataflow(pixel([alu()],fetch=fetch(weights=True)),**OPTIONS)
        item=proof['fetches'][0]
        self.assertEqual(item['operation'],'texture_weights')
        self.assertTrue(item['explicit_linear_filter'])
        self.assertFalse(item['explicit_point_filter'])
        self.assertFalse(item['weight_lod_verified'])
        self.assertTrue(proof['weight_results_are_explicit_inputs'])
        self.assertFalse(proof['sampling_executed'])
        self.assertFalse(proof['native_fragment_pipeline'])
        self.assertEqual(evaluate_analyzed_pixel_dataflow(proof,{0:[.25,.75,None,None]},
            {},{0:[.125,.625,0.,0.]})['0'],[.125,.625,0.,0.])
        with self.assertRaises(RawShaderError):
            evaluate_analyzed_pixel_dataflow(proof,{0:[.25,.75,None,None]}, {},{})

    def test_half_texel_offsets_and_operand_snapshots(self):
        registers=[[r*4+c for c in range(4)] for r in range(64)]
        for weights in (False,True):
            for x in range(-16,16):
                for y in range(-16,16):
                    item=texture_fetch_metadata(fetch(weights=weights,x=x,y=y),registers,5,
                                                filter_weights_profile=True)
                    self.assertEqual(item.get('texel_offset',(0,0)),(x/2,y/2))
                    self.assertEqual(item['coordinates'],[0,1])
                    self.assertEqual(item['slot'],5)
                    self.assertTrue(item['computed_lod_requested'])

    def test_vector_floor_is_unary_masked_and_not_truncation(self):
        def run(values,mask=15,negate=False):
            inst=list(math_alu(op=10,dest=0,mask=mask,sources=(1,64,0),temps=(True,True,True)))
            if negate:inst[1]|=1<<26
            proof=analyze([inst])
            return evaluate_analyzed_pixel_dataflow(proof,{0:[0]*4,1:values},{},{0:[0]*4})['0']
        self.assertEqual(run([-1.75,-.25,.25,2.75]),[-2,-1,0,2])
        self.assertEqual(run([.25,None,None,None],1,True),[-1,None,None,None])
        self.assertEqual(run([-2.,-0.,0.,16777216.]),[-2.,-0.,0.,16777216.])
        self.assertEqual(math.copysign(1,run([-0.]*4)[0]),-1)

    def test_scalar_floor_coissue_reads_old_input(self):
        # Floor scalar W while vector multiplies the same temporary XY.
        inst=list(coissue(vmask=3,smask=4))
        inst[0]=(inst[0]&0x03FFFFFF)|(13<<26)
        inst[1]&=~255;inst[2]=(inst[2]&~255)|(1<<29)
        proof=analyze([inst,alu()])
        import struct
        result=evaluate_analyzed_pixel_dataflow(proof,{0:[0]*4},
            {0:struct.pack('>4f',2,3,4,5)},{0:[1.25,2.5,7.,-1.25]})
        self.assertEqual(result['0'],[2.5,7.5,-2.,-1.25])
        # A zero destination mask still updates previous scalar, and overwritten
        # invalid scalar arithmetic still rejects the finite-only reference.
        scalar=((13<<26),0,(2<<24)|(1<<29))
        proof=analyze([scalar,alu(vector=0,scalar=15)])
        self.assertEqual(evaluate_analyzed_pixel_dataflow(proof,{0:[0]*4},{},
            {0:[0.,0.,0.,-1.25]})['0'],[-2.]*4)
        for v in (float('nan'),float('inf'),2**-149,.1):
            with self.assertRaises(RawShaderError):evaluate_loading_operation('floor',[v])

    def test_filters_and_unsafe_fields_reject(self):
        registers=[[None]*4 for _ in range(64)]
        for weights in (False,True):
            base=fetch(weights=weights,x=1,y=-1)
            for word,bit in ((0,11),(0,18),(0,19),(0,25),(1,12),(1,14),(1,16),
                             (1,21),(1,28),(1,29),(1,31),(2,0),(2,2),(2,15),(2,26),(2,31)):
                changed=list(base);changed[word]^=1<<bit
                with self.assertRaises(RawShaderError,msg=(weights,word,bit)):
                    texture_fetch_metadata(changed,registers,5,filter_weights_profile=True)
            changed=list(base);changed[1]=(changed[1]&~7)|6
            with self.assertRaises(RawShaderError):
                texture_fetch_metadata(changed,registers,5,filter_weights_profile=True)

    def test_opt_in_and_unrelated_profiles_remain_closed(self):
        for weights in (False,True):
            for options in ({},dict(ui_operations=True,extended_math=True,loading_math=True),
                            dict(ui_operations=True,extended_math=True,loading_math=True,post_stencil_profile=True)):
                with self.assertRaises(RawShaderError):
                    analyze_pixel_sample_dataflow(pixel([alu()],fetch=fetch(weights=weights)),**options)
        for option in ('geometry_profile','predicate_profile','rectangle_mrt_profile','tonemap_profile',
                       'cube_profile','composite_profile','explicit_lod_profile','rectangle_depth_profile','post_stencil_profile'):
            with self.assertRaises(RawShaderError):analyze([alu()],**{option:True})
        for opcode in (9,13,18,25):
            with self.assertRaises(RawShaderError):analyze([math_alu(op=opcode)])

if __name__=='__main__':unittest.main()
