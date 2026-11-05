from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from unorm8_vertex_fetch import decode_unorm8_fetch, read_unorm8_fetch, f32
from raw_shader_analysis import _float_vertex_fetch, RawShaderError
from test_raw_vertex_fetch_analysis import fetch


def packed(**kwargs):
    a, b, c = fetch(fmt=6, swizzle=kwargs.pop('swizzle', 0x688), **kwargs)
    return a, b & ~0x3000, c


class Unorm8VertexFetchTests(unittest.TestCase):
    def test_all_values_and_channel_order(self):
        plan, _ = decode_unorm8_fetch(packed(stride=1))
        for lane in range(4):
            for value in range(256):
                data = (value << (lane*8)).to_bytes(4, 'big')
                self.assertEqual(read_unorm8_fetch(plan,data,0),
                    [f32(value*f32(1/255)) if i==lane else 0.0 for i in range(4)])
        reverse, _ = decode_unorm8_fetch(packed(stride=1,swizzle=0x60A))
        self.assertEqual(read_unorm8_fetch(reverse,bytes.fromhex('ff204080'),0),
                         [f32(x*f32(1/255)) for x in (32,64,128,255)])

    def test_mini_inherits_full_float_address_and_saved_index(self):
        regs = [[f'initial_r{r}.{c}' for c in 'xyzw'] for r in range(64)]
        full, previous = _float_vertex_fetch(fetch(slot=95,stride=9),None,regs,0)
        regs[0][0] = 'changed-after-full'
        plan, saved = decode_unorm8_fetch(packed(src=63,slot=0,stride=0,offset=6,mini=True),previous,regs,1)
        self.assertEqual(plan['fetch_slot'],95);self.assertEqual(plan['stride_words'],9)
        self.assertEqual(plan['index_expression'],'initial_r0.x');self.assertEqual(saved,previous)
        self.assertEqual(read_unorm8_fetch(plan,bytes(24)+bytes.fromhex('ff000000'),0),[0,0,0,1])
        with self.assertRaises(RawShaderError):decode_unorm8_fetch(packed(mini=True))

    def test_swizzles_and_unknown_keep(self):
        for swizzle in range(4096):
            words = packed(stride=1,swizzle=swizzle)
            if any((swizzle>>(i*3))&7==6 for i in range(4)):
                with self.assertRaises(RawShaderError):decode_unorm8_fetch(words)
                continue
            plan,_ = decode_unorm8_fetch(words)
            result = read_unorm8_fetch(plan,bytes.fromhex('ff000000'),0)
            expected = [[0.,0.,0.,1.,0.,1.,None,None][(swizzle>>(i*3))&7] for i in range(4)]
            self.assertEqual(result,expected)

    def test_modifiers_bounds_and_kept_nonfinite(self):
        base = packed(stride=1)
        for w,bit in ((0,0),(0,18),(1,12),(1,13),(1,14),(1,15),(1,22),(1,24),(1,31),(2,30),(2,31)):
            changed=list(base);changed[w]^=1<<bit
            with self.assertRaises(RawShaderError):decode_unorm8_fetch(changed)
        plan,_ = decode_unorm8_fetch(base)
        for source,index in ((b'',0),(b'a',0),(bytes(4100),0),(bytes(4),1),(bytes(4),1<<24),(bytes(4),True)):
            with self.assertRaises(RawShaderError):read_unorm8_fetch(plan,source,index)
        plan,_ = decode_unorm8_fetch(packed(stride=1,swizzle=0xFFF))
        with self.assertRaises(RawShaderError):read_unorm8_fetch(plan,bytes(4),0,[0,float('inf'),0,0])


if __name__ == '__main__': unittest.main()
