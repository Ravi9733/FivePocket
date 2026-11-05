from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from half_color_load import unpack_half,load_reference,INPUT_BYTES,OUTPUT_BYTES
from half_color_store import packed_half
from color_7e3 import decode


class HalfColorLoadTests(unittest.TestCase):
    def test_all_codes_and_alpha_roundtrip(self):
        for code in range(1024):
            for alpha in range(4):
                packed=code|((1023-code)<<10)|(((code*37)%1024)<<20)|(alpha<<30)
                result=unpack_half(packed);rgba=struct.unpack('<4e',result)
                self.assertEqual(rgba[:3],tuple(decode(v) for v in (code,1023-code,(code*37)%1024)))
                self.assertEqual(struct.unpack('<4H',result)[3],(0,0x3555,0x3955,0x3C00)[alpha])
                self.assertEqual(packed_half(struct.unpack('<4H',result)),packed)
        for invalid in (-1,2**32,True):
            with self.assertRaises(ValueError):unpack_half(invalid)

    def test_full_black_surface_and_guards(self):
        source=bytearray([0x5A])*INPUT_BYTES;begin=16+3686400
        source[begin:begin+3686400]=struct.pack('<I',0xC0000000)*921600
        source=bytes(source);out,proof=load_reference(source)
        self.assertEqual(len(out),OUTPUT_BYTES);self.assertEqual(proof['decoded_pixels'],921600)
        self.assertEqual(out[16:-16],struct.pack('<4e',0,0,0,1)*921600)
        self.assertEqual(out[:16]+out[-16:],bytes([0x5A])*32)
        for invalid in (source[:-4],b'\0'+source[1:],source[:-1]+b'\0',bytearray(source)):
            with self.assertRaises(ValueError):load_reference(invalid)


if __name__=='__main__':unittest.main()
