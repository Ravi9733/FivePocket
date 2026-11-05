from pathlib import Path
import math,struct,sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from cube_shader_math import CUBE_OPERATIONS,cube_result_nodes,evaluate_cube_operation
from pixel_sample_dataflow import texture_fetch_metadata,analyze_pixel_sample_dataflow,evaluate_pixel_sample_dataflow
from raw_shader_analysis import RawShaderError
from test_raw_shader_analysis import cf_alloc,cf_exec,program,alu
from test_vertex_arithmetic_dataflow import math_alu

class CubeShaderMathTests(unittest.TestCase):
    def test_axes_and_tie_order(self):
        for xyz,expected in [((4,2,1),(-2,-1,8,0)),((-4,2,1),(-2,1,-8,1)),
                ((1,4,2),(2,1,8,2)),((1,-4,2),(-2,1,-8,3)),
                ((1,2,4),(-2,1,8,4)),((1,2,-4),(-2,-1,-8,5)),
                ((2,2,1),(1,2,4,2)),((2,1,2),(-1,2,4,4)),((1,2,-2),(-2,-1,-4,5)),
                ((1,1,1),(-1,1,2,4)),((0,0,0),(0,0,0,4))]:
            self.assertEqual(tuple(evaluate_cube_operation(op,xyz) for op in CUBE_OPERATIONS),expected)
        negative_zero=tuple(evaluate_cube_operation(op,(0.,0.,-0.)) for op in CUBE_OPERATIONS)
        self.assertEqual(struct.pack('>f',negative_zero[2]),bytes.fromhex('80000000'))
        self.assertEqual(negative_zero[3],4)

    def test_bad_operands_and_equivalent_swizzles(self):
        self.assertEqual(cube_result_nodes([[3,3,1,2],[2,1,3,3]],lambda *a:a),
            [(op,1,2,3) for op in CUBE_OPERATIONS])
        for a,b in [([3,4,1,2],[2,1,3,3]),([3,3,1,2],[2,1,4,3]),([3,3,1],[2,1,3,3])]:
            with self.assertRaises(RawShaderError):cube_result_nodes([a,b],lambda *x:x)
        for v in (math.nan,math.inf,2**-149,0.1,1e100):
            with self.assertRaises(RawShaderError):evaluate_cube_operation('cube_sc',(v,1,2))
        with self.assertRaises(RawShaderError):evaluate_cube_operation('cube_ma',(2**127,1,2))

    def test_cube_fetch_is_explicit_and_preserves_three_coordinates(self):
        registers=[[r*4+c for c in range(4)] for r in range(64)]
        fetch=(1|(1<<19)|(4<<20)|(1<<5)|(0<<26)|(1<<28)|(2<<30),0x0F1FF688,0xC000)
        with self.assertRaises(RawShaderError):texture_fetch_metadata(fetch,registers,8)
        p=texture_fetch_metadata(fetch,registers,8,cube_profile=True)
        self.assertEqual(p['coordinates'],[4,5,6]);self.assertEqual(p['dimension'],'cube')
        self.assertFalse(p['computed_lod_requested']);self.assertFalse(p['sampling_executed'])
        for word,bit in ((0,11),(0,18),(0,19),(0,25),(1,28),(1,29),(1,31),(2,0),(2,16)):
            bad=list(fetch);bad[word]^=1<<bit
            with self.assertRaises(RawShaderError):texture_fetch_metadata(bad,registers,8,cube_profile=True)

    def test_original_operand_order_and_profile_gate(self):
        cube=list(math_alu(op=18,dest=0,sources=(1,1,0),temps=(True,True,True),export=False))
        # Relative swizzle encoding for zzxy/yxzz, independently authored.
        cube[1]=(0xA6<<16)|(0xCD<<8)
        code=program([cf_exec(address=2,count=1,opcode=1),cf_alloc(),cf_exec(address=3,count=1),0],[cube,alu()])
        with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(code,ui_operations=True,extended_math=True,loading_math=True)
        result=evaluate_pixel_sample_dataflow(code,{1:[4.,2.,1.,0.]},{},{},ui_operations=True,extended_math=True,loading_math=True,cube_profile=True)
        self.assertEqual(result['0'],[-2.,-1.,8.,0.])
        cube[1]^=1<<8
        bad=program([cf_exec(address=2,count=1,opcode=1),cf_alloc(),cf_exec(address=3,count=1),0],[cube,alu()])
        with self.assertRaises(RawShaderError):analyze_pixel_sample_dataflow(bad,ui_operations=True,extended_math=True,loading_math=True,cube_profile=True)

if __name__=='__main__':unittest.main()
