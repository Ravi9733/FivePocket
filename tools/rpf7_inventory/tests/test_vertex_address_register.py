"""Authored MAX_AS/MOVA -> relative constant consumer, no game payloads.

Reference: pinned Xenia 95a5c3ee250f80c3b9d139658649d9ffb6db3eec
src/xenia/gpu/ucode.h, AluInstruction::src_const_is_addressed and kMaxAs.
Finite float32 inputs only; out-of-bank addressing is rejected, not wrapped.
"""
import math
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from raw_shader_analysis import RawShaderError
from vertex_arithmetic_dataflow import analyze_vertex_dataflow,evaluate_vertex_dataflow
from inline_shader_translation import translate_inline_pair
from test_raw_shader_analysis import alu
from test_extended_shader_math import vertex_code
from test_loading_shader_math import scalar_alu
from test_vertex_arithmetic_dataflow import math_alu
from test_geometry_gradient_depth import gradient
from test_pixel_sample_dataflow import pixel

OPTIONS=dict(dot4=True,scalar_constants=True,loading_math=True,packed_geometry=True,address_profile=True)
TRANSLATE=dict(indexed=True,extended_profile=True,loading_profile=True,geometry_profile=True,
    geometry_gradient_depth_profile=True,vertex_address_profile=True)

def relative_copy(base=0,dest=1):
    a,b,c=math_alu(op=2,dest=dest,sources=(base,base,0),temps=(False,False,True),export=False)
    return a,b|(7<<29),c

def code(items,export_source=1):
    return vertex_code(items[0],[*items[1:],alu(dest=62,source=export_source),alu(source=export_source)])

def evaluate(items,source,constants=None,export_source=1,**extra):
    return evaluate_vertex_dataflow(code(items,export_source),{95:struct.pack('>4f',*source)},
        {k:struct.pack('>4f',*v) for k,v in (constants or {}).items()},0,**OPTIONS,**extra)

