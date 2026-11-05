from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from half_color_reference import full_half_reference,persistent_half_reference,mixed_half_reference,sequence_store_copy_reference,uniform_initial_edram,blend_half_pixel,OUTPUT_BYTES


class HalfColorReferenceTests(unittest.TestCase):
    source=bytes([255])*4096+bytes(256)
    profile='texture32_vertex_pair_test'

    def reference(self,color):return bytes([0x5A])*16+struct.pack('<4f',*color)*1024+bytes([0x5A])*16

    def test_half_storage_and_midpoint_neighbors(self):
        for color,want in (((0,1,-2,65504),(0,0x3C00,0xC000,0x7BFF)),
            ((1+2**-11,1+2**-11+2**-23,1+3*2**-11,2**-14),(0x3C00,0x3C01,0x3C02,0x400))):
            out,proof=full_half_reference(self.profile,self.source,self.reference(color))
            self.assertEqual(len(out),OUTPUT_BYTES);self.assertEqual(tuple(proof['half_words']),want)
            self.assertEqual(out[16:-16],struct.pack('<4H',*want)*(1280*720))
            self.assertEqual(out[:16]+out[-16:],bytes([0x5A])*32)

    def test_discarded_and_uniform_planes(self):
        for profile,source in ((self.profile,self.source),('r8_planes_vertex_pair_test',bytes(1382720))):
            out,proof=full_half_reference(profile,source,bytes([0x5A])*16416)
            self.assertTrue(proof['discarded']);self.assertEqual(out,bytes([0x5A])*OUTPUT_BYTES)
        varied=bytearray(1382720);varied[921600]=255
        with self.assertRaises(ValueError):full_half_reference('r8_planes_vertex_pair_test',bytes(varied),bytes([0x5A])*16416)

    def test_toward_zero_is_separate_explicit_expectation(self):
        color=(1+3*2**-12,-(1+3*2**-12),1+2**-12,-(1+2**-12))
        _,proof=full_half_reference(self.profile,self.source,self.reference(color),'toward-zero')
        self.assertEqual(proof['half_words'],[0x3C00,0xBC00,0x3C00,0xBC00])
        with self.assertRaises(ValueError):full_half_reference(self.profile,self.source,self.reference(color),'either')

    def test_rejects_unproved_new_grid_and_layout(self):
        ref=self.reference((1,1,1,1))
        for profile,source,reference in (('unknown',self.source,ref),(self.profile,self.source[:-4],ref),
            (self.profile,b'\0'+self.source[1:],ref),(self.profile,bytes([128])*4096+bytes(256),ref),
            (self.profile,self.source,ref[:-4]),(self.profile,self.source,b'\0'+ref[1:]),
            (self.profile,self.source,ref[:32]+bytes(16)+ref[48:]),(self.profile,bytearray(self.source),ref)):
            with self.assertRaises(ValueError):full_half_reference(profile,source,reference)

    def test_nonfinite_negative_zero_and_narrowing_extremes_reject(self):
        for invalid in (-0.,float('nan'),float('inf'),-float('inf'),2**-15,65536):
            for lane in range(4):
                color=[0.,1.,-1.,2.];color[lane]=invalid
                with self.assertRaises(ValueError):full_half_reference(self.profile,self.source,self.reference(color))

    def test_persistent_blend_stores_each_draw_and_loads_last_pass(self):
        out,proof=persistent_half_reference(self.profile,self.source,self.reference((2,4,8,.5)),'toward-zero')
        self.assertEqual(len(out),72)
        self.assertEqual(proof['rgba_per_pass'][0],[203.25]*4)
        self.assertEqual(proof['rgba_per_pass'][1],[102.625,103.625,105.625,101.875])
        self.assertEqual(proof['rgba_per_pass'][2],[27.15625,28.90625,32.40625,25.84375])
        self.assertEqual(proof['rgba_per_pass'][3],[5.14453125,7.11328125,11.046875,3.66796875])
        self.assertEqual(out[40:48],out[48:56]);self.assertEqual(out[:16]+out[-16:],bytes([0x5A])*32)

    def test_persistent_zero_alpha_discard_and_opaque(self):
        for ref in (self.reference((2,4,8,0)),bytes([0x5A])*16416):
            out,_=persistent_half_reference(self.profile,self.source,ref,'toward-zero')
            self.assertEqual(out,bytes([0x5A])*72)
        _,proof=persistent_half_reference(self.profile,self.source,self.reference((2,4,8,1)))
        self.assertEqual(proof['rgba_per_pass'][1:],[[2,4,8,1]]*4)
        for alpha in (-.25,1.25):
            with self.assertRaises(ValueError):persistent_half_reference(self.profile,self.source,self.reference((1,1,1,alpha)))

    def test_distinct_color_pairs_share_prior_half_color(self):
        out,proof=mixed_half_reference(bytes(1382720),self.reference((2,4,8,1)),
            self.source,self.reference((4,8,16,.5)),'toward-zero')
        self.assertEqual(len(out),64);self.assertEqual(proof['rgba_per_pass'],
            [[203.25]*4,[2,4,8,1],[3,6,12,.75],[3,6,12,.75]])
        self.assertEqual(out[:16]+out[-16:],bytes([0x5A])*32)
        out,proof=mixed_half_reference(bytes(1382720),bytes([0x5A])*16416,
            self.source,self.reference((2,4,8,1)),'toward-zero')
        self.assertEqual(proof['rgba_per_pass'],[[203.25]*4,[203.25]*4,[2,4,8,1],[2,4,8,1]])

    def test_sequence_copy_record_uses_packed_color_and_bgra(self):
        guard=bytes([0x5A])*16
        reference=guard+struct.pack('<4e',0,0,0,1)*4+guard
        out,proof=sequence_store_copy_reference(reference)
        self.assertEqual(len(out),96);self.assertEqual(proof['packed_words'],[0xC0000000]*4)
        self.assertEqual(proof['copy_words'],[0xFF000000]*4)
        reference=guard+struct.pack('<4e',1,0,0,1)*4+guard
        _,proof=sequence_store_copy_reference(reference);self.assertEqual(proof['copy_words'],[0xFFFF0000]*4)
        for bad in (reference[:-4],b'\0'+reference[1:],guard+struct.pack('<4H',1,0,0,0x3C00)*4+guard):
            with self.assertRaises(ValueError):sequence_store_copy_reference(bad)

    def test_product_narrowing_is_a_separate_falsifiable_policy(self):
        prior=(-0.87060546875,0.529296875,-1.0810546875,1.)
        source=(2.,2.,2.,.25)
        float32=blend_half_pixel(source,prior,'toward-zero')
        half=blend_half_pixel(source,prior,'toward-zero','binary16-products-toward-zero')
        self.assertEqual(struct.unpack('<4H',float32),(0xB0E5,0x3B2D,0xB4F9,0x3A80))
        self.assertEqual(struct.unpack('<4H',half),(0xB0E4,0x3B2D,0xB4F8,0x3A80))
        # Exact products do not distinguish the policies, and opaque draws
        # cannot establish fractional blend arithmetic.
        for alpha in (0.,.5,1.):
            source=(2.,2.,2.,alpha)
            self.assertEqual(blend_half_pixel(source,prior,'toward-zero'),
                blend_half_pixel(source,prior,'toward-zero','binary16-products-toward-zero'))
        for precision in ('unknown','adreno','auto'):
            with self.assertRaises(ValueError):blend_half_pixel(source,prior,'toward-zero',precision)
        with self.assertRaises(ValueError):
            blend_half_pixel((2**-14,1.,1.,.5),prior,'toward-zero','binary16-products-toward-zero')

    def test_nearest_products_distinguish_three_eighths(self):
        source=(2.,2.,2.,.375);prior=(-0.87060546875,0.529296875,-1.0810546875,1.)
        pixel=blend_half_pixel(source,prior,'nearest-even','binary16-inputs-rtz-products-rne')
        self.assertEqual(struct.unpack('<4H',pixel),(0x3298,0x3C53,0x2CC0,0x3A20))
        self.assertNotEqual(pixel,blend_half_pixel(source,prior,'nearest-even','binary16-products-toward-zero'))

    def test_initial_edram_requires_defined_uniform_footprint(self):
        source=bytearray([0x5A])*10485792;begin,end=16+3686400,16+7372800
        source[begin:end]=struct.pack('<I',0xC0000000)*921600;source=bytes(source)
        pixel,proof=uniform_initial_edram(source)
        self.assertEqual(pixel,struct.pack('<4e',0,0,0,1));self.assertFalse(proof['live_depth_connected'])
        _,ref=mixed_half_reference(bytes(1382720),self.reference((2,4,8,.5)),self.source,
            self.reference((0,0,0,0)),'toward-zero',pixel)
        self.assertEqual(ref['rgba_per_pass'],[[0,0,0,1],[1,2,4,.75],[1,2,4,.75],[1,2,4,.75]])
        for at in (0,16,begin+4,end,len(source)-1):
            bad=bytearray(source);bad[at]^=1
            with self.assertRaises(ValueError):uniform_initial_edram(bytes(bad))


if __name__=='__main__':unittest.main()
