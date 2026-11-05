from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'vulkan_probe'))
from generate_texture32_sample_fixture import expected_samples


class Texture32SampleFixtureTests(unittest.TestCase):
    def test_center_periodic_bytes_and_canaries(self):
        source=bytes(i%256 for i in range(4096))
        expected=expected_samples(source,'center')
        self.assertEqual(expected,expected_samples(source,'periodic'))
        self.assertEqual(len(expected),16416)
        self.assertEqual(expected[:16],bytes([0x5A])*16);self.assertEqual(expected[-16:],bytes([0x5A])*16)
        for i in range(1024):self.assertEqual(struct.unpack_from('<4I',expected,16+i*16),tuple(source[i*4:i*4+4]))

    def test_quarter_repeat_edges_and_all_lanes(self):
        source=bytes(v for y in range(32) for x in range(32) for v in (x*4,y*4,(x^y)*4,252))
        expected=expected_samples(source,'quarter')
        self.assertEqual(struct.unpack_from('<4I',expected,16+1023*16),(62,62,62,252))
        self.assertEqual(struct.unpack_from('<4I',expected,16),(2,2,2,252))

    def test_profile_bounds_and_fractional_reference_reject(self):
        for data,mode in ((bytes(4095),'center'),(bytes(4097),'center'),(bytearray(4096),'center'),(bytes(4096),'unknown')):
            with self.assertRaises(ValueError):expected_samples(data,mode)
        data=bytearray(4096);data[0]=1
        with self.assertRaises(ValueError):expected_samples(bytes(data),'quarter')


if __name__=='__main__':unittest.main()
