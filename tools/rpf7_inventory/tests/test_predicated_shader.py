"""Finite-domain instruction predication fixtures, not original GPU equivalence."""
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from pixel_sample_dataflow import analyze_pixel_sample_dataflow, evaluate_pixel_sample_dataflow
from raw_shader_analysis import RawShaderError
from inline_shader_translation import translate_inline_pair
from test_inline_shader_translation import vertex
from test_raw_shader_analysis import alu, program, cf_alloc, cf_exec
from test_loading_shader_math import scalar_alu

OPTIONS=dict(ui_operations=True,extended_math=True,loading_math=True,explicit_lod_profile=True,predicate_profile=True)


def shader(items):
    return program([cf_alloc(),cf_exec(count=len(items))],items)


def predicated(instruction, condition=True):
    a,b,c=instruction
    return a,b|(1<<28)|(int(condition)<<27),c


def setp(source=0,mask=0):
    return scalar_alu(28,source=source,mask=mask)


class PredicatedShaderTests(unittest.TestCase):
    def test_larger_predicate_kernel_budget_is_explicit_and_bounded(self):
        from inline_capture_preflight import shader_requirements
        for count in (256,283,320,321):
            groups=[min(6,count-i) for i in range(0,count,6)]
            body=(len(groups)+2)//2
            # Repeated straight EXEC ranges keep this synthetic fixture small.
            controls=[cf_alloc()]+[cf_exec(address=body,count=n,opcode=2 if i==len(groups)-1 else 1)
                for i,n in enumerate(groups)]
            code=program(controls,[alu()]*6)
            if count<=320:
                proof=analyze_pixel_sample_dataflow(code,**OPTIONS)
                self.assertEqual(len(proof['executed_instruction_addresses']),count)
                self.assertFalse(shader_requirements(code,'pixel','predicated-lod')['issues'])
                _,_,translated=translate_inline_pair(vertex(1),code,extended_profile=True,loading_profile=True,
                    explicit_lod_profile=True,predicate_profile=True)
                self.assertEqual(translated['abi_version'],11 if count>256 else 10)
            else:
                with self.assertRaisesRegex(RawShaderError,'instruction budget'):
                    analyze_pixel_sample_dataflow(code,**OPTIONS)
                self.assertEqual(shader_requirements(code,'pixel','predicated-lod')['issues'][-1]['kind'],'instruction_budget')
            if count>256:
                with self.assertRaisesRegex(RawShaderError,'instruction budget'):
                    analyze_pixel_sample_dataflow(code,**(OPTIONS|{'predicate_profile':False}))

    def test_glsl_uses_separate_abi_and_guarded_math_not_eager_selection(self):
        code=shader([setp(),predicated(scalar_alu(22)),alu()])
        _,fragment,proof=translate_inline_pair(vertex(1),code,extended_profile=True,loading_profile=True,
            explicit_lod_profile=True,predicate_profile=True,rectangle_list=True)
        self.assertEqual(proof['abi_version'],10)
        self.assertFalse(proof['predicate_hardware_verified'])
        depth=0;operations=0
        for line in fragment.split('void main() {',1)[1].splitlines():
            if 'inversesqrt(' in line:
                self.assertGreater(depth,0,line)  # Inside a conditional, not unconditional main.
                operations+=1
            depth+=line.count('{')-line.count('}')
        self.assertGreater(operations,0)
        with self.assertRaises(RawShaderError):
            translate_inline_pair(vertex(1),code,extended_profile=True,loading_profile=True,explicit_lod_profile=True)

    def test_glsl_complementary_writes_do_not_require_old_unprovided_register(self):
        false=predicated(alu(dest=1,export=False),False)
        a,b,c=scalar_alu(0);true=predicated((a|(1<<8),b,c))
        _,fragment,_=translate_inline_pair(vertex(1),shader([setp(),false,true,alu(source=1)]),
            extended_profile=True,loading_profile=True,explicit_lod_profile=True,predicate_profile=True)
        self.assertNotIn('interpolator1',fragment)
        self.assertIn('else {',fragment)

    def evaluate(self,items,initial):
        return evaluate_pixel_sample_dataflow(shader(items),initial,{}, {},include_state=True,**OPTIONS)

    def test_setne_signed_zero_signs_and_mask_zero_side_effects(self):
        for value in (-4.,-0.,0.,4.):
            result=self.evaluate([setp(),alu()],{0:[2.,3.,5.,value]})
            self.assertEqual(result['exports']['0'],[2.,3.,5.,value])
            self.assertEqual(result['predicate'],float(value!=0))
            self.assertEqual(result['previous_scalar'],float(value==0))
        for value in (float('inf'),float('nan'),2**-149):
            with self.assertRaises(RawShaderError):
                self.evaluate([setp(),alu()],{0:[0.,0.,0.,value]})

    def test_false_guard_skips_invalid_math_and_preserves_register_and_previous(self):
        items=[setp(),predicated(scalar_alu(22,source=1)),alu()]
        for unsafe in (None,[0.,0.,0.,0.],[0.,0.,0.,-1.]):
            initial={0:[2.,3.,4.,0.]}
            if unsafe is not None:initial[1]=unsafe
            result=self.evaluate(items,initial)
            self.assertEqual(result['exports']['0'],initial[0])
            self.assertEqual(result['previous_scalar'],1.)
            self.assertEqual(result['predicate'],0.)
        with self.assertRaises(RawShaderError):
            self.evaluate(items,{0:[2.,3.,4.,1.],1:[0.]*4})

    def test_true_and_false_conditions_execute_only_selected_instruction(self):
        for condition in (False,True):
            items=[setp(),predicated(scalar_alu(22,source=1),condition),alu()]
            for value in (-0.,2.):
                initial={0:[2.,3.,4.,value],1:[0.,0.,0.,4.]}
                result=self.evaluate(items,initial)
                taken=bool(value)==condition
                self.assertEqual(result['exports']['0'],[.5]*4 if taken else initial[0])
                self.assertEqual(result['previous_scalar'],.5 if taken else float(value==0.))

    def test_guarded_zero_mask_scalar_still_updates_previous_when_executed(self):
        items=[setp(),predicated(scalar_alu(40,source=1,mask=0)),alu(vector=0,scalar=15)]
        for value in (0.,1.):
            result=self.evaluate(items,{0:[0.,0.,0.,value],1:[0.,0.,0.,16.]})
            self.assertEqual(result['exports']['0'],[4. if value else 1.]*4)

    def test_coissued_predicate_reads_old_source_before_vector_write(self):
        a,b,c=alu(export=False,source=1)
        instruction=(a&((1<<26)-1))|(28<<26),b,c  # scalar source remains old r0.w
        result=self.evaluate([instruction,alu()],{0:[1.,2.,3.,0.],1:[4.,5.,6.,7.]})
        self.assertEqual(result['exports']['0'],[4.,5.,6.,7.])
        self.assertEqual(result['predicate'],0.)
        self.assertEqual(result['previous_scalar'],1.)

    def test_complementary_writes_never_read_uninitialized_old_destination(self):
        false=predicated(alu(dest=1,export=False),False)
        a,b,c=scalar_alu(0);true=predicated((a|(1<<8),b,c))
        items=[setp(),false,true,alu(source=1)]
        for value in (0.,2.):
            initial={0:[3.,4.,5.,value]}  # r1 intentionally not supplied.
            result=self.evaluate(items,initial)
            self.assertEqual(result['exports']['0'],[5.]*4 if value else initial[0])

    def test_skipped_constant_source_not_required(self):
        a,b,c=scalar_alu(22,source=255)
        result=self.evaluate([setp(),predicated((a,b,c&~(1<<29))),alu()],{0:[0.]*4})
        self.assertEqual(result['exports']['0'],[0.]*4)

    def test_guarded_constant_scalar_retains_operand_modifier_checks(self):
        import struct
        from test_extended_shader_math import scalar
        items=[setp(),predicated(scalar(42,constant=1,ta=1,mask=1)),alu()]
        for value in (0.,1.):
            initial={0:[2.,3.,5.,value]}
            result=evaluate_pixel_sample_dataflow(shader(items),initial,
                {1:struct.pack('>4f',7,0,0,0)} if value else {},{},**OPTIONS)
            self.assertEqual(result['0'],[21.,3.,5.,value] if value else initial[0])

    def test_profile_unknown_predicate_mutation_addressing_and_control_guards(self):
        items=[setp(),predicated(scalar_alu(22)),alu()]
        with self.assertRaises(RawShaderError):
            analyze_pixel_sample_dataflow(shader(items),**(OPTIONS|{'predicate_profile':False}))
        for bad in ([predicated(scalar_alu(22)),alu()],
                    [setp(),predicated(setp()),alu()]):
            with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(shader(bad),**OPTIONS)
        for bit in (29,30,31):
            a,b,c=items[1]
            with self.assertRaises(RawShaderError):
                analyze_pixel_sample_dataflow(shader([setp(),(a,b|(1<<bit),c),alu()]),**OPTIONS)
        with self.assertRaises(RawShaderError):
            analyze_pixel_sample_dataflow(program([cf_alloc(),cf_exec(opcode=3)],[alu()]),**OPTIONS)


if __name__=='__main__':unittest.main()
