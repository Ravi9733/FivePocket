import copy
import struct
import unittest
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from clear_capture_preflight import clear_requirements


class ClearPreflightTests(unittest.TestCase):
    def fixture(self):
        m = dict(version=1, stack=0x64000000, device=0x1004CD00, flags=63, rectangle=0x64000080,
                 color=0x64000090, stencil=0, link=0x8241B904, depth_bits=0x3FF0000000000000)
        state = bytearray(13864)
        for at, value in [(12816,0x10274D10),(10368,0x20100A0),(10372,0x10000),(11068,0x42400),
                          (13032,0x43000000),(13036,0x43000000),(12288,1),(13060,128),(13064,128)]:
            struct.pack_into('>I',state,at,value)
        header = bytes(24)+struct.pack('>4I',0x20100A0,0x10000,0,(127<<18)|(127<<3))
        return m, {'device-state.bin':state,'clear-rectangle.bin':struct.pack('>4i',0,0,128,128),
                   'clear-color.bin':struct.pack('>4f',0,0,0,1),'surface-0-resource.bin':header}

    def test_complete_is_not_gameplay_or_draw_support(self):
        m,p=self.fixture(); r=clear_requirements(m,p)
        self.assertFalse(r['issues']); self.assertFalse(r['unavailable']); self.assertFalse(r['binned'])
        self.assertEqual(r['effective_flags'],15); self.assertEqual(r['surfaces'][0]['sample_count'],2)
        self.assertEqual(r['surfaces'][0]['tile_count'],32); self.assertTrue(r['unverified'])

    def test_missing_is_not_null_or_zero(self):
        m,p=self.fixture(); p.pop('device-state.bin'); p.pop('clear-color.bin')
        r=clear_requirements(m,p)
        self.assertIsNone(r['effective_flags']); self.assertIsNone(r['color'])
        self.assertEqual(len(r['unavailable']),2); self.assertIsNone(r['binned'])

    def test_all_independent_issues_preserved(self):
        m,p=self.fixture(); m['flags']|=64
        struct.pack_into('>I',p['device-state.bin'],12832,0x10001000)
        p['device-state.bin'][11068]=0x10
        p['clear-color.bin']=struct.pack('>4f',float('nan'),0,0,1)
        p['clear-rectangle.bin']=struct.pack('>4i',0,0,127,128)
        r=clear_requirements(m,p)
        self.assertEqual(len(r['issues']),4); self.assertIn('surface-4-resource.bin',r['unavailable'])
        self.assertEqual(r['color'][0],'nan')

    def test_binning_byte_and_cache_semantics(self):
        m,p=self.fixture(); p['device-state.bin'][11068]=0x20
        self.assertFalse(clear_requirements(m,p)['binned'])
        struct.pack_into('>I',p['device-state.bin'],13104,0x10274D10)
        self.assertTrue(clear_requirements(m,p)['binned'])

    def test_coupled_exact_float24_full_clear(self):
        m,p=self.fixture();m['depth_bits']=0;m['stencil']=255
        struct.pack_into('>I',p['device-state.bin'],12832,0x10274D50)
        struct.pack_into('>I',p['device-state.bin'],10376,0x10020)
        p['surface-4-resource.bin']=bytes(24)+struct.pack('>4I',0x20100A0,0x10020,0,(127<<18)|(127<<3))
        r=clear_requirements(m,p);self.assertFalse(r['issues']);self.assertFalse(r['unavailable'])
        self.assertEqual(r['exact_depth_encoding'],0);self.assertEqual(r['surfaces'][4]['tile_count'],32)
        m['depth_bits']=struct.unpack('>Q',struct.pack('>d',.1))[0]
        self.assertIn('depth/stencil requires unproved rounding or conversion',clear_requirements(m,p)['issues'])
        m['flags']=31
        self.assertIn('single-aspect clear requires preservation of the other aspect',clear_requirements(m,p)['issues'])

    def test_argument_validation(self):
        m,p=self.fixture()
        for key, value in [('version',2),('flags',True),('depth_bits',1<<64),('stencil',-1)]:
            bad=copy.deepcopy(m); bad[key]=value
            with self.assertRaises(ValueError): clear_requirements(bad,p)
        p['surface-0-resource.bin']=bytes(32)
        self.assertIn('malformed surface-0-resource.bin',clear_requirements(m,p)['issues'])


if __name__ == '__main__': unittest.main()
