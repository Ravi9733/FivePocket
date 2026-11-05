from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from tiled32_texture import decode_descriptor,decode_pixels,texel_offset

DESCRIPTOR=(0x80400002,0x10000086,0x3E01F,0xA80C14,3,0x210)


class Tiled32Tests(unittest.TestCase):
    def test_bijection_and_known_addresses(self):
        offsets={texel_offset(x,y) for x in range(32) for y in range(32)}
        self.assertEqual(offsets,set(range(0,4096,4)))
        for x,y,expected in ((0,0,0),(1,0,4),(4,0,32),(8,0,64),(16,0,128),
                             (0,1,16),(0,2,256),(0,8,1152),(0,16,2048),(31,31,3964)):
            self.assertEqual(texel_offset(x,y),expected)
        for x,y in ((32,0),(0,32),(-1,0),(True,0),(0,0.5)):
            with self.assertRaises(ValueError):texel_offset(x,y)

    def test_big_endian_then_descriptor_swizzle(self):
        data=bytearray(4096)
        for y in range(32):
            for x in range(32):
                at=texel_offset(x,y);data[at:at+4]=bytes((255,x,y,x^y))
        pixels=decode_pixels(DESCRIPTOR,bytes(data))
        for y in range(32):
            for x in range(32):self.assertEqual(pixels[(y*32+x)*4:(y*32+x+1)*4],bytes((x,y,x^y,255)))

    def test_descriptor_profile_and_extent_guards(self):
        self.assertEqual(decode_descriptor(DESCRIPTOR)['swizzle'],[2,1,0,3])
        for word,bit in ((0,0),(0,2),(0,19),(0,22),(0,31),(1,6),(1,10),(1,29),
                         (2,0),(2,26),(3,0),(3,13),(3,19),(4,2),(5,11),(5,12)):
            changed=list(DESCRIPTOR);changed[word]^=1<<bit
            with self.assertRaises(ValueError):decode_descriptor(changed)
        for source in (bytes(4095),bytes(4097),bytearray(4096)):
            with self.assertRaises(ValueError):decode_pixels(DESCRIPTOR,source)

    def test_all_descriptor_swizzles(self):
        accepted=0
        for encoded in range(4096):
            words=list(DESCRIPTOR);words[3]=(words[3]&~0x1FFE)|(encoded<<1)
            if any((encoded>>(3*i))&7>5 for i in range(4)):
                with self.assertRaises(ValueError):decode_descriptor(words)
            else:decode_descriptor(words);accepted+=1
        self.assertEqual(accepted,1296)


if __name__=='__main__':unittest.main()
