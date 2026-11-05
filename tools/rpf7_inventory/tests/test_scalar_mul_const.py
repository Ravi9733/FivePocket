from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from scalar_mul_const import decode_scalar_mul_const,evaluate_scalar_mul_const


def scalar_instruction(register=0,constant=62,ca=0,ta=1,destination=0,mask=2):
    return ((42+(register%2))<<26 | destination<<8 | mask<<20,
        ((ca+1)%4)<<6 | (register&60) | ta,
        2<<24 | ((register>>1)&1)<<29 | constant)


class ScalarMulConstTests(unittest.TestCase):
    def test_all_address_axes_and_masks(self):
        for register in range(64):
            for ca in range(4):
                for ta in range(4):
                    for mask in range(16):
                        words=scalar_instruction(register,255,ca,ta,63,mask)
                        proof=decode_scalar_mul_const(words)
                        self.assertEqual([proof[k] for k in ('temporary_register','constant_register',
                            'constant_component','temporary_component','destination_register','write_mask')],
                            [register,255,ca,ta,63,mask])
                        self.assertTrue(proof['previous_scalar_updated'])
                        self.assertFalse(proof['vector_executed']);self.assertFalse(proof['gpu_executed'])
                        self.assertEqual(evaluate_scalar_mul_const(words,struct.pack('>4f',2,3,4,5),[7,11,13,17]),
                            [2,3,4,5][ca]*[7,11,13,17][ta])

    def test_zero_mask_still_computes_previous_scalar_and_positive_zero(self):
        words=scalar_instruction(mask=0)
        self.assertEqual(evaluate_scalar_mul_const(words,struct.pack('>4f',3,0,0,0),[None,2,None,None]),6)
        self.assertEqual(struct.pack('>f',evaluate_scalar_mul_const(words,struct.pack('>4f',-0.0,0,0,0),
            [None,-2,None,None])),bytes(4))

    def test_guards(self):
        words=scalar_instruction()
        for wi,bit in [(0,b) for b in (6,7,14,15,24,25)]+[(1,b) for b in range(24,32)]:
            changed=list(words);changed[wi]^=1<<bit
            with self.assertRaises(ValueError):decode_scalar_mul_const(changed)
        for malformed in ([],[0,0],[-1,0,0],[True,0,0],[1<<32,0,0]):
            with self.assertRaises(ValueError):decode_scalar_mul_const(malformed)
        for constant,temp in ((bytes(15),[1]*4),(bytes(16),[None]*4),(bytes(16),[1]*3),
            (struct.pack('>4f',float('nan'),0,0,0),[1]*4),
            (struct.pack('>4f',float('inf'),0,0,0),[0]*4),
            (struct.pack('>4f',2**-149,0,0,0),[1]*4),
            (struct.pack('>4f',2**127,0,0,0),[4]*4),
            (struct.pack('>4f',2**-126,0,0,0),[0.5]*4)):
            with self.assertRaises(ValueError):evaluate_scalar_mul_const(words,constant,temp)


if __name__=='__main__':unittest.main()
