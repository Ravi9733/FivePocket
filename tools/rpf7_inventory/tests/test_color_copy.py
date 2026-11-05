from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from color_copy import plan_copy,destination_offset,convert_word,copy_to_buffer
from edram_alias_clear import sample_offset,EDRAM_BYTES
SOURCE=(0x14000500,0xC02D0,1280,720,False)


class ColorCopyTests(unittest.TestCase):
    def test_captured_extent_and_partial_macro_tile(self):
        plan=plan_copy(SOURCE,0x100000,0x1F2D8000,0x2D00500,0x1000300)
        self.assertEqual((plan['capacity'],plan['touched_span'],plan['pixel_bytes']),(3768320,3766272,3686400))
        self.assertEqual(plan['padding_bytes_unwritten'],81920);self.assertFalse(plan['destination_ownership_verified'])
        self.assertEqual(destination_offset(1280,720,32,0),4096)
        self.assertEqual(destination_offset(1280,720,0,32),40*4096)
        for x,y in ((1280,0),(0,720),(True,0)):
            with self.assertRaises(ValueError):destination_offset(1280,720,x,y)

    def test_every_code_saturating_conversion_and_channel_order(self):
        for code in range(1024):
            e,m=divmod(code,128);numerator=m if e==0 else 128+m;denom=256 if e==2 else 512
            expected=255 if code>=384 else (numerator*255+denom//2)//denom
            for alpha in range(4):
                packed=code|(alpha<<30)
                self.assertEqual(convert_word(packed,False),expected|(alpha*85<<24))
                self.assertEqual(convert_word(packed,True),(expected<<16)|(alpha*85<<24))
        self.assertEqual(convert_word(0xC0000000,True),0xFF000000)

    def test_small_multitile_copy_holes_source_immutable(self):
        source=(160,0xC0000,160,16,False)
        edram=bytearray([0x5A])*EDRAM_BYTES
        for y in range(16):
            for x in range(160):
                word=(x%1024)|((y%4)<<30)
                struct.pack_into('<I',edram,sample_offset(source,x,y,0),word)
        initial=bytes(edram)
        result,plan=copy_to_buffer(source,0x100000,0,0x1000A0,0x1000300,initial)
        self.assertEqual(bytes(edram),initial);visited=set()
        for y in range(16):
            for x in range(160):
                at=destination_offset(160,16,x,y);visited.add(at)
                self.assertEqual(struct.unpack_from('<I',result,at)[0],convert_word(x|((y%4)<<30),True))
        for at in range(0,len(result),4):
            if at not in visited:self.assertEqual(result[at:at+4],bytes([0x5A])*4)
        self.assertEqual(plan['capacity'],20480)

    def test_unsupported_copy_fields_reject(self):
        for control,base,pitch,info in ((0x100001,0,0x2D00500,0x1000300),(0x100000,4,0x2D00500,0x1000300),
            (0x100000,0x1FFFF000,0x2D00500,0x1000300),(0x100000,0,0x2D00520,0x1000300),
            *((0x100000,0,0x2D00500,x) for x in (0x1000301,0x1000308,0x1000310,0x1010300,0x2000300))):
            with self.assertRaises(ValueError):plan_copy(SOURCE,control,base,pitch,info)


if __name__=='__main__':unittest.main()
