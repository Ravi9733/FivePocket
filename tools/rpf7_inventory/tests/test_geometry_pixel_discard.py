"""Authored Xenos ALU -> guarded pixel dataflow -> native discard contracts.

Local primary reference: AluScalarOpcode/AluVectorOpcode in ReX/Xenia ucode.h.
Finite normal/zero policy; neither Xbox numeric equivalence nor GPU evidence.
"""
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
from test_raw_shader_analysis import alu,program,cf_alloc,cf_exec
from test_loading_shader_math import scalar_alu
from test_predicated_shader import predicated,setp,shader
from test_vertex_arithmetic_dataflow import math_alu
from test_geometry_shader_translation import vertex,OPTIONS as TRANSLATE
from test_pixel_sample_dataflow import texture

GEOMETRY=dict(ui_operations=True,extended_math=True,loading_math=True,geometry_profile=True)
OPTIONS=GEOMETRY|dict(geometry_predicate_profile=True)

def kill(source=1,right=2,mask=0,dest=3):
    return math_alu(op=25,dest=dest,mask=mask,sources=(source,right,0),temps=(True,True,True),export=False)

def evaluate(items,initial,constants=None):
    return evaluate_pixel_sample_dataflow(shader(items),initial,constants or {},{},include_state=True,**OPTIONS)

