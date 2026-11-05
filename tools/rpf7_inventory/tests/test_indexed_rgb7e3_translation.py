"""Authored offline ABI30 checks. No game bytes or live admission."""
from pathlib import Path
import sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from inline_shader_translation import translate_inline_pair
from raw_shader_analysis import RawShaderError
from test_raw_shader_analysis import program,cf_exec,cf_alloc,alu
from test_raw_vertex_fetch_analysis import fetch
from test_vertex_arithmetic_dataflow import math_alu
from indexed_rgb7e3_shader_policy import indexed_rgb7e3_add
import struct
OPTIONS=dict(indexed=True,extended_profile=True,loading_profile=True,geometry_profile=True,
             geometry_predicate_profile=True,indexed_rgb7e3_profile=True)
def vertex(stride=6,mask=7):
    return program([cf_exec(address=3,sequence=1,opcode=1),cf_alloc(2),
                    cf_alloc(1),cf_exec(address=4,count=2),0,0],
        [fetch(dst=0,slot=95,fmt=57,stride=stride,swizzle=0xA88),
         alu(dest=0,source=0,vector=mask),alu(dest=62,source=0)])
def pixel():
    return program([cf_exec(address=2,count=2,opcode=1),cf_alloc(),
                    cf_exec(address=4),0],
        [math_alu(op=25,dest=1,mask=0,sources=(0,1,0),temps=(False,False,True),export=False),
         math_alu(op=0,dest=1,mask=15,sources=(0,1,0),temps=(False,False,True),export=False),
         alu(dest=0,source=1)])
class IndexedRgb7e3Tests(unittest.TestCase):
    def test_unit_plus_all_signed_subnormal_extremes(self):
        for bits in (1,2,0x400000,0x7FFFFF):
            tiny=struct.unpack('>f',struct.pack('>I',bits))[0]
            for unit in (-1.,1.):
                for sign in (-1,1):
                    self.assertEqual(indexed_rgb7e3_add(unit,sign*tiny),unit)
                    self.assertEqual(indexed_rgb7e3_add(sign*tiny,unit),unit)
    def test_non_unit_denormal_add_stays_rejected(self):
        for unit in (0.,2.,.5,2**-126,float('inf'),float('nan')):
            with self.assertRaises(RawShaderError):indexed_rgb7e3_add(unit,2**-149)
    def test_normal_add_keeps_existing_policy(self):
        self.assertEqual(indexed_rgb7e3_add(.5,.25),.75)
        with self.assertRaises(RawShaderError):indexed_rgb7e3_add(float('inf'),1.)
    def test_unrounded_tiny_reference_rejected(self):
        with self.assertRaises(RawShaderError):indexed_rgb7e3_add(1.,2**-150)
    def test_separate_resource_free_interface_and_no_color_clamp(self):
        v,f,p=translate_inline_pair(vertex(),pixel(),**OPTIONS)
        self.assertEqual((p['abi_version'],p['stride'],p['interpolators'],p['texture_slots']),(30,24,1,[]))
        self.assertTrue(p['indexed']);self.assertFalse(p['indexed_rgb7e3_native_state_verified'])
        self.assertFalse(p['indexed_rgb7e3_physical_publication_verified'])
        self.assertIn('uint index=uint(gl_VertexIndex);',v)
        self.assertNotIn('interpolator0_3',f);self.assertNotIn('geometryStoreMrt',f)
        self.assertNotIn('color=clamp',f);self.assertNotIn('color.xy*(1.0/32.0)',f)
        self.assertIn('0x7f800000u',f)
    def test_discard_precedes_colour_and_quantized_depth(self):
        _,f,_=translate_inline_pair(vertex(),pixel(),**OPTIONS)
        kill=f.index('if (pixelKilled) discard;')
        self.assertLess(kill,f.index('    color=vec4('))
        self.assertLess(kill,f.index('    gl_FragDepth='))
        self.assertIn('geometryDepth20e4(floatBitsToUint(gl_FragCoord.z))',f)
        self.assertNotIn('early_fragment_tests',f)
    def test_position_compute_remains_raw_original(self):
        v,_,p=translate_inline_pair(vertex(),pixel(),**OPTIONS,geometry_position_compute=True)
        self.assertEqual(p['abi_version'],30);self.assertIn('gl_GlobalInvocationID',v)
        self.assertNotIn('viewport.scaleOffset',v);self.assertNotIn('discard;',v)
    def test_strict_extent_and_written_lanes(self):
        for v in (vertex(5),vertex(mask=15),vertex(mask=3)):
            with self.assertRaises(RawShaderError):translate_inline_pair(v,pixel(),**OPTIONS)
    def test_conflicts_and_old_profile_stay_rejected(self):
        for k in ('geometry_compute','world_strip_profile','screen_strip_profile','position_compute',
                  'geometry_depth_color_witness','fixed16_rectangle_depth_profile','vertex_address_profile',
                  'vertex_denormal_multiply_profile','rectangle_mrt_profile','tonemap_profile'):
            with self.assertRaises(RawShaderError):translate_inline_pair(vertex(),pixel(),**{**OPTIONS,k:True})
        for k in ('indexed','extended_profile','loading_profile','geometry_profile','geometry_predicate_profile'):
            with self.assertRaises(RawShaderError):translate_inline_pair(vertex(),pixel(),**{**OPTIONS,k:False})
        for v in (1,'yes'):
            with self.assertRaises(RawShaderError):translate_inline_pair(vertex(),pixel(),**{**OPTIONS,'indexed_rgb7e3_profile':v})
        with self.assertRaises(RawShaderError):translate_inline_pair(vertex(),pixel(),**{**OPTIONS,'indexed_rgb7e3_profile':False})
if __name__=='__main__':unittest.main()
