from copy import deepcopy
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from affine_position import affine_position_xy


def fixture():
    # Synthetic proof, no game shader/constants/vertices.
    return dict(nodes=[('initial',0,0),('fetch',0,0),('fetch',0,1),('constant',7,2),
                       ('constant',8,1),('mul',1,3),('mul',3,2),('add',5,4),
                       ('add',4,6),('literal',0),('literal',1)],
                fetches=[dict(fetch_slot=95,format=57,index_expression=0,
                              offset_words=1,stride_words=9,components=3)],
                exports={'62':[7,8,9,10]})


class AffinePositionTests(unittest.TestCase):
    def test_extracts_both_commutative_orders(self):
        self.assertEqual(affine_position_xy(fixture()),
                         dict(offset_bytes=[4,8],scale_words=[30,30],bias_words=[33,33],stride=36))

    def test_requires_zero_z_unit_w_and_complete_position(self):
        for lane in (2,3):
            p=fixture();p['exports']['62'][lane]=3
            self.assertIsNone(affine_position_xy(p))
        p=fixture();p['exports']['62'].pop();self.assertIsNone(affine_position_xy(p))

    def test_rejects_unknown_and_unbounded_stream(self):
        for key,value in (('format',6),('fetch_slot',94),('stride_words',1),('index_expression',3)):
            p=fixture();p['fetches'][0][key]=value;self.assertIsNone(affine_position_xy(p))
        p=fixture();p['nodes'][1]=('fetch',0,3);self.assertIsNone(affine_position_xy(p))

    def test_rejects_nonlinear_modified_and_invalid_constants(self):
        for replacement in (('negate',1),('mul',1,2),('saturate',5)):
            p=fixture();p['nodes'][5]=replacement;self.assertIsNone(affine_position_xy(p))
        for replacement in (('constant',256,0),('constant',0,4),('initial',1,0)):
            p=fixture();p['nodes'][3]=replacement;self.assertIsNone(affine_position_xy(p))

    def test_copies_work_but_cycles_do_not(self):
        p=fixture();p['nodes'].append(('regular_copy',7));p['exports']['62'][0]=11
        self.assertIsNotNone(affine_position_xy(p))
        p['nodes'][11]=('regular_copy',11);self.assertIsNone(affine_position_xy(p))


if __name__ == '__main__': unittest.main()
