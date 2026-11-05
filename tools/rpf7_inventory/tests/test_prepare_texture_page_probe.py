from copy import deepcopy
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from prepare_texture_page_probe import captured_payloads
import test_submitted_texture_pages as page_fixtures


class PrepareTextureProbeTests(unittest.TestCase):
    def fixture(self):
        fetch=[2,0x10000014,0,0,0,0]
        descriptors=[None]*32;descriptors[3]=fetch
        return dict(submitted_texture_source='driver-upload',submitted_textures=descriptors,
            textures=[dict(slot=3,fetch=fetch.copy(),literal_zero=False,bytes=16)])

    def test_exact_complete_capture_is_not_missing_page_evidence(self):
        d=self.fixture();calls=[]
        def read(name,limit):calls.append((name,limit));return bytes(range(16))
        payloads,kind=captured_payloads(d,{},read,3)
        self.assertEqual(calls,[('texture-3.bin',16)])
        self.assertEqual(payloads,{'fetch.bin':struct.pack('>6I',*d['submitted_textures'][3]),
            'base.bin':bytes(range(16)),'mips.bin':b''})
        self.assertIn('still requires validation',kind)

    def test_stale_duplicate_literal_mip_extent_and_bad_slot_reject(self):
        for mutation in ('stale','duplicate','literal','mip','short','long','zero','bool','source'):
            d=deepcopy(self.fixture());size=16
            if mutation=='stale':d['textures'][0]['fetch'][1]+=4096
            elif mutation=='duplicate':d['textures']*=2
            elif mutation=='literal':d['textures'][0]['literal_zero']=True
            elif mutation=='mip':
                d['textures'][0]['fetch'][5]=d['submitted_textures'][3][5]=4096
            elif mutation=='short':size=15
            elif mutation=='long':size=17
            elif mutation=='zero':d['textures'][0]['bytes']=0
            elif mutation=='bool':d['textures'][0]['bytes']=True
            elif mutation=='source':d['submitted_texture_source']='cpu-shadow'
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):captured_payloads(d,{},lambda *_:bytes(size),3)
        for slot in (-1,32,True):
            with self.assertRaises(ValueError):captured_payloads(self.fixture(),{},lambda *_:bytes(16),slot)

    def test_existing_page_prefix_fallback_preserved(self):
        m,d=page_fixtures.SubmittedTexturePagesTests().fixture()
        result,kind=captured_payloads(d,m,lambda *_:bytes([7])*4096,7)
        self.assertEqual(result['base.bin'],bytes([7])*4096);self.assertEqual(result['mips.bin'],b'')
        self.assertIn('no complete layout claim',kind)


if __name__=='__main__':unittest.main()
