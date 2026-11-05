import math
import unittest
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from geometry_ieee_math import evaluate_geometry_ieee as evaluate
from loading_shader_math import evaluate_loading_operation
from raw_shader_analysis import RawShaderError
from test_loading_shader_math import scalar_alu, vertex_code, VOPTIONS
from vertex_arithmetic_dataflow import evaluate_vertex_dataflow
import struct


class GeometryIeeeTest(unittest.TestCase):
    def test_exp_overflow_is_not_finite_input_rejection(self):
        self.assertEqual(evaluate('exp2',[128.0]),math.inf)
        self.assertEqual(evaluate('exp2',[441.8893737792969]),math.inf)
        self.assertTrue(math.isfinite(evaluate('exp2',[127.0])))
        self.assertEqual(evaluate('exp2',[-math.inf]),0)
        self.assertEqual(evaluate('exp2',[math.inf]),math.inf)
        with self.assertRaises(RawShaderError):evaluate_loading_operation('exp2',[441.8893737792969])

    def test_actual_overflow_consumer_operations(self):
        x=evaluate('exp2',[441.8893737792969])
        x=evaluate('add',[evaluate('negate',[x]),1.0])
        self.assertEqual(x,-math.inf)
        x=evaluate('mul',[x,-2.0]);self.assertEqual(x,math.inf)
        self.assertEqual(evaluate('select_gt0',[0.0,x,1.0]),1.0)
        self.assertEqual(evaluate('mul',[0.0,x]),0.0)
        self.assertEqual(evaluate('maximum',[-math.inf,0.0]),0.0)

    def test_nan_subnormal_and_unproved_operations_reject(self):
        for op,args in [('add',[math.inf,-math.inf]),('mul',[math.nan,0.0]),
                        ('exp2',[-140.0]),('add',[2**-140,1.0]),('sqrt',[-1.0]),
                        ('log2',[0.0]),('exp2',[]),('add',[1.1,0.0])]:
            with self.assertRaises(RawShaderError):evaluate(op,args)

    def test_clamped_export_accepts_infinity_but_unclamped_rejects(self):
        vertices={95:struct.pack('>4f',2,3,5,441.8893737792969)}
        code=vertex_code(scalar_alu(14,clamp=True))
        with self.assertRaises(RawShaderError):evaluate_vertex_dataflow(code,vertices,{},0,**VOPTIONS)
        actual=evaluate_vertex_dataflow(code,vertices,{},0,packed_geometry=True,geometry_ieee=True,**VOPTIONS)
        self.assertEqual(actual['62'],[1.0]*4)
        with self.assertRaises(RawShaderError):
            evaluate_vertex_dataflow(vertex_code(scalar_alu(14)),vertices,{},0,packed_geometry=True,geometry_ieee=True,**VOPTIONS)
        with self.assertRaises(RawShaderError):
            evaluate_vertex_dataflow(vertex_code(scalar_alu(14)),vertices,{},0,position_only=True,packed_geometry=True,geometry_ieee=True,**VOPTIONS)
        with self.assertRaises(RawShaderError):evaluate_vertex_dataflow(code,vertices,{},0,geometry_ieee=True,**VOPTIONS)
