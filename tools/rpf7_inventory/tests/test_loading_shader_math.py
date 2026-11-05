from pathlib import Path
import math
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from loading_shader_math import evaluate_loading_operation
from raw_shader_analysis import RawShaderError
from vertex_arithmetic_dataflow import analyze_vertex_dataflow, evaluate_vertex_dataflow
from pixel_sample_dataflow import analyze_pixel_sample_dataflow, evaluate_pixel_sample_dataflow
from inline_shader_translation import translate_inline_pair
from test_raw_shader_analysis import alu
from test_vertex_arithmetic_dataflow import math_alu
from test_extended_shader_math import vertex_code, scalar
from test_pixel_sample_dataflow import pixel


def scalar_alu(op, *, source=0, mask=15, clamp=False, negate=False, swizzle=0):
    return ((op << 26) | (mask << 20) | (int(clamp) << 25),
            swizzle | (int(negate) << 24), (2 << 24) | (1 << 29) | source)


VOPTIONS = dict(dot4=True, scalar_constants=True, loading_math=True)
POPTIONS = dict(ui_operations=True, extended_math=True, loading_math=True)


class LoadingShaderMathTests(unittest.TestCase):
    def test_scalar_add_truncate_and_add_previous_are_explicit(self):
        for opcode,values,expected in ((0,(2,3,5,16),18),(12,(2,3,5,-16.75),-16)):
            inst=scalar_alu(opcode)
            self.assertEqual(self.vertex(inst,values)['62'],[expected]*4)
            self.assertEqual(evaluate_pixel_sample_dataflow(pixel([inst,alu()]),
                {0:[0]*4},{},{0:list(values)},**POPTIONS)['0'],[expected]*4)
            with self.assertRaises(RawShaderError):
                analyze_pixel_sample_dataflow(pixel([inst,alu()]),ui_operations=True,extended_math=True)
        first=scalar_alu(12,mask=0);second=scalar_alu(1)
        self.assertEqual(self.vertex(first,(2,3,5,2.75),[second,alu(dest=62),alu()])['62'],[4.75]*4)
        self.assertEqual(evaluate_pixel_sample_dataflow(pixel([first,second,alu()]),
            {0:[0]*4},{},{0:[2,3,5,2.75]},**POPTIONS)['0'],[4.75]*4)
        with self.assertRaises(RawShaderError):self.vertex(second)
        for value in (-1.75,-.75,-0.,0.,.75,1.75,16777216.):
            result=evaluate_loading_operation('truncate',[value])
            self.assertEqual(result,math.trunc(value))
            if not result:self.assertEqual(math.copysign(1,result),math.copysign(1,value))

    def test_pixel_sge_includes_equal_and_signed_zero(self):
        initial={0:[0]*4,1:[-1.,-0.,0.,2.],2:[0.,0.,-0.,1.]}
        inst=math_alu(op=6,dest=0,sources=(1,2,0),temps=(True,True,True))
        self.assertEqual(evaluate_pixel_sample_dataflow(pixel([inst]),initial,{},
            {0:[0]*4},**POPTIONS)['0'],[0.,1.,1.,1.])
        with self.assertRaises(RawShaderError):
            analyze_pixel_sample_dataflow(pixel([inst]),ui_operations=True,extended_math=True)

    def vertex(self, instruction, values=(2,3,5,16), tail=None):
        return evaluate_vertex_dataflow(vertex_code(instruction, tail),
            {95:struct.pack('>4f', *values)}, {}, 0, **VOPTIONS)

    def test_scalar_w_x_and_unary_operations_both_stages(self):
        for opcode, expected in ((2,32), (5,16), (14,65536), (19,.0625),
                                 (22,.25), (25,14), (40,4)):
            instruction = scalar_alu(opcode)
            self.assertEqual(self.vertex(instruction)['62'], [expected]*4)
            result = evaluate_pixel_sample_dataflow(pixel([instruction, alu()]),
                {0:[0]*4}, {}, {0:[2,3,5,16]}, **POPTIONS)
            self.assertEqual(result['0'], [expected]*4)
            with self.assertRaises(RawShaderError):
                analyze_vertex_dataflow(vertex_code(instruction), dot4=True, scalar_constants=True)

    def test_abs_before_negation_and_relative_sources_still_reject(self):
        instruction = scalar_alu(25, source=128, negate=True)
        self.assertEqual(self.vertex(instruction, (-2,3,5,-16))['62'], [-14]*4)
        for source in (64, 192, 255):
            with self.assertRaises(RawShaderError): self.vertex(scalar_alu(22,source=source))
            with self.assertRaises(RawShaderError):
                analyze_pixel_sample_dataflow(pixel([scalar_alu(22,source=source),alu()]), **POPTIONS)

    def test_scalar_clamp_stores_only_and_keeps_raw_previous(self):
        instruction = scalar_alu(14, mask=1, clamp=True)
        result = self.vertex(instruction, (2,3,5,2),
            [alu(dest=62, vector=0, scalar=15), alu()])
        self.assertEqual(result['62'], [4]*4)
        self.assertEqual(result['0'], [1,3,5,2])
        ps = pixel([instruction, alu(vector=0,scalar=15)])
        self.assertEqual(evaluate_pixel_sample_dataflow(ps,{0:[0]*4},{},
            {0:[2,3,5,2]},**POPTIONS)['0'], [4]*4)
        # A retained scalar can itself be clamped at a later store, still
        # without overwriting the saved raw previous-scalar result.
        retained = list(alu(dest=62,vector=0,scalar=15)); retained[0] |= 1<<25
        self.assertEqual(self.vertex(instruction,(2,3,5,2),[retained,alu()])['62'],[1]*4)

    def test_zero_mask_updates_previous_and_multiply_previous(self):
        first = scalar_alu(40, mask=0)
        second = scalar_alu(3)
        self.assertEqual(self.vertex(first,tail=[second,alu(dest=62),alu()])['62'],[64]*4)
        with self.assertRaises(RawShaderError): self.vertex(second)

    def test_constant_scalar_clamp_is_opt_in_and_preserves_previous(self):
        instruction = list(scalar(42,constant=1,ta=1,mask=1));instruction[0] |= 1<<25
        code = vertex_code(instruction,[alu(dest=62,vector=0,scalar=15),alu()])
        result = evaluate_vertex_dataflow(code,{95:struct.pack('>4f',2,3,5,7)},
            {1:struct.pack('>4f',4,0,0,0)},0,**VOPTIONS)
        self.assertEqual(result['62'],[12]*4);self.assertEqual(result['0'],[1,3,5,7])
        with self.assertRaises(RawShaderError):
            analyze_vertex_dataflow(code,dot4=True,scalar_constants=True)

    def test_constant_scalar_exports_are_explicit_and_preserve_previous(self):
        # Existing stage allocation/mask rules govern the export; operand
        # decoding must not mistake a valid parameter export for memory output.
        for opcode,expected in ((42,12),(44,7),(46,1)):
            inst=list(scalar(opcode,constant=1,ta=1,mask=8));inst[0]|=1<<15
            code=vertex_code(inst,[alu(dest=62,vector=0,scalar=15)])
            result=evaluate_vertex_dataflow(code,{95:struct.pack('>4f',2,3,5,7)},
                {1:struct.pack('>4f',4,0,0,0)},0,**VOPTIONS)
            self.assertEqual(result['0'],[None,None,None,expected])
            self.assertEqual(result['62'],[expected]*4)
            with self.assertRaises(RawShaderError):
                analyze_vertex_dataflow(code,dot4=True,scalar_constants=True)
            for destination in (16,32,63):
                invalid=inst.copy();invalid[0]|=destination
                with self.assertRaises(RawShaderError):
                    analyze_vertex_dataflow(vertex_code(invalid),**VOPTIONS)
            # Scalar-only pixel export, same constant/temporary operands.
            result=evaluate_pixel_sample_dataflow(pixel([inst]),{0:[0]*4},
                {1:struct.pack('>4f',4,0,0,0)},{0:[2,3,5,7]},**POPTIONS)
            self.assertEqual(result['0'],[None,None,None,expected])

    def test_position_reference_does_not_claim_unused_math_is_supported(self):
        code=vertex_code(scalar_alu(14,mask=0)) # exp2(-140) is subnormal, unused by position.
        vertices={95:struct.pack('>4f',2,3,5,-140)}
        with self.assertRaises(RawShaderError):
            evaluate_vertex_dataflow(code,vertices,{},0,**VOPTIONS)
        self.assertEqual(evaluate_vertex_dataflow(code,vertices,{},0,position_only=True,**VOPTIONS),
                         {'62':[2,3,5,-140]})
        with self.assertRaises(RawShaderError):
            evaluate_vertex_dataflow(code,vertices,{},0,position_only=True)
        # The reference still analyzes all instructions, not just live exports.
        invalid=vertex_code(scalar_alu(41,mask=0))
        with self.assertRaises(RawShaderError):
            evaluate_vertex_dataflow(invalid,vertices,{},0,position_only=True,**VOPTIONS)

    def test_vector_add_dp3_max_compare_conditional_and_modifier_order(self):
        values=(-2,3,-5,7)
        for opcode, expected in ((0,[-4,6,-10,14]),(16,[38]*4),
                                 (5,[0]*4),(14,values),(2,values)):
            instruction = math_alu(op=opcode,dest=0,sources=(0,0,0),temps=(True,True,True),export=False)
            self.assertEqual(self.vertex(instruction,values)['62'],list(expected))
        # abs(r0) is applied independently before vector-source negation.
        instruction = list(math_alu(op=0,dest=0,sources=(128,0,0),temps=(True,True,True),export=False))
        instruction[1] |= 1<<26
        self.assertEqual(self.vertex(instruction,values)['62'],[-4,0,-10,0])
        instruction[0] |= 1<<24
        self.assertEqual(self.vertex(instruction,values)['62'],[0]*4)
        # Conditional must distinguish zero from positive and keep the correct
        # selected operand; non-selected W does not enter the DP3 sum.
        for left, expected in ((-1,5),(0,5),(1,3)):
            self.assertEqual(evaluate_loading_operation('select_gt0',[left,3,5]),expected)
        self.assertEqual(evaluate_loading_operation('greater',[1,0]),1)

    def test_unary_domain_rounding_and_signed_zero(self):
        for exponent in (-126,-20,0,1,20,127):
            self.assertEqual(evaluate_loading_operation('exp2',[exponent]),2.0**exponent)
        self.assertEqual(evaluate_loading_operation('exp2',[-150]),0)
        self.assertEqual(evaluate_loading_operation('exp2',[-4096]),0)
        for power in range(-60,61,2):
            self.assertEqual(evaluate_loading_operation('sqrt',[2.0**power]),2.0**(power//2))
            self.assertEqual(evaluate_loading_operation('rsqrt',[2.0**power]),2.0**(-power//2))
        self.assertEqual(struct.pack('>f',evaluate_loading_operation('sqrt',[-0.])),b'\x80\0\0\0')
        self.assertEqual(struct.pack('>f',evaluate_loading_operation('absolute',[-0.])),bytes(4))
        for operation, values in (('sqrt',[-1]),('rsqrt',[0]),('exp2',[-149]),('exp2',[128]),
             ('sqrt',[math.inf]),('greater',[math.nan,0]),('sqrt',[2**-149]),('sqrt',[.1]),
             ('select_gt0',[1,2,math.inf]),('unknown',[1]),('sqrt',[])):
            with self.assertRaises(RawShaderError):evaluate_loading_operation(operation,values)

    def test_pixel_setgt_cndgt_are_strict_componentwise_and_opt_in(self):
        # Authored vectors, no captured game payload. Both signed zeros select
        # the false branch. All three operands snapshot pre-instruction values.
        initial={0:[0]*4,1:[-1.,-0.,0.,1.],2:[3.,5.,7.,11.],3:[13.,17.,19.,23.]}
        for opcode,expected in ((5,[0.,0.,0.,0.]),(14,[13.,17.,19.,11.])):
            inst=math_alu(op=opcode,dest=0,sources=(1,2,3),temps=(True,True,True))
            code=pixel([inst])
            self.assertEqual(evaluate_pixel_sample_dataflow(code,initial,{}, {0:[0]*4},**POPTIONS)['0'],expected)
            for options in ({},{'ui_operations':True},{'ui_operations':True,'extended_math':True}):
                with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(code,**options)
        inst=math_alu(op=5,dest=0,sources=(1,0,0),temps=(True,False,True))
        self.assertEqual(evaluate_pixel_sample_dataflow(pixel([inst]),initial,
            {0:struct.pack('>4f',-2,0,1,0)}, {0:[0]*4},**POPTIONS)['0'],[1,0,0,1])
        inst=math_alu(op=14,dest=1,sources=(1,2,3),temps=(True,True,True),export=False)
        self.assertEqual(evaluate_pixel_sample_dataflow(pixel([inst,alu(source=1)]),initial,{},
            {0:[0]*4},**POPTIONS)['0'],[13,17,19,11])
        # Third-source absolute then negation, not ignored as a two-input op.
        inst=list(math_alu(op=14,dest=0,sources=(1,2,131),temps=(True,True,True)))
        inst[1]|=1<<24
        self.assertEqual(evaluate_pixel_sample_dataflow(pixel([inst]),initial,{},
            {0:[0]*4},**POPTIONS)['0'],[-13,-17,-19,11])
        initial[3][0]=math.inf
        with self.assertRaises(RawShaderError):
            evaluate_pixel_sample_dataflow(pixel([inst]),initial,{}, {0:[0]*4},**POPTIONS)

    def test_unknown_predicate_kill_address_and_flags_reject(self):
        for opcode in (23,24,27,35,48,51,63):
            with self.assertRaises(RawShaderError): self.vertex(scalar_alu(opcode))
        for word, bit in ((0,6),(0,7),(0,14),(1,27),(1,28),(1,29),(1,30),(1,31)):
            instruction = list(scalar_alu(40));instruction[word]|=1<<bit
            with self.assertRaises(RawShaderError):self.vertex(instruction)
        with self.assertRaises(RawShaderError):analyze_vertex_dataflow(vertex_code(scalar_alu(40)),loading_math=True)
        with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(pixel(),loading_math=True)

    def test_translation_uses_distinct_abi_and_fan_never_enables_native_admission(self):
        vs = vertex_code(scalar_alu(22))
        ps = pixel([scalar_alu(40),alu()])
        vertex, fragment, proof = translate_inline_pair(vs,ps,extended_profile=True,
            loading_profile=True,triangle_fan=True)
        self.assertEqual(proof['abi_version'],6)
        self.assertTrue(proof['triangle_fan']);self.assertFalse(proof['fan_extent_validated'])
        self.assertFalse(proof['hardware_transcendentals_verified'])
        self.assertIn('inversesqrt(',vertex);self.assertIn('sqrt(',fragment)
        self.assertNotIn('quad[',vertex);self.assertIn('corner==0u?0u:',vertex)
        for options in ({}, {'extended_profile':True}, {'loading_profile':True},
                        {'extended_profile':True,'triangle_fan':True},
                        {'extended_profile':True,'loading_profile':True,'triangle_fan':True,'indexed':True}):
            with self.assertRaises(RawShaderError):translate_inline_pair(vs,ps,**options)

    def test_fixed_point_source_clamp_is_an_explicit_raster_abi(self):
        vs=vertex_code(scalar_alu(22));ps=pixel([scalar_alu(40),alu()])
        options=dict(extended_profile=True,loading_profile=True,triangle_fan=True)
        old_vertex,old_pixel,old=translate_inline_pair(vs,ps,**options)
        vertex,fragment,proof=translate_inline_pair(vs,ps,**options,color8_source_clamp=True)
        self.assertEqual(vertex,old_vertex);self.assertEqual(proof['abi_version'],7)
        self.assertTrue(proof['color8_source_clamp']);self.assertFalse(old['color8_source_clamp'])
        self.assertNotIn('color=clamp(color,',old_pixel)
        self.assertEqual(fragment.count('color=clamp(color,vec4(0.0),vec4(1.0));'),1)
        for invalid in ({},dict(extended_profile=True),dict(extended_profile=True,loading_profile=True),dict(**options,position_compute=True)):
            with self.assertRaises(RawShaderError):translate_inline_pair(vs,ps,**invalid,color8_source_clamp=True)


if __name__ == '__main__': unittest.main()
