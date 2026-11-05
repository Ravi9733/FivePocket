from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from edram_depth_probe_reference import reference_words, uniform_depth_initial_pixel, BACKING_BYTES, COLOR_BYTES, EXPECTED_BYTES, CLEAR_WORD
from edram_alias_clear import sample_offset, canonical_depth_bits
from half_color_store import packed_half
from test_rectangle_vertex_stage import rectangle

DEPTH = (0xA020280,0x2D0,640,360,True)
COLOR = (0x14000500,0xC02D0,1280,720,False)


class EdramDepthReferenceTests(unittest.TestCase):
    def depth_source(self,bits=0x3F400001,right=639.5):
        return b''.join(struct.pack('>2fI f',x,y,bits,1) for x,y in ((-.5,-.5),(right,-.5),(right,359.5)))

    def test_uniform_native_depth_initialization_roundtrips(self):
        for n in (0,1,0x1FFFFF,0x400001,0x7FFFFF,0x800000,0xC00000,0xFFFFFF):
            bits=canonical_depth_bits(n)
            pixel,proof=uniform_depth_initial_pixel(rectangle(False),self.depth_source(bits))
            self.assertEqual(packed_half(struct.unpack('<4H',pixel)),n<<8)
            self.assertEqual(proof['initial_packed_word'],n<<8)
            self.assertTrue(proof['live_depth_connected']);self.assertFalse(proof['gpu_executed'])
            self.assertFalse(proof['cpu_supplied_initial_edram'])
        pixel,_=uniform_depth_initial_pixel(rectangle(False),self.depth_source())
        self.assertEqual(pixel,struct.pack('<4e',0,0,0,1))

    def test_uniform_depth_oracle_rejects_unproved_initialization(self):
        for source in (self.depth_source(0x3F000000),self.depth_source(0x80000000),self.depth_source(1),
            self.depth_source(0x7FC00000),self.depth_source(right=319.5),self.depth_source()[:-4]):
            with self.assertRaises(ValueError):uniform_depth_initial_pixel(rectangle(False),source)

    def test_full_opaque_clear_and_four_buffer_guards(self):
        data=reference_words([-.5,-.5,639.5,359.5],0x3F400001,15)
        self.assertEqual(len(data),EXPECTED_BYTES)
        self.assertEqual(data[16+3686400:16+7372800],struct.pack('<I',0xC0000000)*921600)
        self.assertEqual(data[BACKING_BYTES+16:-16],struct.pack('<I',0xC0000000)*921600)
        for at in (0,BACKING_BYTES-16,BACKING_BYTES,EXPECTED_BYTES-16):
            self.assertEqual(data[at:at+16],bytes([0x5A])*16)
        self.assertEqual(data[16:16+3686400],bytes([0x5A])*3686400)
        self.assertEqual(data[16+7372800:BACKING_BYTES-16],bytes([0x5A])*(10485760-7372800))

    def test_individual_sample_masks_and_depth_half_rows(self):
        for mask in (0,1,2,4,8,5,10,15):
            data=reference_words([-.5,-.5,19.5,7.5],0x3F400001,mask)
            for x,y in ((0,0),(19,7),(20,0),(0,8),(639,359)):
                for sample in range(4):
                    expected=0xC0000000 if x<20 and y<8 and mask&(1<<sample) else CLEAR_WORD
                    at=16+sample_offset(DEPTH,x,y,sample)
                    self.assertEqual(struct.unpack_from('<I',data,at)[0],expected)
            for x,y in ((0,0),(39,0),(40,0),(79,15),(1279,719)):
                color=struct.unpack_from('<I',data,BACKING_BYTES+16+(y*1280+x)*4)[0]
                self.assertEqual(color,struct.unpack_from('<I',data,16+sample_offset(COLOR,x,y,0))[0])

    def test_noncanonical_samples_stay_unwritten_not_quantized(self):
        data=reference_words([-.5,-.5,639.5,359.5],0x3F000000,15)
        self.assertEqual(data,bytes([0x5A])*EXPECTED_BYTES)
        data=reference_words([-.5,-.5,639.5,359.5],0x3F400001,0)
        self.assertEqual(data[BACKING_BYTES+16:-16],struct.pack('<I',CLEAR_WORD)*921600)

    def test_reference_domain_guards(self):
        for bounds,bits,mask in (([0,0,639,359],0,15),([-.5,-.5,-.5,359.5],0,15),
            ([-.5,-.5,639.5,359.5],0x80000000,15),([-.5,-.5,639.5,359.5],0,16),
            ([-.5,-.5,639.5,359.5],0,True)):
            with self.assertRaises(ValueError):reference_words(bounds,bits,mask)


if __name__=='__main__':unittest.main()
