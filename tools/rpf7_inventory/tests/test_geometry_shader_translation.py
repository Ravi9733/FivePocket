"""Authored connected packed vertex / partial interpolator / multi-texture tests."""
from pathlib import Path
import math
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from inline_shader_translation import translate_inline_pair
from pixel_sample_dataflow import analyze_pixel_sample_dataflow,evaluate_pixel_sample_dataflow
from loading_shader_math import evaluate_loading_operation
from raw_shader_analysis import RawShaderError
from test_raw_shader_analysis import program,cf_exec,cf_alloc,alu
from test_raw_vertex_fetch_analysis import fetch
from test_packed_geometry_fetch import instruction
from test_pixel_sample_dataflow import texture

OPTIONS=dict(indexed=True,extended_profile=True,loading_profile=True,geometry_profile=True)

def vertex(mask0=3,mask1=5,half_offset=4):
    return program([cf_exec(address=3,count=3,sequence=21,opcode=1),cf_alloc(2,size=2),
                    cf_alloc(1),cf_exec(address=6,count=3),0,0],
        [fetch(dst=1,slot=95,stride=6,fmt=57,swizzle=0xA88),
         instruction(7,dst=2,mini=True,offset=3,stride=0),
         instruction(32,dst=3,mini=True,offset=half_offset,stride=0),
         alu(dest=62,source=1),alu(dest=0,source=2,vector=mask0),alu(dest=1,source=3,vector=mask1)])

def multi_pixel():
    return program([cf_exec(address=2,count=4,sequence=85,opcode=1),cf_alloc(),
                    cf_exec(address=6),0],
        [*(texture(src=0,dst=4+i,slot=slot) for i,slot in enumerate((5,6,3,4))),alu(source=7)])

