"""Authored SETGTs analysis tests, not native draw or Xbox numeric evidence."""
from pathlib import Path
import math,struct,sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from raw_shader_analysis import RawShaderError
from vertex_arithmetic_dataflow import analyze_vertex_dataflow,evaluate_vertex_dataflow
from test_extended_shader_math import vertex_code
from test_loading_shader_math import scalar_alu
from test_raw_shader_analysis import alu
from test_vertex_arithmetic_dataflow import math_alu

OPTIONS=dict(dot4=True,scalar_constants=True,loading_math=True,scalar_gt_zero=True)

def evaluate(instruction,values,tail=None,**overrides):
    return evaluate_vertex_dataflow(vertex_code(instruction,tail),
        {95:struct.pack('>4f',*values)},{},0,**{**OPTIONS,**overrides})

class VertexScalarSetGtTests(unittest.TestCase):
    def test_signed_zero_negative_and_positive(self):
        for value in (-3.,-2.**-126,-0.,0.,2.**-126,3.,2.**127):
            out=evaluate(scalar_alu(8),(2,3,5,value))['62']
            self.assertEqual(out,[float(value>0)]*4)
            if not out[0]:self.assertEqual(math.copysign(1,out[0]),1)

    def test_old_profiles_remain_closed(self):
        code=vertex_code(scalar_alu(8))
        for override in ({'scalar_gt_zero':False},{'scalar_gt_zero':1},{'dot4':False},
                         {'scalar_constants':False},{'loading_math':False},
                         {'packed_geometry':True},{'predicate_profile':True},
                         {'address_profile':True},{'tonemap':True}):
            with self.assertRaises(RawShaderError):analyze_vertex_dataflow(code,**{**OPTIONS,**override})
        for opcode in (7,9,10,16,28,30):
            with self.assertRaises(RawShaderError):analyze_vertex_dataflow(vertex_code(scalar_alu(opcode)),**OPTIONS)

    def test_operand_swizzle_absolute_then_negation(self):
        # Scalar A lane is ((swizzle>>6)+3)&3; choose each lane independently.
        values=(-2.,0.,3.,-4.)
        for lane in range(4):
            swizzle=((lane-3)&3)<<6
            for absolute,negate in ((False,False),(True,False),(False,True),(True,True)):
                value=abs(values[lane]) if absolute else values[lane]
                if negate:value=-value
                inst=scalar_alu(8,source=128 if absolute else 0,negate=negate,swizzle=swizzle)
                self.assertEqual(evaluate(inst,values)['62'],[float(value>0)]*4)

    def test_scalar_previous_updates_even_without_destination_mask(self):
        for value in (-1.,0.,2.):
            out=evaluate(scalar_alu(8,mask=0),(2,3,5,value),
                [alu(dest=62,vector=0,scalar=15),alu()],include_state=True)
            self.assertEqual(out['exports']['62'],[float(value>0)]*4)
            self.assertEqual(out['previous_scalar'],float(value>0));self.assertIsNone(out['predicate'])

    def test_coissue_reads_old_registers_and_preserves_unwritten_lanes(self):
        # Vector writes x/y, scalar writes z/w in the same old r0 snapshot.
        a,b,c=math_alu(op=0,dest=0,mask=3,sources=(0,0,0),temps=(True,True,True),export=False)
        a=(a&~(63<<26))|(8<<26)|(12<<20)
        out=evaluate((a,b,c),(2,3,5,7))['62']
        self.assertEqual(out,[4,6,1,1])

    def test_export_mask_literal_overlap_and_no_predicate_mutation(self):
        a,b,c=scalar_alu(8,mask=5);a|=(1<<15)|(4<<16)
        out=evaluate((a,b,c),(2,3,5,-7),[alu(dest=62)],include_state=True)
        # Overlap bit z is literal 1; only x receives SETGTs; y/w stay unwritten.
        self.assertEqual(out['exports']['0'],[0,None,1,None]);self.assertIsNone(out['predicate'])

    def test_unknown_relative_and_nonfinite_inputs_still_refuse(self):
        for value in (float('nan'),float('inf'),-float('inf'),2.**-149):
            with self.assertRaises(RawShaderError):evaluate(scalar_alu(8),(2,3,5,value))
        for source in (64,192,255):
            with self.assertRaises(RawShaderError):evaluate(scalar_alu(8,source=source),(2,3,5,7))

if __name__=='__main__':unittest.main()