class GeometryPixelDiscardTests(unittest.TestCase):
    def test_scalar_comparison_fraction_zero_sign_and_opt_in(self):
        for opcode in (8,10,11):
            for value in (-2.75,-1.,-0.,0.,.25,1.,16777216.):
                code=shader([scalar_alu(opcode),alu()])
                expected=float(value>0) if opcode==8 else float(value!=0) if opcode==10 else value-math.floor(value)
                result=evaluate_pixel_sample_dataflow(code,{0:[4.,3.,2.,value]},{},{},**GEOMETRY)
                self.assertEqual(result['0'],[expected]*4)
                if value==0:self.assertEqual(struct.pack('>f',result['0'][0]),bytes(4))
                with self.assertRaises(RawShaderError):
                    analyze_pixel_sample_dataflow(code,**(GEOMETRY|dict(geometry_profile=False)))
            for value in (math.nan,math.inf,2**-149):
                with self.assertRaises(RawShaderError):
                    evaluate_pixel_sample_dataflow(code,{0:[0.,0.,0.,value]},{},{},**GEOMETRY)

    def test_scalar_previous_swizzle_abs_negation_and_export_masks(self):
        # W swizzle=1 selects X, then abs then negate: FRAC(-abs(-2.25))=.75.
        first=scalar_alu(11,source=128,swizzle=64,negate=True,mask=0)
        code=shader([first,alu(vector=0,scalar=15)])
        self.assertEqual(evaluate_pixel_sample_dataflow(code,{0:[-2.25,3.,4.,5.]},{},{},**GEOMETRY)['0'],[.75]*4)
        # Co-issued vector writes r0.w, scalar must still see original r0.w.
        a,b,c=math_alu(op=2,dest=0,mask=8,sources=(1,1,0),temps=(True,True,True),export=False)
        inst=((a&((1<<26)-1))|(8<<26)|(1<<20),b,c)
        result=evaluate_pixel_sample_dataflow(shader([inst,alu()]),{0:[2.,3.,4.,-1.],1:[7.]*4},{},{},**GEOMETRY)
        self.assertEqual(result['0'],[0.,3.,4.,7.])
        # vm/sm overlap means literal one on exports, not vector/scalar writes.
        inst=list(scalar_alu(8,mask=9));inst[0]|=(1<<15)|(8<<16)
        result=evaluate_pixel_sample_dataflow(shader([inst]),{0:[0.,0.,0.,-1.]},{},{},**GEOMETRY)
        self.assertEqual(result['0'],[0.,None,None,1.])

    def test_kill_uses_every_component_even_with_zero_write_mask(self):
        for lane in range(4):
            left=[0.]*4;left[lane]=1.
            result=evaluate([kill(),alu()],{0:[.5]*4,1:left,2:[0.]*4})
            self.assertTrue(result['discarded']);self.assertEqual(result['exports'],{})
        for value in (-1.,-0.,0.):
            result=evaluate([kill(),alu()],{0:[.5]*4,1:[value]*4,2:[0.]*4})
            self.assertFalse(result['discarded']);self.assertEqual(result['exports']['0'],[.5]*4)

    def test_false_predicate_does_not_read_missing_kill_operands(self):
        for condition in (True,False):
            skipped=0. if condition else 1.
            result=evaluate([setp(),predicated(kill(),condition),alu()],{0:[.5,.5,.5,skipped]})
            self.assertFalse(result['discarded'])
            with self.assertRaises(RawShaderError):
                evaluate([setp(),predicated(kill(),condition),alu()],{0:[.5,.5,.5,1.-skipped]})

    def test_both_predicate_conditions_and_latched_kill(self):
        for condition in (False,True):
            for value in (-0.,1.):
                result=evaluate([setp(),predicated(kill(),condition),alu()],
                    {0:[.5,.5,.5,value],1:[1.]*4,2:[0.]*4})
                self.assertEqual(result['discarded'],bool(value)==condition)
        result=evaluate([kill(),kill(2,1),alu()],{0:[.5]*4,1:[1.]*4,2:[0.]*4})
        self.assertTrue(result['discarded']) # Later false comparison cannot revive a pixel.

    def test_kill_result_writes_zero_for_survivors_and_scalar_coissues(self):
        a,b,c=kill(mask=1)
        # Scalar FRAC r0.w writes r3.y beside the vector result r3.x.
        inst=((a&((1<<26)-1))|(11<<26)|(3<<8)|(2<<20),b,c)
        result=evaluate([inst,alu(source=3)],{0:[0.,0.,0.,1.25],1:[0.]*4,2:[1.]*4,3:[9.]*4})
        self.assertFalse(result['discarded']);self.assertEqual(result['exports']['0'],[0.,.25,9.,9.])

    def test_kill_sources_preserve_swizzle_and_absolute_before_negation(self):
        a,b,c=kill(source=129)
        # -abs(r1) > r2, with a single positive comparison on W only.
        inst=(a,b|(1<<26),c)
        result=evaluate([inst,alu()],{0:[.5]*4,1:[-2.,-2.,-2.,-.5],2:[-1.]*4})
        self.assertTrue(result['discarded'])
        a,b,c=kill();inst=(a,b|(0x1B<<16),c) # source1 replicated W (relative swizzle)
        result=evaluate([inst,alu()],{0:[.5]*4,1:[1.,0.,0.,-1.],2:[0.]*4})
        self.assertFalse(result['discarded'])

    def test_unsupported_modes_and_exceptional_kill_operands_stay_rejected(self):
        code=shader([setp(),predicated(kill()),alu()])
        for opts in (GEOMETRY,OPTIONS|dict(geometry_profile=False),OPTIONS|dict(predicate_profile=True)):
            with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(code,**opts)
        for opcode in (24,26,27,28):
            a,b,c=kill();c=(c&~(31<<24))|(opcode<<24)
            with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(shader([(a,b,c),alu()]),**OPTIONS)
        for source in (64,192):
            with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(shader([kill(source),alu()]),**OPTIONS)
        with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(shader([predicated(kill()),alu()]),**OPTIONS)
        for value in (math.inf,math.nan,2**-149):
            with self.assertRaises(RawShaderError):evaluate([kill(),alu()],{0:[.5]*4,1:[value]*4,2:[0.]*4})

    def test_glsl_deferred_discard_after_fetches_before_color_depth_and_atomics(self):
        # Actual game has a fetch AFTER its KILLGT. Keep fetches unconditional
        # for helper derivatives, then discard before any observable output.
        code=program([cf_alloc(),cf_exec(address=1,count=4,sequence=16)],
            [setp(source=1),predicated(kill(0,0)),texture(src=0,dst=4),alu(source=4)])
        vs,ps,proof=translate_inline_pair(vertex(mask0=15,mask1=15),code,**TRANSLATE,
            geometry_mrt_formats=(0,0,0,0),geometry_predicate_profile=True)
        self.assertEqual(proof['abi_version'],15);self.assertTrue(proof['pixel']['deferred_discard'])
        self.assertFalse(proof['geometry_discard_hardware_verified'])
        body=ps.split('void main() {',1)[1]
        self.assertLess(body.index('=texture('),body.index('if (pixelKilled) discard;'))
        self.assertLess(body.index('if (pixelKilled) discard;'),body.index('color0=vec4'))
        self.assertLess(body.index('if (pixelKilled) discard;'),body.index('geometryStoreMrtDepth20e4();'))
        self.assertNotIn('early_fragment_tests',ps)
        self.assertNotIn('pixelKilled',vs)
        compute,same_pixel,position_proof=translate_inline_pair(vertex(mask0=15,mask1=15),code,**TRANSLATE,
            geometry_mrt_formats=(0,0,0,0),geometry_predicate_profile=True,geometry_position_compute=True)
        self.assertEqual(ps,same_pixel)
        self.assertEqual(proof['vertex'],position_proof['vertex'])
        self.assertIn('position=vec4(',compute)
        self.assertNotIn('pixelKilled',compute)
        self.assertNotIn('interpolator',compute)
        self.assertNotIn('geometryStoreSampleDepth',compute)
        for options in ({},dict(geometry_mrt_formats=(0,0,0,0),geometry_compute=True)):
            with self.assertRaises(RawShaderError):translate_inline_pair(vertex(),code,**TRANSLATE,
                geometry_predicate_profile=True,**options)

    def test_deferred_reference_keeps_post_kill_arithmetic_domain_explicit(self):
        # Helper arithmetic still validates; no claim of immediate termination.
        with self.assertRaises(RawShaderError):
            evaluate([kill(),scalar_alu(22,source=4),alu()],{0:[.5]*4,1:[1.]*4,2:[0.]*4,4:[0.]*4})

    def test_native_blend_source_is_clamped_not_prequantized(self):
        from test_mrt_shader_translation import mrt_pixel
        controls=(0x10001,0x7010001,0x7000706,0x10001)
        _,ps,proof=translate_inline_pair(vertex(),mrt_pixel(),**TRANSLATE,geometry_mrt_formats=(0,0,0,0),
            geometry_predicate_profile=True,geometry_blend_controls=controls)
        self.assertEqual(proof['geometry_blend_controls'],controls)
        body=ps.split('void main() {',1)[1]
        self.assertIn('geometryClampMrtBlendSource(color1)',body);self.assertIn('geometryClampMrtBlendSource(color2)',body)
        self.assertIn('geometryStoreMrtColor8(color0,0u)',body);self.assertNotIn('geometryStoreMrtColor8(color1',body)
        clamp=ps.split('void geometryClampMrtBlendSource',1)[1].split('void geometryStoreMrtColor8',1)[0]
        self.assertNotIn('roundEven',clamp);self.assertNotIn('*255',clamp)
        for bad in ((0x10001,),[0x10001]*4,(True,)*4,(0,)*4,(0x7010002,)*4):
            with self.assertRaises(RawShaderError):translate_inline_pair(vertex(),mrt_pixel(),**TRANSLATE,
                geometry_mrt_formats=(0,0,0,0),geometry_predicate_profile=True,geometry_blend_controls=bad)
        with self.assertRaises(RawShaderError):translate_inline_pair(vertex(),mrt_pixel(),**TRANSLATE,
            geometry_mrt_formats=(0,1,0,0),geometry_predicate_profile=True,geometry_blend_controls=controls)

if __name__=='__main__':unittest.main()
