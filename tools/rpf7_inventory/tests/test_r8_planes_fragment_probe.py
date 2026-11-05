from pathlib import Path
import struct
import sys
import unittest
sys.path[:0]=[str(Path(__file__).resolve().parents[1]),str(Path(__file__).resolve().parents[2]/'vulkan_probe')]
from test_pixel_dot_scalar_dataflow import triple
from pixel_sample_dataflow import translate_r8_planes_pixel_fragment,R8_PLANE_IMAGE_BYTES
from generate_r8_planes_fragment_probe import make_inputs


class R8FragmentTests(unittest.TestCase):
    def test_fragment_interface_and_all_checks_before_store(self):
        source,proof=translate_r8_planes_pixel_fragment(triple())
        self.assertEqual(proof['probe_stage'],'fragment')
        self.assertEqual(proof['input_bytes'],R8_PLANE_IMAGE_BYTES+16)
        self.assertEqual(proof['output_bytes'],16416)
        self.assertTrue(proof['deferred_arithmetic_guards'])
        self.assertFalse(proof['title_linkage_verified'])
        self.assertIn('layout(location=0) in vec4 coordinate;',source)
        self.assertNotIn('gl_GlobalInvocationID',source)
        self.assertNotIn('return;',source)
        self.assertEqual(source.count('discard;'),8)
        self.assertLess(source.index('if (!valid) discard;'),source.index('color0=vec4'))
        self.assertLess(source.index('source.words.length()'),source.index('textureLod'))

    def test_full_raster_reference_and_nonuniform_footprint_rejection(self):
        planes=[bytes(1280*720),bytes([255])*(640*360),bytes(640*360)]
        _,_,data,expected=make_inputs(triple(),planes,struct.pack('>4f',1,1,1,1),b'')
        self.assertEqual(len(data),R8_PLANE_IMAGE_BYTES+16)
        self.assertEqual(expected,bytes([0x5A])*16+struct.pack('<4f',1,1,1,3)*1024+bytes([0x5A])*16)
        changed=list(planes);changed[0]=bytes((x&1)*255 for x in range(1280))*720
        with self.assertRaises(ValueError):make_inputs(triple(),changed,struct.pack('>4f',1,1,1,1),b'')


if __name__=='__main__':unittest.main()
