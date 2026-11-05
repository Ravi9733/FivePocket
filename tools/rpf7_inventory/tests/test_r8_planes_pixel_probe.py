from pathlib import Path
import struct
import sys
import unittest
sys.path[:0]=[str(Path(__file__).resolve().parents[1]),str(Path(__file__).resolve().parents[2]/'vulkan_probe')]
from test_pixel_dot_scalar_dataflow import triple
from pixel_sample_dataflow import translate_r8_planes_pixel_compute,R8_PLANE_IMAGE_BYTES
from generate_r8_planes_pixel_probe import (binary_linear_reference,make_inputs,reference_bytes)


class R8PlanesPixelTests(unittest.TestCase):
    def test_three_separate_bindings_and_no_sample_overrides(self):
        source,proof=translate_r8_planes_pixel_compute(triple())
        self.assertEqual(proof['input_bytes'],R8_PLANE_IMAGE_BYTES+128)
        self.assertEqual(proof['initial_coordinate_components'],[[0,1]]*3)
        self.assertFalse(proof['sample_result_override'])
        for i in range(3):
            self.assertIn(f'binding={i+2}) uniform sampler2D imageSource{i}',source)
            self.assertIn(f'textureLod(imageSource{i},coordinate{i},0.0)',source)
        self.assertIn('index*8u',source)
        self.assertIn('index>=4u',source)

    def test_binary_repeat_linear_quarters_and_rejection(self):
        source=bytes((0,255,255,0))
        for u,v,expected in ((.25,.25,0),(.75,.25,1),(0,0,.5),(.375,.25,.25),
                             (1.375,-.75,.25),(.375,.375,.375)):
            self.assertEqual(binary_linear_reference(source,2,2,u,v),expected)
        for u,v in ((float('nan'),0),(5,0),(.3,.3)):
            with self.assertRaises(ValueError):binary_linear_reference(source,2,2,u,v)
        with self.assertRaises(ValueError):binary_linear_reference(bytes((0,1,2,3)),2,2,.25,.25)
        with self.assertRaises(ValueError):binary_linear_reference(source,2,2,0,0,require_uniform=True)

    def test_plane_slots_and_separate_zw_coordinates(self):
        code=bytearray(triple())
        word=struct.unpack_from('>I',code,36)[0]
        struct.pack_into('>I',code,36,(word&~(63<<26))|(14<<26))
        source,proof=translate_r8_planes_pixel_compute(bytes(code))
        self.assertEqual(proof['initial_coordinate_components'],[[0,1],[2,3],[0,1]])
        self.assertIn('index*8u+2u',source)
        struct.pack_into('>I',code,36,(word&~(63<<26))|(1<<26))
        with self.assertRaises(ValueError):translate_r8_planes_pixel_compute(bytes(code))
        code=bytearray(triple());word=struct.unpack_from('>I',code,24)[0]
        struct.pack_into('>I',code,24,word^(1<<20))
        with self.assertRaises(ValueError):translate_r8_planes_pixel_compute(bytes(code))

    def test_rgba_swizzle_and_atomic_per_invocation_guards(self):
        planes=[bytes(1280*720),bytes([255])*(640*360),bytes(640*360)]
        records=struct.pack('>8f',.25,.75,.5,.5,1,1,1,1)*4
        _,_,data,expected=make_inputs(triple(),planes,records,b'')
        for index in range(4):self.assertEqual(struct.unpack_from('<4f',expected,16+index*16),(1,1,1,3))
        short,rejected=reference_bytes(triple(),data[:-4])
        self.assertEqual((short,rejected),(bytes([0x5A])*96,list(range(4))))
        bad=bytearray(data);struct.pack_into('>I',bad,R8_PLANE_IMAGE_BYTES,0x7FC00000)
        guarded,rejected=reference_bytes(triple(),bytes(bad))
        self.assertEqual(rejected,[0]);self.assertEqual(guarded[16:32],bytes([0x5A])*16)
        self.assertEqual(guarded[32:],expected[32:])
        with self.assertRaises(ValueError):make_inputs(triple(),planes,records[:-4],b'')
        with self.assertRaises(ValueError):make_inputs(triple(),planes[:-1],records,b'')


if __name__=='__main__':unittest.main()
