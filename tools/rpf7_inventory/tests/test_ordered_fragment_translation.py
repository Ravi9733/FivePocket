import unittest
from unittest.mock import patch
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from ordered_fragment_translation import translate_ordered_fragment_pair
from inline_shader_translation import translate_inline_pair
from raw_shader_analysis import RawShaderError
from test_geometry_lod_translation import OPTIONS,pixel
from test_geometry_shader_translation import vertex

class OrderedFragmentTranslationTests(unittest.TestCase):
    def test_separate_abi_and_descriptor_contract(self):
        vs,ps,p=translate_ordered_fragment_pair(vertex(),pixel())
        self.assertEqual(p['abi_version'],33)
        self.assertEqual(p['ordered_fragment_completion_binding'],6)
        self.assertEqual(p['ordered_fragment_words'],10)
        self.assertEqual(p['ordered_fragment_capacity'],65536)
        self.assertFalse(p['native_draw_admission'])
        self.assertFalse(p['query_completion'])
        self.assertTrue(p['ordered_fragment_position_check_required'])
        self.assertIn('layout(location=16) noperspective out',vs)
        self.assertIn('layout(set=0,binding=6,std430)',ps)
        self.assertIn('record>=primitivePlane.capacity || record>=65536u',ps)
        self.assertIn('atomicOr(pixelTrace.words[5],8u);return;',ps)
        self.assertNotIn('gl_FragDepth=',ps)

    def test_original_math_sampling_and_legacy_abi_unchanged(self):
        oldv,oldp,old=translate_inline_pair(vertex(),pixel(),**OPTIONS)
        vs,ps,p=translate_ordered_fragment_pair(vertex(),pixel())
        self.assertEqual(old['abi_version'],32)
        self.assertEqual(old['pixel'],p['pixel'])
        self.assertEqual(old['vertex'],p['vertex'])
        # The whole original body occurs unchanged, in order, ahead of the
        # new receipt suffix. No replacement shader math or fixed outputs.
        body=oldp.split('void main() {',1)[1].rstrip()[:-1]
        self.assertIn(body,ps)
        self.assertEqual(oldv,vs.replace('layout(location=16) noperspective out float mergerNdc;\n','').replace('    mergerNdc=gl_Position.z/gl_Position.w;\n',''))
        self.assertNotIn('PixelTrace',oldp)

    def test_unexpected_layout_or_template_fails_closed(self):
        vs,ps,p=translate_inline_pair(vertex(),pixel(),**OPTIONS)
        for bad in ({**p,'texture_slots':list(range(6))},
                    {**p,'geometry_interpolator_masks':{'16':1}},
                    {**p,'abi_version':13}):
            with patch('ordered_fragment_translation.translate_inline_pair',return_value=(vs,ps,bad)):
                with self.assertRaises(RawShaderError):translate_ordered_fragment_pair(b'',b'')
        for a,b in ((vs.replace('void main() {','void changed() {'),ps),
                    (vs,ps.replace('void main() {','void main() { discard;'))):
            with patch('ordered_fragment_translation.translate_inline_pair',return_value=(a,b,p)):
                with self.assertRaises(RawShaderError):translate_ordered_fragment_pair(b'',b'')

if __name__=='__main__':unittest.main()
