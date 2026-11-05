from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from half_color_store import packed_half,store_reference,INPUT_BYTES,OUTPUT_BYTES
from color_7e3 import bits,decode,pack


class HalfColorStoreTests(unittest.TestCase):
    def test_validation_is_not_bypassed_by_cached_equal_values(self):
        self.assertEqual(packed_half((0,0,0,0)),0)
        for invalid in ((False,0,0,0),(0.,0,0,0),[0,0,0,0],(0,0,0),(-1,0,0,0)):
            with self.assertRaises(ValueError):packed_half(invalid)

    def test_all_half_words_in_every_lane(self):
        for lane in range(4):
            valid=0
            for h in range(65536):
                rgba=[0,0x3C00,0,0x3C00];rgba[lane]=h
                result=packed_half(tuple(rgba))
                rejected=(h&0x7C00)==0x7C00 or ((h&0x7C00)==0 and h&1023)!=0
                self.assertEqual(result is None,rejected)
                if not rejected:
                    want=pack(tuple(bits(v) for v in struct.unpack('<4e',struct.pack('<4H',*rgba))))
                    self.assertEqual(result,want);valid+=1
            self.assertEqual(valid,61442)

    def test_all_7e3_codes_survive_half_storage(self):
        for code in range(1024):
            h=struct.unpack('<H',struct.pack('<e',decode(code)))[0]
            self.assertEqual(packed_half((h,h,h,0x3C00)),code|(code<<10)|(code<<20)|0xC0000000)

    def test_full_surface_and_failed_source_guards(self):
        guard=bytes([0x5A])*16
        source=guard+struct.pack('<4H',0,0,0,0x3C00)*(1280*720)+guard
        out,proof=store_reference(source)
        self.assertEqual(len(out),OUTPUT_BYTES);self.assertEqual(proof['written_pixels'],921600)
        begin=16+3686400;end=begin+3686400
        self.assertEqual(out[begin:end],struct.pack('<I',0xC0000000)*921600)
        self.assertEqual(out[:begin]+out[end:],bytes([0x5A])*(OUTPUT_BYTES-3686400))
        self.assertEqual(len(source),INPUT_BYTES)
        for invalid in (source[:-4],b'\0'+source[1:],source[:-1]+b'\0',bytearray(source)):
            with self.assertRaises(ValueError):store_reference(invalid)


if __name__=='__main__':unittest.main()
