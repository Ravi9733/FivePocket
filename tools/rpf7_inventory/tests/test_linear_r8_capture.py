import hashlib
import json
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'vulkan_probe'))
import decode_linear_r8_capture as capture


class LinearR8CaptureTests(unittest.TestCase):
    def fixture(self, *, address=0x10000000, relative='range.bin', digest=None, kind=0, register=0x4800):
        words=(0x0A024802,0x10000002,0x0059E4FF,0x00281400,3,0x210)
        data=bytes(921600)
        metadata={'execution':{'stopped_physical_capture':{'status':1,'artifacts':[{
            'path':relative,'physical_address':address,'byte_count':len(data),
            'sha256':digest or hashlib.sha256(data).hexdigest()}]}}}
        encoded=json.dumps(metadata).encode()
        entry=SimpleNamespace(path=Path('/private/report.json'))
        packets=[(1,(20,kind,6,0,0x4800,0,0,0),words)]
        with patch.object(capture,'_prepare_inputs',return_value=[entry]), \
             patch.object(capture,'_read_private_input',side_effect=[encoded,data]), \
             patch.object(capture,'load_capture',return_value=({},{})), \
             patch.object(capture,'ordered_packets',return_value=packets):
            return capture.load_plane(entry.path,0,1,20,register)

    def test_selected_same_report_extent_hash_and_non_execution_flags(self):
        data,proof=self.fixture()
        self.assertEqual(len(data),921600)
        self.assertEqual(proof['histogram'],[921600]+[0]*255)
        self.assertTrue(proof['same_report'])
        self.assertFalse(proof['selected_descriptor_is_draw_binding_verified'])
        self.assertFalse(proof['gpu_execution_time_contents_verified'])
        self.assertFalse(proof['gpu_executed'])

    def test_wrong_range_hash_path_and_packet_fail(self):
        for change in ({'address':0x11000000},{'digest':'0'*64},{'relative':'../range.bin'},
                       {'relative':'/range.bin'},{'kind':1},{'register':0x4801}):
            with self.assertRaises(ValueError):self.fixture(**change)


if __name__=='__main__':unittest.main()
