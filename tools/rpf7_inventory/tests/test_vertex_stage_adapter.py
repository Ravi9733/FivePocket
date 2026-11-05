from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from vertex_arithmetic_dataflow import (translate_vertex_arithmetic_stage,translate_r8_vertex_arithmetic_stage,
    translate_color1280_vertex_stage)
from pixel_sample_dataflow import translate_texture32_pixel_fragment,translate_r8_planes_pixel_fragment,R8_PLANE_IMAGE_BYTES
from test_raw_shader_analysis import cf_alloc,cf_exec,alu,program
from test_raw_vertex_fetch_analysis import fetch
from test_pixel_sample_dataflow import pixel
from test_pixel_dot_scalar_dataflow import triple


def vertex():
    return program([cf_exec(address=3,sequence=1,opcode=1),cf_alloc(1),cf_exec(address=4,opcode=1),cf_alloc(2),
                    cf_exec(address=5,count=2),0],[fetch(dst=0,fmt=38,stride=4,swizzle=0x688),
                    alu(dest=62),alu(dest=0,vector=3),alu(dest=1)])


class VertexStageAdapterTests(unittest.TestCase):
    def test_color_mapping_is_separate_from_original_exports(self):
        old,old_proof=translate_vertex_arithmetic_stage(vertex(),64,4144)
        mapped,proof=translate_color1280_vertex_stage(vertex(),64,4144)
        self.assertNotIn('mappedXY',old);self.assertNotIn('coordinate_adapter',old_proof)
        self.assertIn('precise vec2 scaledXY=gl_Position.xy*vec2(0.625,-0.703125);',mapped)
        self.assertIn('scaledXY+vec2(-0.37451171875,-0.2958984375)',mapped)
        self.assertIn('if (gl_Position.w!=1.0)',mapped)
        self.assertGreater(mapped.index('gl_Position.xy=mappedXY;'),mapped.index('vertexTint=vec4(',mapped.index('void main()')))
        self.assertEqual(proof['exports'],old_proof['exports'])
        self.assertEqual(proof['required_native_viewport'],[0,0,2048,1024,1,0])
        self.assertEqual(proof['required_native_scissor'],[0,0,1280,720])
        for x in (-1,1):
            for y in (-1,1):
                # Exact binary arithmetic: native mapping equals guest scale,
                # offset and half-pixel offset for all four captured corners.
                nx=x*.625-.37451171875;ny=y*-.703125-.2958984375
                self.assertEqual((nx+1)*1024,640*x+640.5)
                self.assertEqual((ny+1)*512,-360*y+360.5)
                self.assertTrue(-1<nx<1 and -1<ny<1)
        self.assertFalse(proof['title_raster_state_replayed'])
        self.assertFalse(proof['original_clip_boundary_preservation_verified'])
        self.assertFalse(proof['whole_primitive_rejection_guaranteed'])
        self.assertNotIn('fma(',mapped)

    def test_color_mapping_preserves_interface_and_bounds_rejections(self):
        full=program([cf_exec(address=3,sequence=1,opcode=1),cf_alloc(1),cf_exec(address=4,opcode=1),cf_alloc(2),
                      cf_exec(address=5,count=2),0],[fetch(dst=0,fmt=38,stride=4,swizzle=0x688),
                      alu(dest=62),alu(dest=0),alu(dest=1)])
        base=R8_PLANE_IMAGE_BYTES+96
        mapped,proof=translate_color1280_vertex_stage(full,64,base,r8_planes=True)
        self.assertIn('layout(location=0) out vec4 coordinate;',mapped)
        self.assertEqual(proof['input_bytes'],base+64)
        for value in (0,1,None,'r8',[],{}):
            with self.assertRaises(ValueError):translate_color1280_vertex_stage(vertex(),64,4144,r8_planes=value)
        for address in (-1,1,True,4608):
            with self.assertRaises(ValueError):translate_color1280_vertex_stage(vertex(),64,address)
        with self.assertRaises(ValueError):translate_color1280_vertex_stage(full,64,base)
        with self.assertRaises(ValueError):translate_color1280_vertex_stage(vertex(),64,base,r8_planes=True)

    def test_r8_profile_has_separate_cap_and_full_parameter(self):
        full=program([cf_exec(address=3,sequence=1,opcode=1),cf_alloc(1),cf_exec(address=4,opcode=1),cf_alloc(2),
                      cf_exec(address=5,count=2),0],[fetch(dst=0,fmt=38,stride=4,swizzle=0x688),
                      alu(dest=62),alu(dest=0),alu(dest=1)])
        base=R8_PLANE_IMAGE_BYTES+96
        source,proof=translate_r8_vertex_arithmetic_stage(full,64,base)
        self.assertEqual(proof['input_bytes'],base+64)
        self.assertEqual(proof['exported_interface']['parameter0'],'xyzw')
        self.assertIn('layout(location=0) out vec4 coordinate;',source)
        with self.assertRaises(ValueError):translate_vertex_arithmetic_stage(full,64,base)
        with self.assertRaises(ValueError):translate_r8_vertex_arithmetic_stage(vertex(),64,base)
        for bad in (0,4144,R8_PLANE_IMAGE_BYTES,R8_PLANE_IMAGE_BYTES+20,base+16):
            with self.assertRaises(ValueError):translate_r8_vertex_arithmetic_stage(full,64,bad)

    def test_r8_fragment_accepts_explicit_vertex_tint_interface(self):
        source,proof=translate_r8_planes_pixel_fragment(triple(),vertex_tint=True)
        self.assertTrue(proof['vertex_tint_input'])
        self.assertFalse(proof['synthetic_vertex_partner'])
        self.assertTrue(proof['deferred_arithmetic_guards'])
        self.assertIn('layout(location=1) in vec4 vertexTint;',source)
        self.assertIn('layout(location=0) in vec4 coordinate;',source)

    def test_bounded_interface_and_explicit_base(self):
        source,proof=translate_vertex_arithmetic_stage(vertex(),64,4144)
        self.assertEqual(proof['input_bytes'],4208);self.assertEqual(proof['probe_stage'],'vertex')
        self.assertFalse(proof['title_linkage_verified']);self.assertFalse(proof['whole_primitive_rejection_guaranteed'])
        self.assertIn('gl_VertexIndex',source);self.assertIn('at+1036u',source)
        self.assertIn('layout(location=0) out vec2 coordinate;',source)
        self.assertIn('layout(location=1) out vec4 vertexTint;',source)
        self.assertNotIn('result.words',source);self.assertNotIn('gl_GlobalInvocationID',source)
        self.assertIn('gl_Position=vec4(',source)

    def test_invalid_base_and_unknown_output_reject(self):
        for base in (-1,1,True,4608):
            with self.assertRaises(ValueError):translate_vertex_arithmetic_stage(vertex(),64,base)
        bad=program([cf_alloc(1),cf_exec()],[alu(dest=62)])
        with self.assertRaises(ValueError):translate_vertex_arithmetic_stage(bad,64,0)

    def test_pixel_tint_comes_from_vertex_not_header(self):
        source,proof=translate_texture32_pixel_fragment(pixel(),vertex_tint=True)
        self.assertTrue(proof['vertex_tint_input']);self.assertFalse(proof['synthetic_vertex_partner'])
        self.assertIn('layout(location=1) in vec4 vertexTint;',source)
        self.assertIn('vertexTint[0]',source);self.assertNotIn('readBE(1024u)',source)


if __name__=='__main__':unittest.main()
