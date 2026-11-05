from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from edram_alias_clear import footprint,sample_offset,canonical_depth_bits,pack_canonical_depth,plan_alias
DEPTH=(0xA020280,0x2D0,640,360,True)
COLOR=(0x14000500,0xC02D0,1280,720,False)


class EdramAliasTests(unittest.TestCase):
    def test_exact_full_views_alias_and_half_rows_differ(self):
        p=plan_alias(DEPTH,COLOR,0x3F400001,0)
        self.assertEqual((p['begin'],p['bytes'],p['word']),(3686400,3686400,0xC0000000))
        self.assertEqual(p['color_rgba'],[0,0,0,1]);self.assertFalse(p['native_gpu_executed'])
        self.assertEqual(sample_offset(DEPTH,0,0,0),p['begin']+160)
        self.assertEqual(sample_offset(DEPTH,20,0,0),p['begin'])
        self.assertEqual(sample_offset(COLOR,0,0,0),p['begin'])
        self.assertEqual(sample_offset(DEPTH,0,0,1),p['begin']+480)
        self.assertEqual(sample_offset(DEPTH,0,0,2),p['begin']+164)

    def test_canonical_conversion_without_general_quantization(self):
        self.assertEqual(canonical_depth_bits(0xC00000),0x3F400001)
        for n in [0,1,2,0x7FFFFE,0x7FFFFF,0x800000,0x800001,0xFFFFFE,0xFFFFFF]+list(range(0,0x1000000,7919)):
            for stencil in (0,37,255):
                self.assertEqual(pack_canonical_depth(canonical_depth_bits(n),stencil),n*256+stencil)
        for bits in (1,0x3F000000,0x80000000,0x7F800000,0x7FC00000,0x3F800001):
            with self.assertRaises(ValueError):pack_canonical_depth(bits,0)
        with self.assertRaises(ValueError):canonical_depth_bits(0x1000000)
        with self.assertRaises(ValueError):pack_canonical_depth(0x3F400001,256)

    def test_wrap_partial_format_alias_and_index_guards(self):
        for view in ((0xA020281,*DEPTH[1:]),(0xA030280,*DEPTH[1:]),(DEPTH[0],0x102D0,*DEPTH[2:]),
            (DEPTH[0],2047,*DEPTH[2:]),(DEPTH[0],0xAD0,*DEPTH[2:]),(*DEPTH[:3],359,True)):
            with self.assertRaises(ValueError):footprint(*view)
        for x,y,s in ((640,0,0),(0,360,0),(0,0,4),(True,0,0)):
            with self.assertRaises(ValueError):sample_offset(DEPTH,x,y,s)
        for depth,color,bits,stencil in ((COLOR,DEPTH,0x3F400001,0),(DEPTH,COLOR,0,0),
            (DEPTH,COLOR,0x3F400001,1),(DEPTH,(COLOR[0],0xC02D1,*COLOR[2:]),0x3F400001,0)):
            with self.assertRaises(ValueError):plan_alias(depth,color,bits,stencil)


if __name__=='__main__':unittest.main()
