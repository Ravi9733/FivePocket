from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'vulkan_probe'))
from generate_texture32_pixel_probe import make_inputs
from pixel_sample_dataflow import translate_texture32_pixel_compute,translate_texture32_pixel_fragment
from test_pixel_sample_dataflow import pixel,texture


class Texture32PixelArtifactTests(unittest.TestCase):
    def test_real_sampler_module_and_exact_binary_channel_references(self):
        rgba=bytes(255 if (i//4)&(1<<(i%4)) else 0 for i in range(4096))
        source,proof,combined,expected=make_inputs(pixel(),rgba,struct.pack('>4f',2,3,4,5),b'')
        self.assertEqual(len(combined),4112);self.assertEqual(len(expected),16416)
        self.assertEqual(expected[:16],bytes([0x5A])*16);self.assertEqual(expected[-16:],bytes([0x5A])*16)
        for i in range(1024):self.assertEqual(struct.unpack_from('<4f',expected,16+i*16),
            tuple([2,3,4,5][c] if i&(1<<c) else 0 for c in range(4)))
        self.assertIn('textureLod(imageSource,coordinate,0.0)',source)
        self.assertIn('index>=1024u',source);self.assertTrue(proof['native_texture_sampling'])
        self.assertTrue(proof['computed_lod_overridden']);self.assertFalse(proof['xenos_sampler_verified'])
        self.assertFalse(proof['native_fragment_pipeline']);self.assertFalse(proof['gpu_executed'])

    def test_invalid_reference_inputs_and_coordinate_linkage_reject(self):
        for rgba,tint,constants in ((bytes(4092),bytes(16),b''),(bytes([127])*4096,bytes(16),b''),
            (bytes(4096),bytes(12),b''),(bytes(4096),bytes(16),bytes(16))):
            with self.assertRaises(ValueError):make_inputs(pixel(),rgba,tint,constants)
        with self.assertRaises(ValueError):translate_texture32_pixel_compute(pixel(fetch=texture(src=1)))

    def test_fragment_adapter_has_explicit_interface_and_discards(self):
        source,proof=translate_texture32_pixel_fragment(pixel())
        self.assertEqual(proof['probe_stage'],'fragment');self.assertTrue(proof['synthetic_vertex_partner'])
        self.assertIn('layout(location=0) in vec2 coordinate;',source)
        self.assertIn('layout(location=0) out vec4 color0;',source)
        self.assertIn('discard;',source);self.assertIn('color0=vec4(',source)
        self.assertNotIn('gl_GlobalInvocationID',source);self.assertNotIn('result.words',source)
        self.assertNotIn('return;',source);self.assertNotIn('local_size_x',source)


if __name__=='__main__':unittest.main()
