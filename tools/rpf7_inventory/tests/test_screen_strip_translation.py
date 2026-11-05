"""Authored ABI28 offline tests. No native draw/admission or interpolation proof."""
from pathlib import Path
import sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from inline_shader_translation import translate_inline_pair
from inline_capture_preflight import selected_profile_requirements
from raw_shader_analysis import RawShaderError
from test_raw_shader_analysis import program,cf_exec,cf_alloc,alu
from test_raw_vertex_fetch_analysis import fetch
from test_pixel_sample_dataflow import texture
from test_vertex_arithmetic_dataflow import math_alu

OPTIONS=dict(extended_profile=True,loading_profile=True,screen_strip_profile=True)
def vertex(stride=2,mask=15):
    operations=[math_alu(op=17,dest=1,sources=(1,1,1),temps=(True,True,True),export=False),
        alu(dest=0,source=1,vector=mask),alu(dest=1,source=1),alu(dest=62,source=1)]
    return program([cf_exec(address=3,sequence=1,opcode=1),cf_alloc(2,size=2),
                    cf_alloc(1),cf_exec(address=4,count=4),0,0],
                   [fetch(dst=1,slot=95,fmt=37,stride=stride,swizzle=0xB08),*operations])
def pixel(slot=3):
    return program([cf_exec(address=2,sequence=1,opcode=1),cf_alloc(),cf_exec(address=3),0],
        [texture(src=0,dst=4,slot=slot),alu(dest=0,source=4)])

class ScreenStripTranslationTests(unittest.TestCase):
    def test_separate_profile_math_interfaces_and_no_native_permission(self):
        vs,ps,p=translate_inline_pair(vertex(),pixel(),**OPTIONS)
        self.assertEqual((p['abi_version'],p['stride'],p['texture_slots']),(28,8,[3]))
        self.assertIn('uint index=triangle+stripCorners[(triangle&1u)*3u+corner];',vs)
        self.assertIn('out vec4 interpolator0',vs);self.assertIn('in vec4 interpolator1',ps)
        self.assertNotIn('color.xy*(1.0/32.0)',ps)
        self.assertIn('screenStripStoreDepth();',ps)
        self.assertIn('color=screenStripPrepareBlend(color);',ps)
        self.assertIn('vec4(value.rgb,clamp(value.a,0.0,1.0))',ps)
        self.assertIn('any(isnan(value)) || any(isinf(value))',ps)
        self.assertEqual(p['screen_strip_depth_policy'],'single-sample-fragcoord-trunc20e4')
        self.assertFalse(p['screen_strip_native_state_verified']);self.assertFalse(p['strip_interpolation_verified'])
        self.assertFalse(p['strip_extent_validated']);self.assertTrue(p['strip_provoking_last_required'])

    def test_inventory_profile_and_actual_topology(self):
        payload={'vertex-shader.bin':vertex(),'pixel-shader.bin':pixel()}
        p=selected_profile_requirements(dict(primitive=6,index_count=0),payload,'screen-strip')
        self.assertTrue(p['translation']['accepted']);self.assertFalse(p['translation']['native_admission'])
        self.assertFalse(p['vertex']['issues']);self.assertFalse(p['pixel']['issues'])
        for primitive,indexed in ((4,0),(5,0),(8,0),(13,0),(6,1)):
            self.assertFalse(selected_profile_requirements(dict(primitive=primitive,index_count=indexed),payload,'screen-strip')['translation']['accepted'])

    def test_position_compute_retains_original_index_space(self):
        vs,_,p=translate_inline_pair(vertex(),pixel(),**OPTIONS,position_compute=True)
        self.assertIn('gl_GlobalInvocationID.x',vs);self.assertNotIn('stripCorners',vs)
        self.assertEqual(p['abi_version'],28)

    def test_old_profiles_and_wrong_resource_interfaces_reject(self):
        with self.assertRaises(RawShaderError):translate_inline_pair(vertex(),pixel(),extended_profile=True,loading_profile=True)
        for v,p in ((vertex(3),pixel()),(vertex(mask=3),pixel()),(vertex(),pixel(2))):
            with self.assertRaises(RawShaderError):translate_inline_pair(v,p,**OPTIONS)

    def test_conflicting_options_do_not_open_existing_native_abis(self):
        for key in ('indexed','direct_triangles','triangle_fan','quad_list','rectangle_list','cube_profile',
                    'geometry_profile','post_stencil_profile','screen_space_profile','offset_sample_profile',
                    'tonemap_profile','vertex_only','predicate_profile','color8_source_clamp'):
            with self.assertRaises(RawShaderError):translate_inline_pair(vertex(),pixel(),**{**OPTIONS,key:True})
        with self.assertRaises(RawShaderError):translate_inline_pair(vertex(),pixel(),**{**OPTIONS,'screen_strip_profile':1})

    def test_strip_expansion_extent_winding_and_provoking_vertex(self):
        from triangle_strip_topology import screen_strip_indices
        # Independent rolling-vertex strip assembly, including alternating
        # winding. Equality up to a CYCLIC rotation preserves orientation.
        for count in (3,4,5,6,99,100,101,32768):
            indices=screen_strip_indices(count)
            self.assertEqual(len(indices),3*(count-2))
            self.assertEqual((min(indices),max(indices)),(0,count-1))
            a,b=0,1
            for c in range(2,count):
                reference=(a,b,c) if c%2==0 else (b,a,c)
                actual=indices[3*(c-2):3*(c-1)]
                self.assertEqual(actual[0],c)  # guest last -> native first
                self.assertIn(actual,(reference,reference[1:]+reference[:1],reference[2:]+reference[:2]))
                a,b=b,c
        for count in (0,1,2,32769,-1,True,3.0,None):
            with self.assertRaises(RawShaderError):screen_strip_indices(count)

    def test_strip_keeps_degenerates_and_subsequent_parity(self):
        from triangle_strip_topology import screen_strip_indices
        # Deliberate repeated positions between two strips. Do not drop them
        # and accidentally invert the second strip or move its provoking vertex.
        vertices=[(0,0),(0,1),(1,0),(1,0),(2,0),(2,0),(2,1),(3,0)]
        triangles=[screen_strip_indices(len(vertices))[at:at+3] for at in range(0,18,3)]
        self.assertEqual(triangles,[(2,0,1),(3,2,1),(4,2,3),(5,4,3),(6,4,5),(7,6,5)])
        area=lambda t: ((vertices[t[1]][0]-vertices[t[0]][0])*(vertices[t[2]][1]-vertices[t[0]][1])-
                        (vertices[t[1]][1]-vertices[t[0]][1])*(vertices[t[2]][0]-vertices[t[0]][0]))
        self.assertEqual([area(t) for t in triangles],[-1,0,0,0,0,1])

if __name__=='__main__':unittest.main()
