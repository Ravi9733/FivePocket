"""Authored vertex predicate producer/store/evaluator/GLSL connections."""
from pathlib import Path
import math
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from raw_shader_analysis import RawShaderError
from vertex_arithmetic_dataflow import analyze_vertex_dataflow,evaluate_vertex_dataflow
from inline_shader_translation import translate_inline_pair
from test_extended_shader_math import vertex_code,scalar
from test_loading_shader_math import scalar_alu
from test_raw_shader_analysis import alu,program,cf_alloc,cf_exec
from test_vertex_arithmetic_dataflow import math_alu

OPTIONS=dict(dot4=True,scalar_constants=True,loading_math=True,packed_geometry=True,predicate_profile=True)
TRANSLATE=dict(indexed=True,extended_profile=True,loading_profile=True,geometry_profile=True,vertex_predicate_profile=True)
def guarded(ins,condition=True):
    a,b,c=ins;return a,b|(1<<28)|(int(condition)<<27),c
def setp(source=0):return scalar_alu(28,source=source,mask=0)
def copy_constant(register=0,dest=0,mask=7):
    return math_alu(op=2,dest=dest,mask=mask,sources=(register,register,0),temps=(False,False,True),export=False)
def shader(items,tail=None):
    return vertex_code(items[0],[*items[1:],*(tail or [alu(dest=62),alu()])])
def pixel():return program([cf_alloc(),cf_exec()],[alu()])
def evaluate(items,source,constants=None,tail=None):
    return evaluate_vertex_dataflow(shader(items,tail),{95:struct.pack('>4f',*source)},
        {k:struct.pack('>4f',*v) for k,v in (constants or {}).items()},0,include_state=True,**OPTIONS)

class VertexPredicationTests(unittest.TestCase):
    def test_predicate_signed_zero_and_both_condition_directions(self):
        for value in (-4.,-0.,0.,4.):
            for condition in (False,True):
                source=[2.,3.,5.,value]
                out=evaluate([setp(),guarded(copy_constant(),condition)],source,{0:[9.,8.,7.,6.]})
                wanted=[9.,8.,7.,value] if (value!=0)==condition else source
                self.assertEqual(out['exports'],{'0':wanted,'62':wanted})
                self.assertEqual(out['predicate'],float(value!=0))
                self.assertEqual(out['previous_scalar'],float(value==0))

    def test_predicate_reads_pre_coissue_source(self):
        a,b,c=copy_constant(mask=15)
        a=(a&((1<<26)-1))|(28<<26)
        # Scalar source is pre-instruction r0.w, while vector overwrites r0.
        c=(c&~255)|(1<<29)
        for old in (0.,1.):
            out=evaluate([(a,b,c),guarded(copy_constant(register=1))],[2.,3.,5.,old],
                {0:[4.,4.,4.,0.],1:[9.,8.,7.,6.]})
            self.assertEqual(out['exports']['62'],[9.,8.,7.,0.] if old else [4.,4.,4.,0.])
            self.assertEqual(out['predicate'],float(old!=0))

    def test_false_instruction_skips_unknown_input_and_unsafe_overwritten_math(self):
        for source in (0,1):
            ins=guarded(scalar_alu(22,source=source,mask=0))
            self.assertEqual(evaluate([setp(),ins],[2.,3.,5.,0.])['previous_scalar'],1.)
            with self.assertRaises(RawShaderError):
                evaluate([setp(),ins],[2.,3.,5.,-1.])
        # True instruction executes even if no lane stores its scalar result.
        with self.assertRaises(RawShaderError):
            evaluate([setp(),guarded(scalar_alu(22,mask=0),False)],[2.,3.,5.,0.])

    def test_scalar_store_and_previous_result_are_conditionally_retained(self):
        for value in (0.,4.):
            out=evaluate([setp(),guarded(scalar_alu(40,mask=1))],[2.,3.,5.,value])
            self.assertEqual(out['exports']['62'],[2.,3.,5.,value])
            self.assertEqual(out['previous_scalar'],2. if value else 1.)
        # Retained previous is used by later scalar-result exports, independently
        # of whether the guarded instruction wrote a temporary component.
        for value in (0.,9.):
            out=evaluate([setp(),guarded(scalar_alu(40,mask=0))],[2.,3.,5.,value],
                tail=[alu(dest=62,vector=0,scalar=15),alu()])
            self.assertEqual(out['exports']['62'],[3. if value else 1.]*4)

    def test_complementary_export_paths_preserve_undefined_lanes_until_selected(self):
        items=[setp(),guarded(alu(dest=62),False),guarded(alu(dest=62),True)]
        code=shader(items,tail=[alu()])
        for value in (0.,1.):
            out=evaluate_vertex_dataflow(code,{95:struct.pack('>4f',2,3,5,value)},{},0,**OPTIONS)
            self.assertEqual(out['62'],[2.,3.,5.,value])
        vs,_,_=translate_inline_pair(code,pixel(),**TRANSLATE,geometry_mrt_formats=(0,None,None,None))
        self.assertIn('else {',vs)

    def test_glsl_vertex_predication_is_opt_in_and_does_not_change_pixel_profile(self):
        code=shader([setp(),guarded(scalar_alu(22,mask=0))])
        vs,ps,proof=translate_inline_pair(code,pixel(),**TRANSLATE,geometry_mrt_formats=(0,None,None,None))
        self.assertEqual(proof['abi_version'],14)
        self.assertTrue(proof['vertex_predicate_profile'])
        self.assertFalse(proof['predicate_profile'])
        self.assertFalse(proof['vertex']['predicate_hardware_verified'])
        depth=0;found=0
        for line in vs.split('void main() {',1)[1].splitlines():
            if 'inversesqrt(' in line:self.assertGreater(depth,0);found+=1
            depth+=line.count('{')-line.count('}')
        self.assertGreater(found,0)
        for options in ({**TRANSLATE,'vertex_predicate_profile':False},
                        {**TRANSLATE,'geometry_profile':False}):
            with self.assertRaises(RawShaderError):translate_inline_pair(code,pixel(),**options)

    def test_unknown_predicate_mutation_addressing_and_nonfinite_sources_reject(self):
        for items in ([guarded(copy_constant())],[setp(),guarded(setp())]):
            with self.assertRaises(RawShaderError):analyze_vertex_dataflow(shader(items),**OPTIONS)
        for word,bit in ((0,6),(0,7),(0,14),(1,29),(1,30),(1,31)):
            ins=list(copy_constant());ins[word]|=1<<bit
            with self.assertRaises(RawShaderError):analyze_vertex_dataflow(shader([setp(),guarded(ins)]),**OPTIONS)
        a,b,c=copy_constant()
        with self.assertRaises(RawShaderError):analyze_vertex_dataflow(shader([setp(),(a,b|(1<<27),c)]),**OPTIONS)
        for value in (math.inf,math.nan,2**-149):
            with self.assertRaises(RawShaderError):evaluate([setp()],[2.,3.,5.,value])
        for options in ({**OPTIONS,'predicate_profile':False},{**OPTIONS,'packed_geometry':False}):
            with self.assertRaises(RawShaderError):analyze_vertex_dataflow(shader([setp()]),**options)

if __name__=='__main__':unittest.main()
