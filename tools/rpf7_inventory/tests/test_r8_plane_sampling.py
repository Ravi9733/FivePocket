from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'vulkan_probe'))
from generate_r8_plane_sampling_fixture import sweep_planes,integer_filter_reference,make_inputs,EXTENTS


class R8SamplingTests(unittest.TestCase):
    def test_all_byte_values_and_independent_planes(self):
        planes=sweep_planes()
        for n in range(256):
            coords=((n*5+2.5)/1280,(n*2+1)/640,(n*2+1)/640)
            for i,(p,(w,h)) in enumerate(zip(planes,EXTENTS)):
                u=struct.unpack('>f',struct.pack('>f',coords[i]))[0]
                expected=n if i==0 else (n+37)%256 if i==1 else (n*3+11)%256
                self.assertEqual(integer_filter_reference(p,w,h,u,.5),expected)
        source,expected=make_inputs('sweep',planes)
        self.assertEqual(len(source),1382400)
        for i in range(4):self.assertEqual(struct.unpack_from('<4I',expected,16+i*16),(64,64,64,1))

    def test_integer_averages_and_rejected_fractional_intermediates(self):
        data=bytes((0,64,128,192))
        self.assertEqual(integer_filter_reference(data,2,2,.375,.375),48)
        self.assertEqual(integer_filter_reference(data,2,2,-1,2),96)
        with self.assertRaises(ValueError):integer_filter_reference(bytes((0,255,255,0)),2,2,0,0)
        with self.assertRaises(ValueError):integer_filter_reference(data,2,2,.3,.3)
        with self.assertRaises(ValueError):integer_filter_reference(data,2,2,float('nan'),0)


if __name__=='__main__':unittest.main()
