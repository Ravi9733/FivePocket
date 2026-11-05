from pathlib import Path
import struct
import sys
import unittest
sys.path[:0]=[str(Path(__file__).resolve().parents[1]),str(Path(__file__).resolve().parents[2]/'vulkan_probe')]
from rectangle_vertex_stage import translate_rectangle_vertex,evaluate_rectangle,passthrough_fragment
from generate_rectangle_vertex_probe import make_inputs,make_depth_inputs,depth_clear_bytes
from test_raw_shader_analysis import cf_alloc,cf_exec,alu,program
from test_raw_vertex_fetch_analysis import fetch


def rectangle(color=True):
    if color:
        return program([cf_exec(address=3,count=2,sequence=5,opcode=1),cf_alloc(1),
            cf_exec(address=5,opcode=1),cf_alloc(2),cf_exec(address=6),0],
            [fetch(dst=1,fmt=38,stride=8,swizzle=0x688),fetch(dst=2,fmt=38,stride=8,offset=4,swizzle=0x688),
             alu(dest=62,source=1),alu(dest=0,source=2)])
    return program([cf_exec(address=2,sequence=1,opcode=1),cf_alloc(1),cf_exec(address=3),0],
        [fetch(dst=1,fmt=38,stride=4,swizzle=0x688),alu(dest=62,source=1)])


def vertices(color=True):
    return b''.join(struct.pack('>4f',x,y,.75,1)+(struct.pack('>4f',.25,.5,1,1) if color else b'')
                   for x,y in ((-.5,-.5),(63.5,-.5),(63.5,31.5)))


