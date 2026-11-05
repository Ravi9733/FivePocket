"""Observed EXP subnormal -> MUL; all other numeric domains stay strict."""
import math,struct,sys,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from raw_shader_analysis import RawShaderError
from world_strip_shader_policy import world_strip_exp2
from pixel_sample_dataflow import evaluate_analyzed_pixel_dataflow
from inline_shader_translation import translate_inline_pair
from test_world_strip_translation import vertex,pixel,OPTIONS


class WorldStripNumericTests(unittest.TestCase):
    def proof(self,op='mul',final=3):
        return dict(world_strip_profile=True,predicate_profile=False,nodes=[('constant',0,0),('exp2',0),('literal',2), (op,1,2)],
            fetches=[],exports={'0':[final]*4},kills=[],predicate=None,previous_scalar=None)

    def test_binary32_exponent_domain(self):
        self.assertEqual(world_strip_exp2(-143.08145141601562),8.407790785948902e-44)
        for x,result in ((-150.,0.),(-149.,2**-149),(-126.,2**-126),(0.,1.)):
            self.assertEqual(world_strip_exp2(x),result)
        for x in (math.inf,math.nan,128.,.1):
            with self.assertRaises(RawShaderError):world_strip_exp2(x)

    def test_connected_underflow_consumed_as_positive_zero(self):
        args=(self.proof(),{}, {0:struct.pack('>4f',-143.08145141601562,0,0,0)}, {})
        with self.assertRaises(RawShaderError):evaluate_analyzed_pixel_dataflow(*args)
        result=evaluate_analyzed_pixel_dataflow(*args,world_strip_underflow=True)['0']
        self.assertEqual(result,[0.]*4)
        self.assertTrue(all(math.copysign(1,x)==1 for x in result))

    def test_profile_isolation_add_and_final_subnormal_refuse(self):
        for proof in (self.proof('add'),self.proof(final=1),{**self.proof(),'world_strip_profile':False}):
            with self.assertRaises(RawShaderError):evaluate_analyzed_pixel_dataflow(proof,{},
                {0:struct.pack('>4f',-143.,0,0,0)},{},world_strip_underflow=True)
        with self.assertRaises(RawShaderError):evaluate_analyzed_pixel_dataflow(self.proof(),{},{},{},world_strip_underflow=1)

    def test_glsl_uses_exponent_bits_and_metadata_is_explicit(self):
        # The authored pixel helper copies a sample, but vertex instructions
        # contain MUL. Ensure VS is unchanged and the explicit tag is available
        # to the connected reference. A directly authored pixel MUL exercises
        # the emission below using an existing geometry test instruction helper.
        from test_vertex_arithmetic_dataflow import math_alu
        from test_pixel_sample_dataflow import texture
        from test_raw_shader_analysis import program,cf_exec,cf_alloc
        ps=program([cf_exec(address=3,count=5,sequence=0x155,opcode=1),cf_alloc(),cf_exec(address=8),0,0,0],
            [*[texture(src=0,dst=4+i,slot=s) for i,s in enumerate((2,6,1,1,3))],
             math_alu(op=1,dest=0,sources=(4,8,0),temps=(True,True,False))])
        v,f,p=translate_inline_pair(vertex(),ps,**OPTIONS)
        self.assertTrue(p['pixel']['world_strip_profile'])
        self.assertIn('&0x7f800000u)==0u',f);self.assertNotIn('&0x7f800000u)==0u',v)


if __name__=='__main__':unittest.main()
