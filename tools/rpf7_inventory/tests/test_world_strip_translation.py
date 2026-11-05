"""Authored ABI29 offline contract; no native state, textures or gameplay proof."""
from pathlib import Path
import sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from inline_shader_translation import translate_inline_pair
from inline_capture_preflight import selected_profile_requirements,profile_options
from raw_shader_analysis import RawShaderError
from test_raw_shader_analysis import program,cf_exec,cf_alloc,alu
from test_raw_vertex_fetch_analysis import fetch
from test_pixel_sample_dataflow import texture
from test_loading_shader_math import scalar_alu
from test_vertex_arithmetic_dataflow import math_alu

OPTIONS=dict(extended_profile=True,loading_profile=True,world_strip_profile=True)
def vertex(stride=9,mask=15):
    return program([cf_exec(address=3,sequence=1,opcode=1),cf_alloc(2,size=4),
                    cf_alloc(1),cf_exec(address=4,count=6),0,0],
        [fetch(dst=0,slot=95,fmt=57,stride=stride,swizzle=0xA88),scalar_alu(8,mask=0),
         *[alu(dest=i,source=0,vector=mask if i==2 else 15) for i in (0,1,2,3,62)]])
def pixel(slots=(2,6,1,1,3)):
    return program([cf_exec(address=3,count=5,sequence=0x155,opcode=1),cf_alloc(),
                    cf_exec(address=8,count=2),0,0,0],
        [*[texture(src=0,dst=4+i,slot=s) for i,s in enumerate(slots)],
         math_alu(op=25,dest=9,mask=0,sources=(4,0,0),temps=(True,False,True),export=False),alu(dest=0,source=8)])

class WorldStripTranslationTests(unittest.TestCase):
    def test_separate_abi_and_four_complete_interfaces(self):
        v,f,p=translate_inline_pair(vertex(),pixel(),**OPTIONS)
        self.assertEqual((p['abi_version'],p['stride'],p['texture_slots']),(29,36,[2,6,1,3]))
        self.assertFalse(p['indexed']);self.assertFalse(p['world_strip_native_state_verified'])
        self.assertFalse(p['world_strip_resources_verified']);self.assertFalse(p['strip_extent_validated'])
        self.assertFalse(p['strip_interpolation_verified']);self.assertTrue(p['strip_provoking_last_required'])
        for slot in range(4):
            self.assertIn(f'out vec4 interpolator{slot};',v);self.assertIn(f'in vec4 interpolator{slot};',f)
        self.assertIn('uint index=triangle+stripCorners[(triangle&1u)*3u+corner];',v)
        self.assertNotIn('color.xy*(1.0/32.0)',f);self.assertNotIn('geometryStoreMrtColor8',f)

    def test_kill_precedes_both_outputs_and_fetches_preserve_derivatives(self):
        _,f,p=translate_inline_pair(vertex(),pixel(),**OPTIONS)
        self.assertEqual(len(p['pixel']['fetches']),5);self.assertEqual(len(p['pixel']['kills']),1)
        kill=f.index('if (pixelKilled) discard;')
        self.assertLess(f.index('vec4 sampled4=texture('),kill)
        self.assertLess(kill,f.index('    color=vec4('))
        self.assertLess(kill,f.index('    gl_FragDepth='))
        self.assertNotIn('early_fragment_tests',f)

    def test_original_position_compute_does_not_expand_or_run_pixel(self):
        v,_,p=translate_inline_pair(vertex(),pixel(),**OPTIONS,position_compute=True)
        self.assertEqual(p['abi_version'],29);self.assertIn('gl_GlobalInvocationID.x',v)
        self.assertNotIn('stripCorners',v);self.assertNotIn('discard;',v)
        self.assertNotIn('viewport.scaleOffset',v)

    def test_old_profiles_and_interface_mutations_refuse(self):
        with self.assertRaises(RawShaderError):translate_inline_pair(vertex(),pixel(),extended_profile=True,loading_profile=True)
        for v,f in ((vertex(8),pixel()),(vertex(mask=7),pixel()),(vertex(),pixel((2,6,1,1,7)))):
            with self.assertRaises(RawShaderError):translate_inline_pair(v,f,**OPTIONS)

    def test_conflicting_profiles_and_invalid_boolean_refuse(self):
        for key in ('indexed','direct_triangles','triangle_fan','quad_list','rectangle_list','cube_profile',
                    'geometry_profile','geometry_compute','geometry_position_compute','geometry_predicate_profile',
                    'post_stencil_profile','screen_space_profile','screen_strip_profile','offset_sample_profile',
                    'tonemap_profile','vertex_only','predicate_profile','color8_source_clamp'):
            with self.assertRaises(RawShaderError):translate_inline_pair(vertex(),pixel(),**{**OPTIONS,key:True})
        with self.assertRaises(RawShaderError):translate_inline_pair(vertex(),pixel(),**{**OPTIONS,'world_strip_profile':1})

    def test_selected_inventory_does_not_imply_native_admission(self):
        self.assertFalse(profile_options('world-strip')['geometry_profile'])
        self.assertFalse(profile_options('world-strip')['geometry_predicate_profile'])
        payload={'vertex-shader.bin':vertex(),'pixel-shader.bin':pixel()}
        p=selected_profile_requirements(dict(primitive=6,index_count=0),payload,'world-strip')
        self.assertTrue(p['translation']['accepted']);self.assertFalse(p['translation']['native_admission'])
        self.assertFalse(p['vertex']['issues']);self.assertFalse(p['pixel']['issues'])
        for primitive,indexed in ((4,0),(8,0),(13,0),(6,1)):
            self.assertFalse(selected_profile_requirements(dict(primitive=primitive,index_count=indexed),payload,'world-strip')['translation']['accepted'])

if __name__=='__main__':unittest.main()