class RectangleVertexTests(unittest.TestCase):
    def test_completion_and_explicit_mapping(self):
        source,proof=translate_rectangle_vertex(rectangle(),96,64,32)
        result=evaluate_rectangle(rectangle(),vertices(),64,32)
        self.assertEqual(result['clip_corners'],[[-1,-1,.75,1],[1,-1,.75,1],[1,1,.75,1],[-1,1,.75,1]])
        self.assertIn('readVertex(2u,c,cc)',source)
        self.assertIn('uint[6](0u,1u,2u,0u,2u,3u)',source)
        self.assertTrue(proof['whole_primitive_rejection_guaranteed'])
        self.assertFalse(proof['title_raster_state_replayed'])

    def test_actual_copy_pixel_or_explicit_synthetic_depth_color(self):
        ps=program([cf_alloc(),cf_exec()],[alu()])
        _,_,proof,_,expected=make_inputs(rectangle(),vertices(),64,32,ps)
        self.assertTrue(proof['captured_pixel_partner']);self.assertEqual(proof['reference_covered_pixels'],1024)
        self.assertEqual(expected[16:32],struct.pack('<4f',.25,.5,1,1))
        _,_,proof,_,expected=make_inputs(rectangle(False),vertices(False),64,32)
        self.assertFalse(proof['captured_pixel_partner']);self.assertEqual(expected[16:32],struct.pack('<4f',.75,.75,.75,1))
        with self.assertRaises(ValueError):make_inputs(rectangle(False),vertices(False),64,32,ps)
        with self.assertRaises(ValueError):passthrough_fragment(program([cf_alloc(),cf_exec()],[alu(vector=0,zero=True)]))

    def test_partial_pixel_grid_coverage_and_non_grid_reference_reject(self):
        data=bytearray(vertices());struct.pack_into('>f',data,32,31.5);struct.pack_into('>f',data,64,31.5)
        _,_,proof,_,expected=make_inputs(rectangle(),bytes(data),64,32)
        self.assertEqual(proof['reference_covered_pixels'],512)
        for y in range(32):self.assertEqual(expected[16+(y*32+16)*16:16+(y*32+32)*16],bytes([0x5A])*256)
        struct.pack_into('>f',data,32,31.25);struct.pack_into('>f',data,64,31.25)
        with self.assertRaises(ValueError):make_inputs(rectangle(),bytes(data),64,32)

    def test_all_vertices_reject_malformed_shape_color_depth(self):
        for at,value in ((0,float('nan')),(36,1),(64,60),(72,.5),(76,2),(12,0),(8,-1),(8,2),
                         (48,.5),(16,float('inf')),(16,2**-149),(0,-32768)):
            data=bytearray(vertices());struct.pack_into('>f',data,at,value)
            with self.subTest(at=at,value=value),self.assertRaises(ValueError):evaluate_rectangle(rectangle(),bytes(data),64,32)
        for source in (vertices()[:-4],bytes(32)):
            with self.assertRaises(ValueError):evaluate_rectangle(rectangle(),source,64,32)

    def test_bounded_explicit_profile(self):
        for length,w,h in ((0,64,32),(96,0,32),(96,True,32),(96,4097,32),(4097,64,32),(95,64,32)):
            with self.assertRaises(ValueError):translate_rectangle_vertex(rectangle(),length,w,h)

    def test_raw_subnormal_fetches_reject_before_float_decode(self):
        shader,proof=translate_rectangle_vertex(rectangle(),96,64,32)
        self.assertTrue(proof['raw_float_bits_guarded_before_decode'])
        self.assertFalse(proof['subnormal_fetch_supported'])
        self.assertLess(shader.index('fetch0_x_exponent=='),shader.index('uintBitsToFloat(fetch0_x_bits)'))
        # Every fetched component of every source vertex is checked, even if
        # the later shape/color check would otherwise conceal the bad input.
        for index in range(3):
            for lane in range(8):
                for bits in (1,0x007FFFFF,0x80000001,0x807FFFFF):
                    data=bytearray(vertices());struct.pack_into('>I',data,index*32+lane*4,bits)
                    with self.subTest(index=index,lane=lane,bits=bits),self.assertRaisesRegex(ValueError,'subnormal raw'):
                        evaluate_rectangle(rectangle(),bytes(data),64,32)
        # The smallest normal depth remains in the rectangle adapter domain.
        data=bytearray(vertices())
        for index in range(3):struct.pack_into('>I',data,index*32+8,0x00800000)
        self.assertEqual(evaluate_rectangle(rectangle(),bytes(data),64,32)['clip_corners'][0][2],2**-126)

    def test_negative_zero_color_reference_is_not_silently_normalized(self):
        data=bytearray(vertices())
        for i in range(3):struct.pack_into('>4f',data,i*32+16,-0.0,-0.0,-0.0,-0.0)
        result=evaluate_rectangle(rectangle(),bytes(data),64,32)
        self.assertEqual(struct.pack('>4f',*result['uniform_color']),struct.pack('>4I',*([0x80000000]*4)))
        with self.assertRaisesRegex(ValueError,'negative-zero'):make_inputs(rectangle(),bytes(data),64,32)

    def test_depth_stencil_reference_copies_have_separate_canaries(self):
        clear=depth_clear_bytes();self.assertEqual(len(clear),5168)
        _,_,proof,_,expected=make_depth_inputs(rectangle(),vertices(),64,32)
        self.assertTrue(proof['depth_attachment']);self.assertFalse(proof['pixel_shader_bound'])
        self.assertFalse(proof['xenos_edram_storage'])
        for start,end in ((0,16),(4112,4128),(5152,5168)):
            self.assertEqual(expected[start:end],bytes([0x5A])*(end-start))
        self.assertEqual(expected[16:4112],struct.pack('<f',.75)*1024)
        self.assertEqual(expected[4128:5152],bytes(1024))
        data=bytearray(vertices());struct.pack_into('>f',data,32,31.5);struct.pack_into('>f',data,64,31.5)
        _,_,proof,_,expected=make_depth_inputs(rectangle(),bytes(data),64,32)
        self.assertEqual(proof['reference_covered_pixels'],512)
        for y in range(32):
            self.assertEqual(expected[16+(y*32+16)*4:16+(y*32+32)*4],struct.pack('<f',.125)*16)
            self.assertEqual(expected[4128+y*32+16:4128+y*32+32],bytes([0xA5])*16)


if __name__=='__main__':unittest.main()
