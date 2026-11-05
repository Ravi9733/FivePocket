from pathlib import Path
import math
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from geometry_pixel_parameters import geometry_parameter_register,geometry_parameter_xy
from inline_shader_translation import translate_inline_pair
from pixel_sample_dataflow import evaluate_pixel_sample_dataflow
from raw_shader_analysis import RawShaderError
from test_geometry_pixel_discard import OPTIONS,TRANSLATE,vertex,shader,alu
from test_vertex_arithmetic_dataflow import math_alu

class GeometryPixelParameterTests(unittest.TestCase):
    def test_actual_register_selection_and_disabled_state(self):
        self.assertEqual(geometry_parameter_register(0x40450706,0x50A),5)
        self.assertIsNone(geometry_parameter_register(0x40410706,8))
        for pos in range(16):
            self.assertEqual(geometry_parameter_register((1<<18)|(15<<8),(pos<<8)|10),pos)
        for program,misc in ((0x404D0706,0x50A),(0x40450706,0x100A),(0x40450706,0x8001000A),(-1,8),(True,8)):
            with self.assertRaises(RawShaderError):geometry_parameter_register(program,misc)

    def test_pixel_address_face_sign_and_unknown_zw(self):
        for facing in (True,False):
            for x,y in ((.5,.5),(17.75,383.5),(1279.5,719.5),(-.5,-1.25)):
                result=geometry_parameter_xy(x,y,facing)
                self.assertEqual(abs(result[0]),abs(math.floor(x)))
                self.assertEqual(result[1],abs(math.floor(y)))
                self.assertEqual(math.copysign(1,result[0]),1 if facing else -1)
                self.assertEqual(result[2:],[None,None])
        self.assertEqual(struct.pack('>f',geometry_parameter_xy(.5,.5,False)[0]),b'\x80\0\0\0')
        for args in ((math.nan,0,True),(math.inf,0,True),(2**24,0,True),(.1,0,True),(0,0,1)):
            with self.assertRaises(RawShaderError):geometry_parameter_xy(*args)

    def test_generated_input_reaches_cpu_and_native_exports_without_filling_holes(self):
        code=shader([alu(source=5,vector=3,zero=True)])
        values=evaluate_pixel_sample_dataflow(code,{5:geometry_parameter_xy(17.5,63.5,False)},{},{},**OPTIONS)
        self.assertEqual(values['0'],[-17.,63.,0.,0.])
        _,ps,p=translate_inline_pair(vertex(),code,**TRANSLATE,geometry_mrt_formats=(0,0,0,0),
            geometry_predicate_profile=True,geometry_parameter_register=5)
        self.assertEqual(p['geometry_parameter_register'],5)
        self.assertIn('abs(floor(gl_FragCoord.xy))',ps);self.assertIn('gl_FrontFacing?0u:0x80000000u',ps)
        self.assertIn('generatedPixelXY[0]',ps);self.assertNotIn('packedInterpolator5',ps)
        with self.assertRaisesRegex(RawShaderError,'unwritten geometry'):
            translate_inline_pair(vertex(),code,**TRANSLATE,geometry_mrt_formats=(0,0,0,0),geometry_predicate_profile=True)
        for source,lane in ((5,4),(5,8),(6,1)):
            with self.assertRaises(RawShaderError):translate_inline_pair(vertex(),shader([alu(source=source,vector=lane,zero=True)]),
                **TRANSLATE,geometry_mrt_formats=(0,0,0,0),geometry_predicate_profile=True,geometry_parameter_register=5)
        for index in (-1,16,True,'5'):
            with self.assertRaises(RawShaderError):translate_inline_pair(vertex(),code,**TRANSLATE,
                geometry_mrt_formats=(0,0,0,0),geometry_predicate_profile=True,geometry_parameter_register=index)

    def test_masked_off_generated_components_are_not_evaluated_by_modifiers(self):
        # Only XY of abs(r5) is consumed; unknown ZW must remain untouched.
        inst=math_alu(op=1,dest=0,mask=3,sources=(133,0,0),temps=(True,False,True),export=False)
        code=shader([inst,alu(source=0,vector=3,zero=True)])
        result=evaluate_pixel_sample_dataflow(code,{5:geometry_parameter_xy(17.5,63.5,False)},
            {0:struct.pack('>4f',.5,.5,0,0)},{},**OPTIONS)
        self.assertEqual(result['0'],[8.5,31.5,0.,0.])
        translate_inline_pair(vertex(),code,**TRANSLATE,geometry_mrt_formats=(0,0,0,0),
            geometry_predicate_profile=True,geometry_parameter_register=5)

if __name__=='__main__':unittest.main()
