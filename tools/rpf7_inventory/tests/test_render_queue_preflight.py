import copy
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from render_queue_preflight import queue_requirements


class QueuePreflightTests(unittest.TestCase):
    def fixture(self):
        root, bank = 0x10004000, 0xA0008000
        word = (5<<20)|(7<<12)
        records = [dict(handle=0,bank_pointer=root+1032,bank_base=bank,address=bank,word=word,
                        advance=20,handler_slot=7,handler=0x822F6500,payload_bytes=20),
                   dict(handle=20,bank_pointer=root+1032,bank_base=bank,address=bank+20,word=2<<12,
                        advance=0,handler_slot=2,handler=0,payload_bytes=4)]
        return dict(version=1,execution_proven=False,root=root,entry_handle=0,records=records,stop='linear-terminator'),{
            'queue-record-0.bin':struct.pack('>4I',word,0xFF000000,0x3F800000,255)+bytes([63,0,0,0]),
            'queue-record-1.bin':struct.pack('>I',2<<12)}

    def test_families_and_known_adapter_not_execution(self):
        m,p=self.fixture();r=queue_requirements(m,p)
        self.assertFalse(r['issues']);self.assertFalse(r['unavailable']);self.assertTrue(r['unverified'])
        self.assertEqual(r['suggested_codegen_roots'],['822F6500'])
        self.assertEqual(r['records'][0]['queued_clear_adapter'],dict(packed_color=0xFF000000,
            depth_float32_bits=0x3F800000,stencil=255,flags_byte=63))

    def test_independent_missing(self):
        m,p=self.fixture();m['records'][0]['handler']=None;p.pop('queue-record-0.bin')
        r=queue_requirements(m,p);self.assertEqual(len(r['unavailable']),2)
        self.assertFalse(r['suggested_codegen_roots']);self.assertEqual(r['records'][1]['handler'],0)

    def test_metadata_cross_checks(self):
        m,p=self.fixture()
        for field in ['handle','bank_pointer','bank_base','address','advance','handler_slot']:
            bad=copy.deepcopy(m);bad['records'][0][field]+=4
            self.assertTrue(queue_requirements(bad,p)['issues'],field)
        p['queue-record-0.bin']=bytes(20)
        self.assertIn('record 0: body/header mismatch',queue_requirements(m,p)['issues'])

    def test_limits_and_non_integer_words(self):
        m,p=self.fixture()
        for key,value in [('version',2),('execution_proven',True),('root',True),('entry_handle',-1),('records',[{}]*129)]:
            bad=copy.deepcopy(m);bad[key]=value
            with self.assertRaises(ValueError):queue_requirements(bad,p)
        p['queue-record-0.bin']=bytes(256*1024+1)
        with self.assertRaises(ValueError):queue_requirements(m,p)

    def test_no_mutation_or_unknown_adapter_decoding(self):
        m,p=self.fixture();m['records'][0]['handler']=0x82220000
        before=copy.deepcopy((m,p));r=queue_requirements(m,p)
        self.assertEqual((m,p),before);self.assertNotIn('queued_clear_adapter',r['records'][0])


if __name__=='__main__':unittest.main()
