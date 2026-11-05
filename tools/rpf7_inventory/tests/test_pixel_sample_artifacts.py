from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'vulkan_probe'))
from generate_pixel_sample_probe import make_inputs,load_pixel_shader
from test_pixel_sample_dataflow import pixel


class PixelSampleArtifactTests(unittest.TestCase):
    def test_four_records_and_canaries(self):
        records=b''.join(struct.pack('>12f',0,0,0,0,2,3,4,5,6+i,7+i,8+i,9+i) for i in range(4))
        source,proof,combined,expected=make_inputs(pixel(),records,b'')
        self.assertEqual(combined,records);self.assertEqual(len(expected),96)
        self.assertEqual(expected[:16],bytes([0x5A])*16);self.assertEqual(expected[-16:],bytes([0x5A])*16)
        for i in range(4):self.assertEqual(struct.unpack_from('<4f',expected,16+i*16),
            ((6+i)*2,(7+i)*3,(8+i)*4,(9+i)*5))

    def test_atomic_input_guards(self):
        for records,constants in ((bytes(188),b''),(bytes(196),b''),(bytes(192),bytes(16)),
            (bytes(188)+struct.pack('>f',float('nan')),b'')):
            with self.assertRaises(ValueError):make_inputs(pixel(),records,constants)
        for index in (-1,8,True):
            with self.assertRaises(ValueError):load_pixel_shader(Path('/unused'),index)


if __name__=='__main__':unittest.main()
