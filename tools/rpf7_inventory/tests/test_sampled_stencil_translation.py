from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sampled_stencil_translation import translate_sampled_stencil
from inline_shader_translation import translate_inline_pair
from raw_shader_analysis import RawShaderError
from test_raw_shader_analysis import cf_alloc, cf_exec, program, alu
from test_raw_vertex_fetch_analysis import fetch
from test_pixel_sample_dataflow import texture


def pair(slot=1, swizzle=0xFFB, stride=9):
    vs = program([cf_exec(address=3, sequence=1, opcode=1), cf_alloc(2),
                  cf_alloc(1), cf_exec(address=4, count=2), 0, 0],
                 [fetch(dst=1, slot=95, fmt=38, stride=stride, swizzle=0x688),
                  alu(dest=0, source=1), alu(dest=62, source=1)])
    # Authored scalar MAX-copy from sampled r0.x to output alpha, zero RGB.
    scalar = ((5 << 26) | (1 << 15) | (1 << 14) | (8 << 20), 64, (2 << 24) | (1 << 29))
    ps = program([cf_exec(address=2, sequence=1, opcode=1), cf_alloc(),
                  cf_exec(address=3), 0], [texture(slot=slot, swizzle=swizzle), scalar])
    return vs, ps


class SampledStencilTranslationTests(unittest.TestCase):
    def test_separate_abi_keeps_original_arithmetic_and_ordered_kill(self):
        vs, ps = pair()
        old_v, old_p, old = translate_inline_pair(vs, ps, extended_profile=True, direct_triangles=True)
        new_v, new_p, new = translate_sampled_stencil(vs, ps)
        self.assertEqual(old_v, new_v)
        self.assertEqual(old['abi_version'], 5)
        self.assertEqual(new['abi_version'], 34)
        self.assertFalse(new['native_admission'])
        self.assertEqual(old['pixel'], new['pixel'])
        self.assertEqual(old_p[:-3] + '\n    if (!(color.a <= 0.0)) discard;\n}\n', new_p)
        self.assertNotIn('early_fragment_tests', new_p)

    def test_wrong_slot_channel_and_stride_refuse(self):
        for options in ({'slot': 0}, {'slot': 2}, {'swizzle': 0xFF8}, {'stride': 4}):
            with self.assertRaises(RawShaderError):
                translate_sampled_stencil(*pair(**options))


if __name__ == '__main__':
    unittest.main()
