"""Authored ABI27 shader interfaces; native blending/ownership remain unproven."""
import unittest
from inline_shader_translation import translate_inline_pair
from raw_shader_analysis import RawShaderError
from test_raw_shader_analysis import program,cf_exec,cf_alloc,alu
from test_raw_vertex_fetch_analysis import fetch
from test_pixel_sample_dataflow import texture
from test_vertex_arithmetic_dataflow import math_alu

OPTIONS=dict(extended_profile=True,loading_profile=True,rectangle_list=True,post_stencil_profile=True)
def vertex(stride=9,first_mask=15):
    exports=[alu(dest=0,source=1,vector=first_mask),alu(dest=1,source=1),alu(dest=62,source=1)]
    return program([cf_exec(address=3,sequence=1,opcode=1),cf_alloc(2,size=2),
                    cf_alloc(1),cf_exec(address=4,count=3),0,0],
                   [fetch(dst=1,slot=95,fmt=57,stride=stride,swizzle=0xA88),*exports])
def pixel(slots=(8,7,9)):
    instructions=[texture(src=0,dst=4+i,slot=s) for i,s in enumerate(slots)]
    instructions.extend([math_alu(op=0,dest=4,sources=(4,5,0),temps=(True,True,True),export=False),
                         math_alu(op=8,dest=4,sources=(4,0,0),temps=(True,True,True),export=False),
                         math_alu(op=11,dest=0,sources=(4,6,4),temps=(True,True,True))])
    return program([cf_exec(address=2,count=3,sequence=21,opcode=1),cf_alloc(),
                    cf_exec(address=5,count=3),0],instructions)

class PostStencilTranslationTests(unittest.TestCase):
    def test_readonly_inventory_uses_the_selected_profile_without_admission(self):
        from inline_capture_preflight import selected_profile_requirements
        result=selected_profile_requirements(dict(primitive=8,index_count=0),
            {'vertex-shader.bin':vertex(),'pixel-shader.bin':pixel()},'post-stencil')
        self.assertTrue(result['translation']['accepted']);self.assertEqual(result['translation']['abi'],27)
        self.assertFalse(result['translation']['native_admission'])
        self.assertFalse(result['vertex']['issues']);self.assertFalse(result['pixel']['issues'])

    def test_separate_abi_three_samplers_fraction_and_no_tonemap_scale(self):
        vs,ps,m=translate_inline_pair(vertex(),pixel(),**OPTIONS)
        self.assertEqual((m['abi_version'],m['stride'],m['texture_slots']),(27,36,[8,7,9]))
        self.assertTrue(m['post_stencil_profile']);self.assertFalse(m['post_stencil_native_state_verified'])
        for i in range(3):self.assertIn(f'binding={i+1}) uniform sampler2D atlas{i}',ps)
        self.assertIn('fract(',ps);self.assertNotIn('color.xy*(1.0/32.0)',ps)
        self.assertIn('out vec4 interpolator0',vs);self.assertIn('in vec4 interpolator0',ps)
        self.assertIn('originalVertex',vs)

    def test_position_compute_uses_same_original_program(self):
        source,_,m=translate_inline_pair(vertex(),pixel(),**OPTIONS,position_compute=True)
        self.assertIn('gl_GlobalInvocationID.x',source);self.assertEqual(m['abi_version'],27)

    def test_old_profiles_do_not_gain_fraction_or_three_slot_admission(self):
        with self.assertRaises(RawShaderError):translate_inline_pair(vertex(),pixel(),**{**OPTIONS,'post_stencil_profile':False})
        with self.assertRaises(RawShaderError):translate_inline_pair(vertex(),pixel(),extended_profile=True,loading_profile=True,
            rectangle_list=True,offset_sample_profile=True,tonemap_profile=True)

    def test_wrong_slots_stream_and_unprovided_lanes_reject(self):
        for vs,ps in ((vertex(),pixel((8,7,10))),(vertex(6),pixel()),(vertex(first_mask=5),pixel())):
            with self.assertRaises(RawShaderError):translate_inline_pair(vs,ps,**OPTIONS)

    def test_conflicting_profiles_and_nonboolean_are_rejected(self):
        for key in ('indexed','geometry_profile','tonemap_profile','offset_sample_profile','composite_profile',
                    'screen_space_profile','vertex_only','triangle_fan','direct_triangles','color8_source_clamp'):
            with self.assertRaises(RawShaderError):translate_inline_pair(vertex(),pixel(),**{**OPTIONS,key:True})
        with self.assertRaises(RawShaderError):translate_inline_pair(vertex(),pixel(),**{**OPTIONS,'post_stencil_profile':1})

if __name__=='__main__':unittest.main()
