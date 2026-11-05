from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from inline_shader_translation import translate_inline_pair
from raw_shader_analysis import RawShaderError
from test_raw_shader_analysis import cf_alloc,cf_exec,program,alu
from test_raw_vertex_fetch_analysis import fetch
from test_pixel_sample_dataflow import texture
from test_vertex_arithmetic_dataflow import math_alu


def vertex(parameters=4):
    exports=[alu(dest=i,source=1) for i in range(parameters)]+[alu(dest=62,source=1)]
    return program([cf_exec(address=3,sequence=1,opcode=1),cf_alloc(2,size=parameters),
                    cf_alloc(1),cf_exec(address=4,count=len(exports)),0,0],
        [fetch(dst=1,slot=95,fmt=38,stride=4,swizzle=0x688),*exports])


def pixel(slots=(2,3),second_source=1):
    instruction=list(math_alu(op=0,dest=0,sources=(4,5,0),temps=(True,True,True)))
    instruction[1]|=1<<25
    return program([cf_exec(address=2,count=2,sequence=5,opcode=1),cf_alloc(),cf_exec(address=4),0],
        [texture(src=0,dst=4,slot=slots[0]),texture(src=second_source,dst=5,slot=slots[1]),instruction])


class InlineShaderTranslationTests(unittest.TestCase):
    def test_scalar_fan_preserves_only_written_lane_and_has_separate_abi(self):
        vs_code=program([cf_exec(address=3,sequence=1,opcode=1),cf_alloc(2),
                         cf_alloc(1),cf_exec(address=4,count=2),0,0],
            [fetch(dst=1,slot=95,fmt=38,stride=4,swizzle=0x688),
             alu(dest=0,source=1,vector=1),alu(dest=62,source=1)])
        ps_code=program([cf_alloc(),cf_exec()],[alu(swizzle=0x6C)])
        options=dict(extended_profile=True,loading_profile=True,triangle_fan=True,scalar_fan_profile=True)
        vs,ps,proof=translate_inline_pair(vs_code,ps_code,**options)
        self.assertEqual(proof['abi_version'],31)
        self.assertTrue(proof['scalar_fan_profile'])
        self.assertFalse(proof['scalar_fan_query_semantics_verified'])
        self.assertEqual(proof['vertex']['exports']['0'][1:],[None,None,None])
        self.assertIn('out float interpolator0;',vs)
        self.assertIn('in float interpolator0;',ps)
        self.assertIn('(interpolator0>=interpolator0?interpolator0:interpolator0)',ps)
        self.assertIn('color=vec4(n256,n256,n256,n256);',ps)
        for text in (vs,ps):
            self.assertNotIn('vec1',text)
            self.assertNotIn('interpolator0[',text)
        compute,_,cp=translate_inline_pair(vs_code,ps_code,position_compute=True,**options)
        self.assertEqual(cp['abi_version'],31)
        self.assertNotIn('interpolator',compute)
        self.assertIn('gl_GlobalInvocationID.x',compute)
        with self.assertRaises(RawShaderError):
            translate_inline_pair(vs_code,ps_code,extended_profile=True,loading_profile=True,triangle_fan=True)
        # Missing pixel lanes must fail, never be zero-filled or alias x.
        for swizzle in (0,0x55,0xAA,0xFF):
            bad=program([cf_alloc(),cf_exec()],[alu(swizzle=swizzle)])
            with self.assertRaises(RawShaderError):translate_inline_pair(vs_code,bad,**options)
        for extra in (dict(indexed=True),dict(direct_triangles=True),dict(quad_list=True),
                      dict(rectangle_list=True),dict(geometry_profile=True),dict(vertex_only=True),
                      dict(scalar_fan_profile=1),dict(triangle_fan=False),dict(loading_profile=False),
                      dict(extended_profile=False),dict(vertex_denormal_multiply_profile=True)):
            with self.assertRaises(RawShaderError):translate_inline_pair(vs_code,ps_code,**{**options,**extra})
        for mask in (0,2,3,5,7,15):
            other=program([cf_exec(address=3,sequence=1,opcode=1),cf_alloc(2),
                           cf_alloc(1),cf_exec(address=4,count=2),0,0],
                [fetch(dst=1,slot=95,fmt=38,stride=4,swizzle=0x688),
                 alu(dest=0,source=1,vector=mask),alu(dest=62,source=1)])
            with self.assertRaises(RawShaderError):translate_inline_pair(other,ps_code,**options)

    def test_cube_array_fetch_keeps_full_interpolator_and_explicit_lod(self):
        cube=list(texture(src=0,dst=4,slot=4))
        cube[0]=(cube[0]&~(63<<26))|(0<<26)|(1<<28)|(2<<30)
        cube[1]&=~(1<<28);cube[2]=0xC000
        ps_code=program([cf_exec(address=2,sequence=1,opcode=1),cf_alloc(),cf_exec(address=3),0],
            [cube,alu(source=4)])
        options=dict(extended_profile=True,loading_profile=True,cube_profile=True)
        vs,ps,proof=translate_inline_pair(vertex(1),ps_code,**options)
        self.assertEqual(proof['abi_version'],8)
        self.assertEqual(proof['texture_slots'],[4])
        self.assertFalse(proof['cube_sampling_state_verified'])
        self.assertIn('out vec4 interpolator0',vs)
        self.assertIn('in vec4 interpolator0',ps)
        self.assertIn('uniform sampler2DArray atlas0',ps)
        self.assertIn('sampleCubeBase(atlas0,vec3((interpolator0[0])-1.0,(interpolator0[1])-1.0,interpolator0[2]),true)',ps)
        self.assertNotIn('textureLod(',ps)
        _,gamma,_=translate_inline_pair(vertex(1),ps_code,**options,cube_pwl_rgb=True)
        self.assertLess(gamma.index('vec4 sampled0=sampleCubeBase'),gamma.index('sampled0.r=xenosPwlSample'))
        self.assertIn('sampled0.b=xenosPwlSample(sampled0.b)',gamma)
        self.assertNotIn('sampled0.a=xenosPwlSample',gamma)
        with self.assertRaises(RawShaderError):translate_inline_pair(vertex(1),ps_code,cube_pwl_rgb=True)
        _,point,p=translate_inline_pair(vertex(1),ps_code,**options,cube_filter='point')
        self.assertEqual(p['cube_filter'],'point');self.assertIn('interpolator0[2]),false)',point)
        with self.assertRaises(RawShaderError):translate_inline_pair(vertex(1),ps_code,**options,cube_filter='anisotropic')
        with self.assertRaises(RawShaderError):translate_inline_pair(vertex(1),ps_code,extended_profile=True,loading_profile=True)
        with self.assertRaises(RawShaderError):translate_inline_pair(vertex(1),ps_code,cube_profile=True)

    def test_cube_arithmetic_reaches_generated_shader(self):
        cube=list(math_alu(op=18,dest=0,sources=(0,0,0),temps=(True,True,True),export=False))
        cube[1]=(0xA6<<16)|(0xCD<<8)
        ps_code=program([cf_exec(address=2,count=1,opcode=1),cf_alloc(),cf_exec(address=3,count=1),0],[cube,alu()])
        _,ps,proof=translate_inline_pair(vertex(1),ps_code,extended_profile=True,loading_profile=True,cube_profile=True)
        self.assertEqual(proof['abi_version'],8)
        for lane in range(4):self.assertIn(f'xenosCube(vec3(interpolator0[0],interpolator0[1],interpolator0[2]))[{lane}]',ps)

    def test_rectangle_expands_original_exports_and_has_separate_abi(self):
        solid=program([cf_alloc(),cf_exec()],[alu()])
        options=dict(extended_profile=True,loading_profile=True,rectangle_list=True)
        vs,_,proof=translate_inline_pair(vertex(1),solid,**options)
        self.assertEqual(proof['abi_version'],8)
        self.assertTrue(proof['rectangle_list']);self.assertFalse(proof['rectangle_extent_validated'])
        self.assertIn('void originalVertex(uint index, out vec4 position, out vec4 interpolator0)',vs)
        self.assertIn('originalVertex(base+i,positions[i],parameters0[i])',vs)
        self.assertIn('uint[6](0u,1u,2u,2u,1u,3u)',vs)
        self.assertIn('(positions[b]-positions[a])+positions[c]',vs)
        self.assertIn('(parameters0[b]-parameters0[a])+parameters0[c]',vs)
        self.assertEqual(vs.count('gl_Position.xy=gl_Position.xy*viewport.'),1)
        self.assertNotIn('viewport.',vs.split('void main()')[0])
        compute,_,proof=translate_inline_pair(vertex(1),solid,**options,position_compute=True)
        self.assertIn('gl_GlobalInvocationID.x',compute);self.assertNotIn('originalVertex',compute)
        self.assertNotIn('viewport.',compute)
        for extra in (dict(indexed=True),dict(direct_triangles=True),dict(triangle_fan=True),dict(quad_list=True)):
            with self.assertRaises(RawShaderError):translate_inline_pair(vertex(1),solid,**options,**extra)
        with self.assertRaises(RawShaderError):translate_inline_pair(vertex(1),solid,rectangle_list=True)

    def test_explicit_quad_position_compute_keeps_legacy_expansion_and_guards(self):
        solid=program([cf_alloc(),cf_exec()],[alu()])
        options=dict(extended_profile=True,loading_profile=True,quad_list=True)
        raster,_,proof=translate_inline_pair(vertex(1),solid,**options)
        legacy,_,_=translate_inline_pair(vertex(1),solid,extended_profile=True,loading_profile=True)
        self.assertEqual(raster,legacy)
        self.assertTrue(proof['quad_list']);self.assertFalse(proof['triangle_fan'])
        compute,_,proof=translate_inline_pair(vertex(1),solid,**options,position_compute=True)
        self.assertIn('gl_GlobalInvocationID.x',compute)
        self.assertNotIn('gl_VertexIndex',compute)
        self.assertNotIn('viewport.',compute)
        self.assertIn('2052u+count*4u',compute)
        for extra in (dict(indexed=True),dict(direct_triangles=True),dict(triangle_fan=True)):
            with self.assertRaises(RawShaderError):translate_inline_pair(vertex(1),solid,**options,**extra)
        with self.assertRaises(RawShaderError):translate_inline_pair(vertex(1),solid,quad_list=True)
        with self.assertRaises(RawShaderError):
            translate_inline_pair(vertex(1),program([cf_alloc(),cf_exec()],[alu(dest=32)]),**options,position_compute=True)

    def test_position_compute_preserves_raw_position_and_bounds_every_write(self):
        solid=program([cf_alloc(),cf_exec()],[alu()])
        source,_,proof=translate_inline_pair(vertex(1),solid,extended_profile=True,
            loading_profile=True,triangle_fan=True,position_compute=True)
        self.assertTrue(proof['position_compute'])
        self.assertEqual(proof['abi_version'],6)
        self.assertIn('uint index=gl_GlobalInvocationID.x;',source)
        self.assertIn('count*4u',source)
        self.assertIn('2052u+count*4u',source)
        self.assertIn('count>65536u',source)
        self.assertIn('result.words[4u+index*4u+lane]',source)
        self.assertIn('position=vec4(',source)
        self.assertNotIn('gl_Position',source)
        self.assertNotIn('viewport.',source)
        self.assertNotIn('interpolator',source)
        with self.assertRaises(RawShaderError):
            translate_inline_pair(vertex(1),solid,position_compute=True)
        # Position-only execution must NOT bypass pixel export/side-effect
        # validation merely because a candidate primitive might be clipped.
        for destination in (1,32,63):
            invalid_pixel=program([cf_alloc(),cf_exec()],[alu(dest=destination)])
            with self.assertRaises(RawShaderError):
                translate_inline_pair(vertex(1),invalid_pixel,extended_profile=True,
                    loading_profile=True,triangle_fan=True,position_compute=True)
        with self.assertRaises(RawShaderError):
            translate_inline_pair(vertex(1),solid,indexed=True,extended_profile=True,
                loading_profile=True,triangle_fan=True,position_compute=True)

    def test_legacy_solid_and_single_texture_layouts_remain_supported(self):
        solid=program([cf_alloc(),cf_exec()],[alu()])
        vs,ps,proof=translate_inline_pair(vertex(1),solid,indexed=True)
        self.assertEqual(proof['texture_slots'],[])
        self.assertNotIn('sampler2D',ps)
        single=program([cf_exec(address=2,sequence=1,opcode=1),cf_alloc(),cf_exec(address=3),0],
            [texture(src=0,dst=4,slot=2),alu(source=4)])
        for count in (2,3):
            vs,ps,proof=translate_inline_pair(vertex(count),single)
            self.assertEqual(proof['texture_slots'],[2])
            self.assertEqual(proof['interpolators'],count)
            self.assertIn('texture(atlas0,interpolator0.xy)',ps)
            self.assertIn('out vec4 interpolator1',vs)
            self.assertNotIn('atlas1',ps)
            self.assertIn('const uint quad[6]',vs)

    def test_two_real_bindings_keep_their_own_coordinates_and_negation(self):
        vs,ps,proof=translate_inline_pair(vertex(),pixel(),indexed=True)
        self.assertEqual(proof['texture_slots'],[2,3])
        self.assertEqual((proof['stride'],proof['interpolators'],proof['abi_version']),(16,4,4))
        self.assertIn('binding=1) uniform sampler2D atlas0',ps)
        self.assertIn('binding=2) uniform sampler2D atlas1',ps)
        self.assertIn('texture(atlas0,interpolator0.xy)',ps)
        self.assertIn('texture(atlas1,interpolator1.xy)',ps)
        self.assertIn('(-sampled1[0])',ps)
        self.assertIn('out vec2 interpolator1',vs)
        self.assertIn('out vec4 interpolator3',vs)
        self.assertIn('uint index=uint(gl_VertexIndex)',vs)

    def test_unsupported_linkage_and_duplicate_slots_are_rejected(self):
        for vs,ps in ((vertex(),pixel((2,2))), (vertex(),pixel(second_source=6)),
                      (vertex(2),pixel()), (vertex(3),pixel())):
            with self.assertRaises(RawShaderError):translate_inline_pair(vs,ps,indexed=True)

    def test_extended_direct_triangles_repeated_samples_keep_coordinate_snapshots(self):
        arithmetic=math_alu(op=0,dest=0,mask=3,sources=(0,0,0),temps=(True,False,False),export=False)
        ps_code=program([cf_exec(address=3,count=3,sequence=17,opcode=1),cf_alloc(),
                         cf_exec(address=6,sequence=1,opcode=1),cf_exec(address=7),0,0],
                        [texture(src=0,dst=4,slot=2),arithmetic,texture(src=0,dst=5,slot=2),
                         texture(src=0,dst=6,slot=2),alu(source=6)])
        vs,ps,proof=translate_inline_pair(vertex(1),ps_code,extended_profile=True,direct_triangles=True)
        self.assertEqual((proof['abi_version'],proof['texture_slots'],proof['interpolators']),(5,[2],1))
        self.assertTrue(proof['experimental_profile']);self.assertTrue(proof['direct_triangles'])
        self.assertNotIn('const uint quad',vs)
        self.assertEqual(ps.count('uniform sampler2D'),1)
        self.assertEqual(ps.count('=texture(atlas0,'),3)
        self.assertIn('sampled0=texture(atlas0,vec2(interpolator0[0],interpolator0[1]))',ps)
        first,changed,last=ps.index('sampled0=texture'),ps.index('precise float'),ps.index('sampled2=texture')
        self.assertLess(first,changed);self.assertLess(changed,last)
        with self.assertRaises(RawShaderError):translate_inline_pair(vertex(1),ps_code)
        with self.assertRaises(RawShaderError):translate_inline_pair(vertex(1),ps_code,direct_triangles=True)
        with self.assertRaises(RawShaderError):
            translate_inline_pair(vertex(1),ps_code,indexed=True,extended_profile=True,direct_triangles=True)

    def test_extended_float3_and_reciprocal_do_not_enable_runtime_profile(self):
        vs_code=program([cf_exec(address=3,sequence=1,opcode=1),cf_alloc(2),cf_alloc(1),
                         cf_exec(address=4,count=2),0,0],
                        [fetch(dst=1,slot=95,fmt=57,stride=3,swizzle=0xA88),
                         alu(source=1),alu(dest=62,source=1)])
        # RCP of explicitly supplied scalar constant W, broadcast to color.
        reciprocal=((19<<26)|(1<<15)|(15<<20),0,(2<<24)|1)
        ps_code=program([cf_alloc(),cf_exec()],[reciprocal])
        vs,ps,proof=translate_inline_pair(vs_code,ps_code,extended_profile=True,direct_triangles=True)
        self.assertEqual(proof['stride'],12)
        self.assertIn('(1.0/uintBitsToFloat(readBE(1035u)))',ps)
        self.assertEqual(proof['abi_version'],5)
        with self.assertRaises(RawShaderError):translate_inline_pair(vs_code,ps_code)


if __name__=='__main__':unittest.main()
