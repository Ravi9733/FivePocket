import copy
import struct
import unittest
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from resolve_capture_preflight import resolve_requirements


class ResolvePreflightTests(unittest.TestCase):
    def fixture(self):
        meta = dict(version=2, arguments=[0x1004CD00, 0x100, 0x64000090, 0xABFBABB4,
                    0, 0, 0, 0x64000080, 0x823889E8, 0x10278C50], source_plan=dict(width=512, height=512))
        header = bytes(28) + struct.pack('>6I', 2 | (18 << 22) | (1 << 31), 0x10006,
                                        511 | (511 << 13), (0 | (1 << 3) | (2 << 6) | (3 << 9)) << 1, 0, 512)
        payloads = {'source-resource.bin':bytes(24)+struct.pack('>2I',560,1601), 'destination-resource.bin':header,
                    'source-rectangle.bin':struct.pack('>4i',0,0,512,512),
                    'clear-color.bin':struct.pack('>4f',0,.5,1,0),
                    'stack-92.bin':bytes(4), 'stack-100.bin':bytes(4)}
        return meta, payloads

    def test_complete_capture_is_not_complete_compatibility(self):
        m, p = self.fixture(); r = resolve_requirements(m, p)
        self.assertEqual(r['issues'], []); self.assertEqual(r['unavailable'], [])
        self.assertTrue(r['unverified']); self.assertEqual(r['header_reader'], 'graphics')
        self.assertFalse(r['depth_value_used']); self.assertTrue(r['clear_after_copy'])
        self.assertEqual(r['destination']['pitch'], 576)

    def test_old_capture_missing_bytes_are_not_guessed(self):
        m, p = self.fixture(); m['version'] = 1; m.pop('source_plan')
        for name in ('source-rectangle.bin','clear-color.bin','destination-resource.bin'): p[name] = b''
        r = resolve_requirements(m, p)
        self.assertEqual(len(r['unavailable']), 3); self.assertIsNone(r['source_rectangle'])
        self.assertIsNone(r['destination']); self.assertIn('source full written extent', r['unverified'])

    def test_all_requirements_reported_after_first_failure(self):
        m, p = self.fixture(); m['arguments'][1] |= 0x200; m['arguments'][5] = 1
        m['arguments'][4] = 0x640000B0
        p['destination-point.bin'] = struct.pack('>2i',1,0)
        p['source-rectangle.bin'] = struct.pack('>4i',0,0,256,512)
        p['clear-color.bin'] = struct.pack('>4f',float('nan'),0,0,0)
        p['stack-92.bin'] = struct.pack('>I',1)
        r = resolve_requirements(m, p)
        self.assertEqual(len(r['issues']), 6); self.assertTrue(r['depth_value_used'])

    def test_unused_color_is_not_required(self):
        m, p = self.fixture(); m['arguments'][1] = 0; m['arguments'][2] = 0
        p.pop('clear-color.bin'); p.pop('source-rectangle.bin')
        r = resolve_requirements(m, p)
        self.assertEqual(r['issues'], []); self.assertEqual(r['unavailable'], [])

    def test_destination_properties_are_audited_together(self):
        m, p = self.fixture()
        p['destination-resource.bin'] = bytes(28) + struct.pack('>6I',0,2,511 | (255 << 13),1,4,2048)
        r = resolve_requirements(m, p)
        self.assertGreaterEqual(len(r['issues']), 6)

    def test_source_storage_requirements_are_not_inferred_from_extent(self):
        m, p = self.fixture()
        p['source-resource.bin'] = bytes(24)+struct.pack('>2I',512 | (1<<16),2047 | (3<<16))
        r = resolve_requirements(m, p)
        self.assertIn('source pitch profile', r['issues'])
        self.assertIn('source sample/format/exponent profile', r['issues'])
        self.assertIn('source EDRAM wraps', r['issues'])

    def test_bad_capture_rejected(self):
        m, p = self.fixture()
        for key, value in [('version',3),('arguments',[]),('arguments',[True]*10)]:
            bad = copy.deepcopy(m); bad[key] = value
            with self.assertRaises(ValueError): resolve_requirements(bad, p)
        p['source-rectangle.bin'] = bytes(15)
        self.assertIn('malformed source-rectangle.bin', resolve_requirements(m,p)['issues'])

    def test_full_source_header_and_explicit_mip_are_not_false_failures(self):
        m,p=self.fixture();p['source-resource.bin']+=bytes(8)
        words=list(struct.unpack('>6I',p['destination-resource.bin'][28:]));words[4]=3<<6;words[5]=0x20000200
        p['destination-resource.bin']=bytes(28)+struct.pack('>6I',*words)
        base=resolve_requirements(m,p);self.assertEqual(base['issues'],[])
        self.assertEqual(base['destination']['mip_max'],3)
        m['arguments'][5]=1;m['source_plan']=dict(width=256,height=256)
        p['source-rectangle.bin']=struct.pack('>4i',0,0,256,256)
        level=resolve_requirements(m,p);self.assertEqual(level['issues'],[])
        self.assertEqual(level['destination']['selected_extent'],[256,256])
        self.assertTrue(level['unverified']) # Layout inventory is not native admission.


if __name__ == '__main__': unittest.main()
