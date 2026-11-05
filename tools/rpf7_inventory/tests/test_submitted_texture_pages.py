from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from submitted_texture_pages import audit_pages,captured_plane_prefix


class SubmittedTexturePagesTests(unittest.TestCase):
    def fixture(self,address=0x10000000):
        descriptors=[None]*32;descriptors[7]=[2,address|20,0,0,0,0]
        count=min(16,(0x100000000-address)//4096)
        plane=dict(slot=7,mip=False,address=address,address_space_truncated=count<16,pages=[4096]+[0]*15)
        meta=dict(version=1,source='submitted-fetch-owned-pages',layout_verified=False,complete_texture=False,
                  page_bytes=4096,pages_per_plane_limit=16,total_byte_limit=4194304,
                  captured_bytes=4096,unavailable_pages=count-1,planes=[plane])
        return meta,dict(submitted_texture_source='driver-upload',submitted_textures=descriptors)

    def test_exact_pages_and_missing_evidence_stay_distinct(self):
        m,d=self.fixture();names=[]
        def read(name):names.append(name);return bytes(4096)
        r=audit_pages(m,d,read)
        self.assertEqual(names,['raw-texture-7-base-page-0.bin'])
        self.assertEqual(r['captured_bytes'],4096);self.assertEqual(r['unavailable_pages'],15)
        self.assertFalse(r['sampling_verified']);self.assertFalse(r['ownership_independently_verified'])

    def test_address_space_edge_is_not_missing_owner_page(self):
        m,d=self.fixture(0xFFFFF000);r=audit_pages(m,d,lambda _:bytes(4096))
        self.assertEqual(r['unavailable_pages'],0)
        self.assertEqual(r['planes'][0]['outside_address_space'],list(range(1,16)))

    def test_descriptor_mismatch_duplicate_and_false_completion_reject(self):
        for mutation in ('address','duplicate','source','complete'):
            m,d=self.fixture()
            if mutation=='address':m['planes'][0]['address']+=4096
            elif mutation=='duplicate':m['planes']*=2
            elif mutation=='source':d['submitted_texture_source']='cpu-shadow'
            else:m['complete_texture']=True
            with self.assertRaises(ValueError):audit_pages(m,d,lambda _:bytes(4096))

    def test_short_payload_and_disagreeing_counts_reject(self):
        m,d=self.fixture()
        with self.assertRaises(ValueError):audit_pages(m,d,lambda _:bytes(4095))
        m['captured_bytes']=8192
        with self.assertRaises(ValueError):audit_pages(m,d,lambda _:bytes(4096))

    def test_no_page_zero_wrap_or_boolean_indices(self):
        for field,value in (('address',0),('address',0x100000000),('slot',True),('address_space_truncated',True)):
            m,d=self.fixture();m['planes'][0][field]=value
            with self.assertRaises(ValueError):audit_pages(m,d,lambda _:bytes(4096))

    def test_prefix_stops_at_hole_does_not_fabricate_zeros(self):
        m,d=self.fixture();m['planes'][0]['pages'][2]=4096
        m['captured_bytes']=8192;m['unavailable_pages']=14
        read=lambda _:bytes([37])*4096
        audit=audit_pages(m,d,read)
        self.assertEqual(captured_plane_prefix(audit,7,False,read),bytes([37])*4096)
        with self.assertRaises(ValueError):captured_plane_prefix(audit,7,True,read)

    def test_prefix_rejects_changed_payload(self):
        m,d=self.fixture();audit=audit_pages(m,d,lambda _:bytes(4096))
        with self.assertRaises(ValueError):captured_plane_prefix(audit,7,False,lambda _:bytes([1])*4096)


if __name__=='__main__':unittest.main()
