"""Separate ABI16 contract; producer/consumer coverage is tested natively."""
import unittest
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from inline_shader_translation import translate_inline_pair
from raw_shader_analysis import RawShaderError
from test_geometry_shader_translation import vertex,OPTIONS
from test_mrt_shader_translation import mrt_pixel

class GeometryDepthWitnessTests(unittest.TestCase):
    def test_opt_in_and_legacy_unchanged(self):
        opts=dict(**OPTIONS,geometry_mrt_formats=(0,0,0,0),geometry_predicate_profile=True)
        _,old,p=translate_inline_pair(vertex(),mrt_pixel(),**opts)
        self.assertEqual(p['abi_version'],15);self.assertNotIn('witnessRequired',old)
        _,new,p=translate_inline_pair(vertex(),mrt_pixel(),**opts,geometry_depth_color_witness=True)
        self.assertEqual(p['abi_version'],16);self.assertTrue(p['geometry_depth_color_witness'])
        self.assertFalse(p['geometry_destination_coverage_verified'])
        self.assertIn('layout(offset=24) uvec2 witnessOrigin;',new)
        self.assertIn('layout(offset=32) uvec2 witnessExtent;',new)
        self.assertIn('layout(offset=40) uint witnessRequired;',new)
        self.assertIn('16u+uint(pixel.y)*extent.x+uint(pixel.x)',new)
        self.assertIn('previous<=0.0',new)
        self.assertGreater(new.rindex('geometryCheckDestinationWitness();'),new.rindex('geometryStoreMrtDepth20e4();'))
    def test_requires_pixel_mrt_profile(self):
        for v in (True,1,'enabled'):
            with self.assertRaises(RawShaderError):
                translate_inline_pair(vertex(),mrt_pixel(),**OPTIONS,geometry_mrt_formats=(0,0,0,0),geometry_depth_color_witness=v)

if __name__=='__main__':unittest.main()
