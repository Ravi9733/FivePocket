from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from position_capture_preflight import summarize_positions,capture_position_report


class PositionCaptureTests(unittest.TestCase):
    def test_negative_w_not_reported_as_visible_ndc(self):
        r=summarize_positions([[.1,.1,-.5,-1]]*5,5)
        self.assertTrue(r['all_negative_w']);self.assertIsNone(r['positive_w_ndc_ranges'])
        self.assertFalse(r['gpu_executed']);self.assertTrue(r['ring_closed'])

    def test_mixed_w_and_quad_completeness(self):
        p=[[3,-1,.5,1],[3,-1,.5,-1],[3,1,.5,-1],[3,1,.5,1]]
        r=summarize_positions(p,13);self.assertIsNone(r['positive_w_ndc_ranges'])
        self.assertEqual(r['quads'][0]['constant_axes'],[0,2]);self.assertFalse(r['quads'][0]['point_degenerate'])
        with self.assertRaises(ValueError):summarize_positions(p[:3],13)

    def test_positive_ranges_and_ring(self):
        p=[[0,0,1,2],[-1,-1,1,2],[1,-1,1,2],[1,1,1,2],[-1,-1,1,2]]
        r=summarize_positions(p,5);self.assertEqual(r['positive_w_ndc_ranges'],[[-.5,.5],[-.5,.5],[.5,.5]])
        self.assertTrue(r['ring_closed']);p[-1]=[0,1,1,2];self.assertFalse(summarize_positions(p,5)['ring_closed'])

    def test_invalid_inputs(self):
        for p in ([],[[0,0,0,float('nan')]]*4,[[0,0,float('inf'),1]]*4,[[0,0,1]]*4,[[False,0,0,1]]*4):
            with self.assertRaises(ValueError):summarize_positions(p,13)
        with self.assertRaises(ValueError):summarize_positions([[0,0,0,1]]*4,True)
        with self.assertRaises(ValueError):capture_position_report({'vertex_count':4,'stride':16,'primitive':13,'index_count':0},b'',bytes(63),bytes(4096))
        with self.assertRaises(ValueError):capture_position_report({'vertex_count':4,'stride':16,'primitive':13,'index_count':1},b'',bytes(64),bytes(4096))
        with self.assertRaises(ValueError):capture_position_report([],b'',bytes(64),bytes(4096))
        with self.assertRaises(ValueError):capture_position_report({'vertex_count':4,'stride':16,'primitive':13,'index_count':False},b'',bytes(64),bytes(4096))
        r=summarize_positions([[1e308,0,0,1e-308]]*4,13)
        self.assertIsNone(r['positive_w_ndc_ranges']);self.assertFalse(r['positive_w_divide_finite'])


if __name__=='__main__':unittest.main()