class VertexAddressRegisterTests(unittest.TestCase):
    def test_rounding_clamp_and_real_constant_selection(self):
        for opcode in (23,24):
            for value in (-1000.,-256.75,-1.5,-.5000000596046448,-.5,-0.,0.,.4999999701976776,.5,1.5,254.5,1000.):
                adjusted=struct.unpack('>f',struct.pack('>f',value+.5))[0] if opcode==23 else value
                expected=max(-256,min(255,math.floor(adjusted)))
                base=max(0,-expected)
                if base>255:
                    with self.assertRaises(RawShaderError):evaluate([scalar_alu(opcode,mask=0),relative_copy()],(2,3,5,value),{})
                    continue
                bank={base+expected:(9,8,7,6)}
                out=evaluate([scalar_alu(opcode,mask=0),relative_copy(base)],(2,3,5,value),bank,include_state=True)
                self.assertEqual(out['exports']['62'],[9,8,7,6])
                self.assertEqual(out['address_register'],expected)

    def test_zero_mask_still_mutates_address_but_scalar_result_is_max(self):
        for mask in (0,15):
            out=evaluate([scalar_alu(23,mask=mask)],(7,3,5,2.25),export_source=0,include_state=True)
            self.assertEqual(out['address_register'],2)
            self.assertEqual(out['previous_scalar'],7)
            self.assertEqual(out['exports']['62'],[7]*4 if mask else [7,3,5,2.25])

    def test_coissue_reads_old_address_and_prewrite_temporary(self):
        a,b,c=relative_copy(base=10,dest=0)
        # Scalar src3=r0.w changes a0; vector consumes old a0 and overwrites r0.
        coissued=((a&((1<<26)-1))|(23<<26),b|64,c)
        out=evaluate([scalar_alu(23,mask=0),coissued,relative_copy(10)],(2,3,5,4),
            {14:(99,99,99,99),12:(7,8,9,10)},include_state=True)
        self.assertEqual(out['address_register'],2)
        self.assertEqual(out['exports']['62'],[7,8,9,10])

    def test_first_and_second_constant_flags_follow_operand_kinds(self):
        # Operand1 is temporary; operand2 is the FIRST constant, controlled by31.
        a,b,c=math_alu(op=1,dest=1,sources=(0,10,0),temps=(True,False,True),export=False)
        first=(a,b|(1<<31)|(1<<29),c)
        out=evaluate([scalar_alu(23,mask=0),first],(2,3,5,4),{14:(2,2,2,2)})
        self.assertEqual(out['62'],[4,6,10,8])
        # With operand1 constant, operand2 uses30 and operand1 remains absolute.
        a,b,c=math_alu(op=1,dest=1,sources=(3,10,0),temps=(False,False,True),export=False)
        second=(a,b|(1<<30)|(1<<29),c)
        out=evaluate([scalar_alu(23,mask=0),second],(2,3,5,4),{3:(3,3,3,3),14:(2,2,2,2)})
        self.assertEqual(out['62'],[6]*4)

    def test_abs_negation_and_swizzle_precede_address_rounding(self):
        op=scalar_alu(23,source=128,mask=0,negate=True,swizzle=64)
        out=evaluate([op,relative_copy(10)],(-2.25,3,5,4),{8:(4,3,2,1)},include_state=True)
        self.assertEqual(out['address_register'],-2)
        self.assertEqual(out['exports']['62'],[4,3,2,1])

    def test_unsafe_bank_and_unknown_address_fail_without_wrapping(self):
        with self.assertRaises(RawShaderError):analyze_vertex_dataflow(code([relative_copy()]),**OPTIONS)
        for value,base in ((-1,0),(255,1)):
            with self.assertRaises(RawShaderError):evaluate([scalar_alu(23,mask=0),relative_copy(base)],(2,3,5,value),{0:(1,1,1,1),255:(1,1,1,1)})
        for value in (math.inf,math.nan,2**-149):
            with self.assertRaises(RawShaderError):evaluate([scalar_alu(23,mask=0),relative_copy()],(2,3,5,value),{0:(1,1,1,1)})

    def test_new_profile_does_not_admit_loops_predication_or_old_profiles(self):
        shader=code([scalar_alu(23,mask=0),relative_copy()])
        for changes in ({'address_profile':False},{'address_profile':1},{'packed_geometry':False},{'predicate_profile':True}):
            with self.assertRaises(RawShaderError):analyze_vertex_dataflow(shader,**{**OPTIONS,**changes})
        a,b,c=relative_copy()
        with self.assertRaises(RawShaderError):analyze_vertex_dataflow(code([scalar_alu(23,mask=0),(a,b&~(1<<29),c)]),**OPTIONS)

    def test_glsl_bounds_guard_precedes_load_and_compute_marks_fault(self):
        shader=code([scalar_alu(23,mask=0),relative_copy()])
        ps=pixel([math_alu(op=2,dest=61,mask=1,sources=(0,0,0),temps=(True,True,True)),alu()],fetch=gradient())
        vs,_,proof=translate_inline_pair(shader,ps,**TRANSLATE)
        self.assertEqual(proof['abi_version'],17)
        self.assertTrue(proof['vertex']['dynamic_constants'])
        self.assertIn('atomicOr(geometryCompletion.words[4],8u)',vs)
        self.assertLess(vs.index('>=256'),vs.index('readBE(4u+uint(constantIndex'))
        comp,_,_=translate_inline_pair(shader,ps,**TRANSLATE,geometry_position_compute=True)
        self.assertIn('0x7fc00000u',comp)
        self.assertNotIn('geometryCompletion',comp)
        for changes in ({'vertex_address_profile':False},{'vertex_address_profile':1},{'geometry_gradient_depth_profile':False}):
            with self.assertRaises(RawShaderError):translate_inline_pair(shader,ps,**{**TRANSLATE,**changes})

if __name__=='__main__':unittest.main()
