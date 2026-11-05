from pathlib import Path
import math
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from packed_geometry_fetch import decode_packed_geometry_fetch,read_packed_geometry_fetch,unpack_geometry_word
from raw_shader_analysis import RawShaderError,_float_vertex_fetch
from unorm8_vertex_fetch import f32
from vertex_arithmetic_dataflow import analyze_vertex_dataflow,evaluate_vertex_dataflow,translate_vertex_arithmetic_compute
from test_raw_vertex_fetch_analysis import fetch
from test_raw_shader_analysis import program,cf_exec,cf_alloc,alu


def instruction(fmt=7,**kwargs):
    a,b,c=fetch(fmt=fmt,swizzle=kwargs.pop('swizzle',0xB08 if fmt==31 else 0x688),**kwargs)
    return a,(b&~0x3000)|(0x1000 if fmt==7 else 0x3000),c


class PackedGeometryFetchTests(unittest.TestCase):
    def test_half4_two_words_all_lanes_extent_and_connected_exports(self):
        plan,_=decode_packed_geometry_fetch(instruction(32,stride=2))
        source=bytes.fromhex('c0003c0080000001')
        self.assertEqual(read_packed_geometry_fetch(plan,source,0),[1,-2,2**-24,-0.0])
        self.assertEqual(struct.pack('>f',read_packed_geometry_fetch(plan,source,0)[3]),bytes.fromhex('80000000'))
        for lane in range(4):
            for half in (0,1,0x3c00,0x7bff,0x8000,0xfbff,0x7c00,0xffff):
                words=[0,0];words[lane//2]=half<<((lane%2)*16)
                data=struct.pack('>II',*words)
                if half&0x7c00==0x7c00:
                    with self.assertRaises(RawShaderError):read_packed_geometry_fetch(plan,data,0)
                else:
                    result=read_packed_geometry_fetch(plan,data,0)
                    self.assertEqual(struct.pack('>f',result[lane]),
                        struct.pack('>f',struct.unpack('>e',half.to_bytes(2,'big'))[0]))
        for data,index in ((source[:4],0),(source,1)):
            with self.assertRaises(RawShaderError):read_packed_geometry_fetch(plan,data,index)
        code=program([cf_exec(address=2,count=1,sequence=1,opcode=1),cf_alloc(2),
                      cf_alloc(1),cf_exec(address=3,count=2)],
            [instruction(32,dst=1,stride=2),alu(dest=62,source=1),alu(dest=0,source=1)])
        result=evaluate_vertex_dataflow(code,{0:source},{},0,packed_geometry=True)
        self.assertEqual(result['62'],[1,-2,2**-24,-0.0]);self.assertEqual(result['0'],result['62'])
        with self.assertRaises(RawShaderError):analyze_vertex_dataflow(code)
        with self.assertRaises(RawShaderError):evaluate_vertex_dataflow(code,{0:source[:4]},{},0,packed_geometry=True)

    def test_all_snorm_patterns_and_low_bits_first_lanes(self):
        for lane,(shift,width) in enumerate(((0,10),(10,10),(20,10),(30,2))):
            for raw in range(1<<width):
                signed=raw if raw<(1<<(width-1)) else raw-(1<<width)
                expected=[0.0]*4
                expected[lane]=max(-1.0,f32(signed*f32(1/((1<<(width-1))-1))))
                self.assertEqual(unpack_geometry_word(raw<<shift,7),expected)
        self.assertEqual(unpack_geometry_word(511|(512<<10)|(1<<30),7),[1,-1,0,1])

    def test_all_finite_half_patterns_and_extended_range_rejection(self):
        for raw in range(65536):
            for lane in (0,1):
                if raw&0x7C00==0x7C00:
                    with self.assertRaisesRegex(RawShaderError,'extended exponent'):
                        unpack_geometry_word(raw<<(lane*16),31)
                else:
                    expected=[0.0,0.0];expected[lane]=struct.unpack('>e',raw.to_bytes(2,'big'))[0]
                    got=unpack_geometry_word(raw<<(lane*16),31)
                    self.assertEqual(struct.pack('>2f',*got),struct.pack('>2f',*expected))
        self.assertEqual(unpack_geometry_word(0xC0003C00,31),[1,-2])

    def test_mini_saved_index_and_swizzle_keep(self):
        regs=[[f'initial_r{r}.{c}' for c in 'xyzw'] for r in range(64)]
        _,previous=_float_vertex_fetch(fetch(dst=0,slot=95,stride=7),None,regs,0)
        regs[0][0]='overwritten'
        plan,saved=decode_packed_geometry_fetch(instruction(31,src=63,slot=0,stride=0,offset=5,
            mini=True,swizzle=0xA0F),previous,regs,1)
        self.assertEqual(saved,previous);self.assertEqual(plan['index_expression'],'initial_r0.x')
        self.assertEqual((plan['fetch_slot'],plan['stride_words']),(95,7))
        self.assertEqual(read_packed_geometry_fetch(plan,bytes(20)+bytes.fromhex('c0003c00'),0),[None,-2,1,1])
        with self.assertRaises(RawShaderError):decode_packed_geometry_fetch(instruction(mini=True))

    def test_modifiers_formats_and_input_bounds_reject(self):
        for fmt in (7,31,32):
            base=instruction(fmt,stride=1)
            for word,bit in ((0,0),(0,11),(0,18),(0,19),(1,12),(1,13),(1,14),(1,15),
                             (1,22),(1,24),(1,31),(2,30),(2,31)):
                bad=list(base);bad[word]^=1<<bit
                with self.assertRaises(RawShaderError):decode_packed_geometry_fetch(bad)
            plan,_=decode_packed_geometry_fetch(base)
            for data,index in ((b'',0),(b'a',0),(bytes(4),1),(bytes(4),True),(bytes(4),-1),
                               (bytes(4),1<<24),(bytes(256*1024+4),0)):
                with self.assertRaises(RawShaderError):read_packed_geometry_fetch(plan,data,index)
        for words in ((0,0),(-1,0,0),(0,1<<32,0)):
            with self.assertRaises(RawShaderError):decode_packed_geometry_fetch(words)
        with self.assertRaises(RawShaderError):decode_packed_geometry_fetch(instruction(31,swizzle=0x688))
        plan,_=decode_packed_geometry_fetch(instruction(31,stride=1,swizzle=0xFFF))
        with self.assertRaises(RawShaderError):read_packed_geometry_fetch(plan,bytes(4),0,[math.inf,0,0,0])

    def test_connected_shader_copy_exports_and_default_admission_unchanged(self):
        code=program([cf_exec(address=2,count=2,sequence=5,opcode=1),cf_alloc(2),
                      cf_alloc(1),cf_exec(address=4,count=2)],
            [instruction(7,dst=0,stride=2),instruction(31,dst=1,mini=True,stride=0,offset=1),
             alu(dest=62,source=0),alu(dest=0,source=1)])
        data=struct.pack('>II',511|(512<<10)|(1<<30),0xC0003C00)
        result=evaluate_vertex_dataflow(code,{0:data},{},0,packed_geometry=True)
        self.assertEqual(result,{'0':[1,-2,0,1],'62':[1,-1,0,1]})
        proof=analyze_vertex_dataflow(code,packed_geometry=True)
        self.assertEqual([f['format'] for f in proof['fetches']],[7,31])
        self.assertTrue(proof['packed_geometry']);self.assertFalse(proof['gpu_executed'])
        self.assertFalse(proof['hardware_float_edge_cases_validated'])
        self.assertFalse(proof['half_extended_range_supported'])
        with self.assertRaises(RawShaderError):analyze_vertex_dataflow(code)
        with self.assertRaises(RawShaderError):translate_vertex_arithmetic_compute(code,8)
        with self.assertRaises(RawShaderError):evaluate_vertex_dataflow(code,{0:data[:4]},{},0,packed_geometry=True)
        with self.assertRaises(RawShaderError):evaluate_vertex_dataflow(code,{0:data[:4]+bytes.fromhex('7c003c00')},{},0,packed_geometry=True)


if __name__=='__main__':unittest.main()
