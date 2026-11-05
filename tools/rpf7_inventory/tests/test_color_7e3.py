from pathlib import Path
import struct
import sys
import unittest
sys.path[:0]=[str(Path(__file__).resolve().parents[1]),str(Path(__file__).resolve().parents[2]/'vulkan_probe')]
from color_7e3 import bits,value,decode,encode,source_candidate,pack,unpack
from generate_color_7e3_probe import make_reference


class Color7e3Tests(unittest.TestCase):
    def test_all_codes_and_rgba_roundtrips(self):
        self.assertEqual([decode(i) for i in (0,1,127,128,384,385,1023)],
                         [0,1/512,127/512,.25,1,1+1/128,31.875])
        for code in range(1024):
            self.assertEqual(encode(bits(decode(code))),code)
            for a in range(4):
                packed=code|((1023-code)<<10)|(((code*37)%1024)<<20)|(a<<30)
                self.assertEqual(pack(unpack(packed)),packed)
        self.assertEqual(unpack(0xC0000000),[0,0,0,bits(1)])

    def test_midpoints_neighbors_and_retained_sticky_disagreements(self):
        rejected=0
        for code in range(1023):
            mid=bits((decode(code)+decode(code+1))/2)
            for direction in (-1,0,1):
                expected=code+(1 if direction>0 else code%2 if direction==0 else 0)
                try:self.assertEqual(encode(mid+direction),expected)
                except ValueError:
                    self.assertTrue(code<128 and code%2==0 and direction==1);rejected+=1
        self.assertEqual(rejected,64)
        # The first ambiguous interval has255 float32 inputs, not merely one.
        mid=bits(1/1024)
        for delta in range(1,256):
            self.assertEqual(source_candidate(mid+delta),0)
            with self.assertRaisesRegex(ValueError,'sticky'):encode(mid+delta)
        self.assertEqual(encode(mid+256),1)

    def test_explicit_finite_clamp_alpha_policy_and_atomic_output_records(self):
        for raw in (bits(-0.),bits(-1.),0xFF7FFFFF):self.assertEqual(encode(raw),0)
        for raw in (bits(32.),0x7F7FFFFF):self.assertEqual(encode(raw),1023)
        self.assertEqual(pack([0,0,0,bits(.5)])>>30,2) # Not DirectXTex's alpha truncation.
        for bad in (0x7F800000,0xFF800000,0x7FC00000,0xFFC00000):
            for lane in range(4):
                rgba=[0,0,0,bits(1)];rgba[lane]=bad
                with self.assertRaises(ValueError):pack(rgba)
                expected,proof=make_reference(struct.pack('<4I',*rgba))
                self.assertEqual(proof['accepted_records'],0);self.assertEqual(expected,bytes([0x5A])*8224)
        data=struct.pack('<4f',0,0,0,1);expected,proof=make_reference(data)
        self.assertEqual(expected[16:36],struct.pack('<5I',0xC0000000,0,0,0,bits(1)))
        self.assertEqual(expected[36:],bytes([0x5A])*(8224-36))
        self.assertFalse(proof['retail_rop_verified'])
        for invalid in (b'',bytes(15),bytes(4112)):
            with self.assertRaises(ValueError):make_reference(invalid)


if __name__=='__main__':unittest.main()
