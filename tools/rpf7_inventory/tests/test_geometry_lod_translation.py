"""Separate offline sampling profile, never a native rendering certificate."""
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from inline_shader_translation import translate_inline_pair
from inline_capture_preflight import selected_profile_requirements
from pixel_sample_dataflow import evaluate_pixel_sample_dataflow
from raw_shader_analysis import RawShaderError
from test_geometry_shader_translation import vertex
from test_raw_shader_analysis import program,cf_alloc,cf_exec
from test_vertex_arithmetic_dataflow import math_alu
from test_pixel_sample_dataflow import texture

OPTIONS=dict(indexed=True,extended_profile=True,loading_profile=True,geometry_profile=True,
             vertex_predicate_profile=True,geometry_lod_profile=True)

def pixel(destination=6,relative=False):
    offset=list(texture(src=0,dst=4,slot=15));offset[2]|=(3<<16)|(31<<21)
    explicit=list(texture(src=0,dst=8,slot=1));explicit[1]=(explicit[1]&4095)|0x2F1FF000
    lod=(24|(1<<19)|(7<<5)|(destination<<12)|(int(relative)<<11),0x0F1FFFFF,0)
    return program([cf_alloc(),cf_exec(address=1,count=6,sequence=321)],
        [offset,math_alu(op=8,dest=6,sources=(4,0,0),temps=(True,True,True),export=False),
         math_alu(op=3,dest=7,sources=(6,0,0),temps=(True,False,True),export=False),
         lod,explicit,math_alu(op=0,dest=0,sources=(8,6,0),temps=(True,True,True))])


class GeometryLodTranslationTests(unittest.TestCase):
    def test_pair_keeps_fraction_min_half_offsets_and_explicit_lod(self):
        vs,ps,m=translate_inline_pair(vertex(),pixel(),**OPTIONS)
        self.assertEqual(m['abi_version'],32)
        self.assertEqual(m['texture_slots'],[15,1])
        self.assertFalse(m['geometry_lod_native_sampling_verified'])
        self.assertIn('vec2(1.5,-0.5)/vec2(textureSize(atlas0,0))',ps)
        self.assertIn('textureLod(atlas1',ps)
        self.assertIn('fract(',ps)
        self.assertIn('interpolator0_0',vs)
        self.assertNotIn('interpolator0_2',vs)

    def test_unused_setlod_destination_does_not_overwrite_fraction_register(self):
        for destination in range(64):
            value=evaluate_pixel_sample_dataflow(pixel(destination),{0:[.25,.75,None,None]},
                {0:struct.pack('>4f',.125,.125,.125,.125)},
                {0:[2.25,-1.25,4.75,2.0],1:[.125,.25,.5,1.]},
                ui_operations=True,extended_math=True,loading_math=True,geometry_profile=True,
                geometry_lod_profile=True,include_queries=True)
            self.assertEqual(value['queries'][1]['lod'],.125)
            for actual,expected in zip(value['exports']['0'],(.375,1.,1.25,1.)):
                self.assertAlmostEqual(actual,expected,places=6)

    def test_old_profiles_and_unsupported_combinations_still_reject(self):
        for change in ({'geometry_lod_profile':False},{'geometry_lod_profile':1},
                       {'indexed':False},{'geometry_profile':False},{'vertex_predicate_profile':False},
                       {'explicit_lod_profile':True},{'geometry_predicate_profile':True},
                       {'geometry_color8_format':0},{'post_stencil_profile':True},
                       {'rectangle_list':True},{'indexed_rgb7e3_profile':True}):
            with self.assertRaises(RawShaderError):
                translate_inline_pair(vertex(),pixel(),**{**OPTIONS,**change})
        with self.assertRaises(RawShaderError):
            translate_inline_pair(vertex(),pixel(relative=True),**OPTIONS)

    def test_position_compute_retains_same_full_pixel_validation(self):
        _,ps,m=translate_inline_pair(vertex(),pixel(),**OPTIONS)
        compute,other,proof=translate_inline_pair(vertex(),pixel(),geometry_position_compute=True,**OPTIONS)
        self.assertEqual(ps,other)
        self.assertEqual(m['vertex'],proof['vertex'])
        self.assertIn('gl_GlobalInvocationID.x',compute)
        self.assertNotIn('gl_Position',compute)

    def test_selected_inventory_uses_actual_capability_without_native_admission(self):
        result=selected_profile_requirements(dict(primitive=4,index_count=3),
            {'vertex-shader.bin':vertex(),'pixel-shader.bin':pixel()},'geometry-lod')
        self.assertTrue(result['translation']['accepted'])
        self.assertEqual(result['translation']['abi'],32)
        self.assertFalse(result['translation']['native_admission'])
        self.assertEqual(result['vertex']['issues'],[])
        self.assertEqual(result['pixel']['issues'],[])


if __name__=='__main__':unittest.main()
