"""SGE producer -> masked exports/evaluator/GLSL; no native draw admission."""
import math
import struct
import unittest
from test_vertex_predication import OPTIONS, TRANSLATE, pixel, guarded, setp
from test_extended_shader_math import vertex_code
from test_vertex_arithmetic_dataflow import math_alu
from test_raw_shader_analysis import alu
from raw_shader_analysis import RawShaderError
from vertex_arithmetic_dataflow import analyze_vertex_dataflow, evaluate_vertex_dataflow
from inline_shader_translation import translate_inline_pair


class VertexSgeTests(unittest.TestCase):
    def evaluate(self, code, source, constant, **options):
        return evaluate_vertex_dataflow(code, {95:struct.pack('>4f',*source)},
            {0:struct.pack('>4f',*constant)}, 0, **{**OPTIONS,**options})

    def test_equality_order_signed_zero_and_partial_export_masks(self):
        source=(-2.,-0.,3.,-4.)
        constant=(-3.,0.,3.,-2.)
        for mask in range(1,16):
            code=vertex_code(math_alu(op=6,dest=62,mask=mask),tail=[alu()])
            self.assertEqual(self.evaluate(code,source,constant)['62'],
                [v if mask&(1<<i) else None for i,v in enumerate([1.,1.,1.,0.])])

    def test_swizzles_read_preinstruction_registers_before_overwrite(self):
        # src1.yzwx compared with src0.xyzw. r0 is also the destination.
        code=vertex_code(math_alu(op=6,dest=0,mask=15,sources=(0,0,0),
            temps=(True,True,True),swizzles=(0,85,0),export=False))
        self.assertEqual(self.evaluate(code,(1.,3.,2.,4.),(0.,)*4)['62'],[0.,1.,0.,1.])

    def test_predicate_preserves_old_values_and_comparison_reaches_glsl(self):
        ins=math_alu(op=6,dest=0,mask=7,export=False)
        code=vertex_code(setp(),[guarded(ins),alu(dest=62),alu()])
        for w,expected in ((0.,[2.,3.,5.,0.]),(1.,[1.,0.,1.,1.])):
            self.assertEqual(self.evaluate(code,(2.,3.,5.,w),(2.,4.,1.,0.))['62'],expected)
        vs,_,proof=translate_inline_pair(code,pixel(),**TRANSLATE,geometry_mrt_formats=(0,None,None,None))
        self.assertIn('>=',vs)
        self.assertIn('if (',vs)
        self.assertTrue(any(n[0]=='greater_equal' for n in proof['vertex']['nodes']))
        self.assertFalse(proof['geometry_resources_verified'])

    def test_old_profiles_modifiers_and_numeric_domain_stay_strict(self):
        code=vertex_code(math_alu(op=6,dest=0,mask=15,export=False))
        for options in ({},{**OPTIONS,'packed_geometry':False},{**OPTIONS,'loading_math':False}):
            with self.assertRaises(RawShaderError):analyze_vertex_dataflow(code,**options)
        for bad in (math.inf,math.nan,2.**-149):
            with self.assertRaises(RawShaderError):self.evaluate(code,(bad,0.,0.,0.),(0.,)*4)
        invalid=vertex_code(math_alu(op=6,sources=(64,0,0)))
        with self.assertRaises(RawShaderError):analyze_vertex_dataflow(invalid,**OPTIONS)


if __name__=='__main__':unittest.main()