class GeometryShaderTranslationTests(unittest.TestCase):
    def test_vertex_log_reaches_raster_varying_but_not_unrelated_position_cone(self):
        code=program([cf_exec(address=2,sequence=1,opcode=1),cf_alloc(2),
                      cf_alloc(1),cf_exec(address=3,count=2)],
            [fetch(dst=1,slot=95,stride=3,fmt=57,swizzle=0xA88),
             ((16<<26)|(15<<20)|(1<<15),0,(2<<24)),alu(dest=62,source=1)])
        pixel=program([cf_alloc(),cf_exec()],[alu(source=0)])
        raster,fragment,proof=translate_inline_pair(code,pixel,**OPTIONS)
        self.assertIn('log2(uintBitsToFloat(readBE(7u)))',raster)
        self.assertIn('interpolator0_0',fragment)
        compute,other,position=translate_inline_pair(code,pixel,**OPTIONS,geometry_position_compute=True)
        self.assertNotIn('log2(',compute)
        self.assertEqual(position['vertex'],proof['vertex'])
        self.assertEqual(other,fragment)
        self.assertFalse(proof['hardware_transcendentals_verified'])
        with self.assertRaises(RawShaderError):
            translate_inline_pair(code,pixel,indexed=True,extended_profile=True,loading_profile=True)

    def test_indexed_position_compute_keeps_pixel_validation_and_original_expressions(self):
        raster,pixel,baseline=translate_inline_pair(vertex(),multi_pixel(),**OPTIONS)
        compute,other,proof=translate_inline_pair(vertex(),multi_pixel(),**OPTIONS,geometry_position_compute=True)
        self.assertEqual(other,pixel)
        self.assertEqual(proof['vertex'],baseline['vertex'])
        self.assertEqual(proof['pixel'],baseline['pixel'])
        self.assertTrue(proof['geometry_position_compute'])
        self.assertFalse(proof['geometry_compute'])
        self.assertIn('uint index=gl_GlobalInvocationID.x',compute)
        self.assertIn('position=vec4(',compute)
        self.assertIn('result.words[4u+index*4u+lane]',compute)
        self.assertIn('2052u+count*6u',compute)
        self.assertNotIn('gl_Position',compute)
        self.assertNotIn('gl_VertexIndex',compute)
        self.assertNotIn('viewport.',compute)
        self.assertNotIn('interpolator',compute)
        for options in ({'geometry_profile':False},{'geometry_compute':True},{'position_compute':True}):
            with self.assertRaises(RawShaderError):
                translate_inline_pair(vertex(),multi_pixel(),**(OPTIONS|options),geometry_position_compute=True)
        for value in (1,None,'yes'):
            with self.assertRaises(RawShaderError):
                translate_inline_pair(vertex(),multi_pixel(),**OPTIONS,geometry_position_compute=value)
        # A position pass cannot bypass unsupported guest memory exports.
        bad=program([cf_alloc(),cf_exec()],[alu(dest=32)])
        with self.assertRaises(RawShaderError):
            translate_inline_pair(vertex(),bad,**OPTIONS,geometry_position_compute=True)

    def test_color8_depth_output_is_distinct_per_sample_guarded_abi(self):
        plain=translate_inline_pair(vertex(),multi_pixel(),**OPTIONS)
        for fmt in (0,1):
            vs,ps,proof=translate_inline_pair(vertex(),multi_pixel(),**OPTIONS,geometry_color8_format=fmt)
            self.assertIn('noperspective out float geometryNdcZ',vs)
            self.assertIn('geometryStoreSampleDepth(viewport.depthLimits);',vs)
            self.assertIn('constantDepth=limits.x==limits.y',vs)
            self.assertIn('gl_Position.w==0.0',vs)
            self.assertNotIn('geometryStoreSampleDepth',plain[0])
            self.assertEqual(proof['abi_version'],13)
            self.assertEqual(proof['geometry_color8_format'],fmt)
            self.assertTrue(proof['geometry_depth20e4'])
            self.assertTrue(proof['geometry_sample_depth'])
            self.assertNotIn('gl_FragCoord.z',ps)
            self.assertIn('interpolateAtSample(geometryNdcZ,gl_SampleID)',ps)
            self.assertIn('layout(offset=16) vec2 limits',ps)
            self.assertIn('binding=5,std430',ps)
            self.assertIn('gl_SampleID',ps)
            self.assertIn('interpolateAtOffset(packedInterpolator0,vec2(0.0))',ps)
            self.assertIn('gl_FragDepth=geometryExpand20e4',ps)
            self.assertIn(f'geometryStoreColor8Depth20e4(color,{fmt}u);',ps)
            self.assertNotIn('early_fragment_tests',ps)
            self.assertFalse(proof['geometry_resources_verified'])
        self.assertEqual(plain[2]['abi_version'],12)
        self.assertNotIn('gl_FragDepth',plain[1])
        _,centroid,proof=translate_inline_pair(vertex(),multi_pixel(),**OPTIONS,geometry_color8_format=1,geometry_centroid_mask=2)
        self.assertEqual(proof['geometry_centroid_mask'],2)
        self.assertIn('interpolateAtCentroid(packedInterpolator1)',centroid)
        self.assertIn('interpolateAtOffset(packedInterpolator0,vec2(0.0))',centroid)
        for mask in (-1,0x10000,True):
            with self.assertRaises(RawShaderError):translate_inline_pair(vertex(),multi_pixel(),**OPTIONS,geometry_centroid_mask=mask)
        for fmt in (-1,2,True,'gamma'):
            with self.assertRaises(RawShaderError):translate_inline_pair(vertex(),multi_pixel(),**OPTIONS,geometry_color8_format=fmt)
        for extra in ({'geometry_profile':False},{'geometry_compute':True},{'color8_source_clamp':True}):
            with self.assertRaises(RawShaderError):translate_inline_pair(vertex(),multi_pixel(),**{**OPTIONS,**extra},geometry_color8_format=1)

    def test_compute_observes_written_lanes_and_keeps_holes_unwritten(self):
        compute,pixel,proof=translate_inline_pair(vertex(),multi_pixel(),**OPTIONS,geometry_compute=True)
        self.assertTrue(proof['geometry_compute'])
        self.assertIn('index=gl_GlobalInvocationID.x',compute)
        self.assertIn('count*68u',compute)
        self.assertIn('index*68u+6u]=floatBitsToUint(interpolator1_2)',compute)
        self.assertNotIn('index*68u+5u]',compute)
        self.assertNotIn('gl_Position',compute)
        self.assertNotIn('gl_VertexIndex',compute)
        self.assertNotIn('scaleOffset',compute)
        self.assertNotIn('layout(location=',compute)
        self.assertIn('index*68u+64u+lane',compute)
        self.assertEqual(pixel,translate_inline_pair(vertex(),multi_pixel(),**OPTIONS)[1])
        with self.assertRaises(RawShaderError):
            translate_inline_pair(vertex(),multi_pixel(),geometry_compute=True)

    def test_packed_fetches_partial_lane_linkage_and_four_texture_bindings(self):
        vs,ps,proof=translate_inline_pair(vertex(),multi_pixel(),**OPTIONS)
        self.assertEqual(proof['abi_version'],12)
        self.assertEqual(proof['geometry_interpolator_masks'],{'0':3,'1':5})
        self.assertEqual(proof['texture_slots'],[5,6,3,4])
        self.assertIn('uint index=uint(gl_VertexIndex)',vs)
        self.assertIn('bitfieldExtract(int(packed1),30,2)',vs)
        self.assertIn('unpackHalf2x16(packed2High)[1]',vs)
        for shader,direction in ((vs,'out'),(ps,'in')):
            self.assertIn(f'layout(location=1) {direction} vec4 packedInterpolator1;',shader)
            self.assertIn('#define interpolator1_2 packedInterpolator1[2]',shader)
            self.assertNotIn('component=',shader)
            self.assertNotIn('packedInterpolator1=vec4',shader) # No fabricated hole values.
            self.assertNotIn('interpolator1_1',shader);self.assertNotIn('interpolator1_3',shader)
        self.assertIn('binding=4) uniform sampler2D atlas3',ps)
        self.assertEqual(ps.count('=texture('),4)
        self.assertLess(ps.index('sampled0=texture'),ps.index('sampled3=texture'))
        self.assertFalse(proof['geometry_resources_verified'])
        self.assertFalse(proof['geometry_numeric_domain_verified'])
        self.assertFalse(proof['hardware_transcendentals_verified'])

    def test_missing_lanes_are_rejected_only_if_consumed_never_zero_filled(self):
        for source,mask in ((0,4),(1,2),(1,8),(2,1)):
            ps=program([cf_alloc(),cf_exec()],[alu(source=source,vector=mask,zero=True)])
            with self.assertRaisesRegex(RawShaderError,'unwritten geometry'):
                translate_inline_pair(vertex(),ps,**OPTIONS)
        ps=program([cf_alloc(),cf_exec()],[alu(source=1,vector=4,zero=True)])
        _,fragment,_=translate_inline_pair(vertex(),ps,**OPTIONS)
        self.assertIn('interpolator1_2',fragment)

    def test_profile_bounds_do_not_change_legacy_admission(self):
        for options in ({},{'indexed':True,'extended_profile':True,'loading_profile':True},
                        {**OPTIONS,'indexed':False},{**OPTIONS,'loading_profile':False},
                        {**OPTIONS,'position_compute':True},{**OPTIONS,'cube_profile':True}):
            with self.assertRaises(RawShaderError):translate_inline_pair(vertex(),multi_pixel(),**options)
        with self.assertRaisesRegex(RawShaderError,'extent'):
            translate_inline_pair(vertex(half_offset=5),multi_pixel(),**OPTIONS)

    def test_geometry_log_ieee_opcode_is_distinct_opt_in_and_finite_domain(self):
        # Scalar16 exports log2(c0.w) to all four lanes. No vector operation.
        code=program([cf_alloc(),cf_exec()],[((16<<26)|(15<<20)|(1<<15),0,(2<<24))])
        opts=dict(ui_operations=True,extended_math=True,loading_math=True)
        with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(code,**opts)
        proof=analyze_pixel_sample_dataflow(code,**opts,geometry_profile=True)
        self.assertIn(('log2',proof['nodes'].index(('constant',0,3))),proof['nodes'])
        for value,expected in ((.125,-3),(1.,0),(2.,1),(16.,4)):
            result=evaluate_pixel_sample_dataflow(code,{}, {0:struct.pack('>4f',0,0,0,value)}, {},
                **opts,geometry_profile=True)
            self.assertEqual(result['0'],[expected]*4)
        for value in (0.,-0.,-1.,math.inf,math.nan,2**-149):
            with self.assertRaises(RawShaderError):evaluate_loading_operation('log2',[value])
        _,fragment,metadata=translate_inline_pair(vertex(),code,**OPTIONS)
        self.assertIn('log2(uintBitsToFloat(readBE(1031u)))',fragment)
        self.assertFalse(metadata['pixel']['log2_nonpositive_supported'])

if __name__=='__main__':unittest.main()
