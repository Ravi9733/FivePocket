from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from raw_shader_analysis import analyze_copy_shader,evaluate_float_vertex_copy,translate_float_vertex_copy_compute,RawShaderError
from test_raw_shader_analysis import program,cf_alloc,cf_exec,alu


def fetch(*,dst=1,src=0,slot=0,stride=7,offset=0,fmt=57,swizzle=0xA88,mini=False):
    return ((src<<5)|(dst<<12)|(1<<19)|((slot//3)<<20)|((slot%3)<<25),
        swizzle|0x3000|(fmt<<16)|(int(mini)<<30),(offset<<8)|stride)


def pair(first=None,second=None):
    return program([cf_exec(address=2,count=2,sequence=5,opcode=1)|(3<<28),cf_alloc(2),
                    cf_alloc(1),cf_exec(address=4,count=2)],
        [first or fetch(),second or fetch(dst=0,fmt=38,offset=3,swizzle=0x688,stride=0,mini=True),
         alu(source=0),alu(source=1,dest=62)])


class RawVertexFetchTests(unittest.TestCase):
    def test_default_rejects_and_explicit_dataflow_preserves_mini_inheritance(self):
        data=pair()
        with self.assertRaises(RawShaderError):analyze_copy_shader(data,'vertex')
        with self.assertRaises(RawShaderError):analyze_copy_shader(data,'pixel',allow_vertex_fetch=True)
        p=analyze_copy_shader(data,'vertex',allow_vertex_fetch=True)
        self.assertEqual(p['exports'],{'0':['fetch1.'+c for c in 'xyzw'],
                                      '62':['fetch0.x','fetch0.y','fetch0.z','1']})
        self.assertEqual([f['stride_words'] for f in p['vertex_fetches']],[7,7])
        self.assertFalse(p['gpu_executed']);self.assertFalse(p['resource_bytes_read'])
        # The mini's own source/constant/stride are not used. Full fetch writes
        # r0, but mini must retain the original index, not read fetched r0.x.
        data=pair(fetch(dst=0),fetch(dst=1,src=63,slot=95,stride=255,offset=3,
                                    fmt=38,swizzle=0x688,mini=True))
        p=analyze_copy_shader(data,'vertex',allow_vertex_fetch=True)
        self.assertEqual(p['vertex_fetches'][1]['index_expression'],'initial_r0.x')
        self.assertEqual(p['vertex_fetches'][1]['fetch_slot'],0)
        self.assertEqual(p['vertex_fetches'][1]['stride_words'],7)

    def test_finite_buffer_and_bounds_for_each_vertex(self):
        values=[-0.5,-0.5,0.75,0,0,0,1, 639.5,-0.5,0.75,0,0,0,1,
                639.5,359.5,0.75,0,0,0,1]
        data=pair();buffer=struct.pack('>21f',*values)
        for i in range(3):
            out=evaluate_float_vertex_copy(data,{0:buffer},i)
            self.assertEqual(out['62'],values[i*7:i*7+3]+[1.0])
            self.assertEqual(out['0'],[0.,0.,0.,1.])
        for i in (-1,3,1<<24,True,0.5):
            with self.assertRaises(RawShaderError):evaluate_float_vertex_copy(data,{0:buffer},i)
        for source in (b'',buffer[:-1],buffer[:24],bytearray(buffer),b'0'*4100):
            with self.assertRaises(RawShaderError):evaluate_float_vertex_copy(data,{0:source},0)
        with self.assertRaises(RawShaderError):evaluate_float_vertex_copy(data,{},0)
        for nonfinite in (float('nan'),float('inf'),float('-inf')):
            invalid=struct.pack('>f',nonfinite)+buffer[4:]
            with self.assertRaises(RawShaderError):evaluate_float_vertex_copy(data,{0:invalid},0)

    def test_unsupported_instruction_fields_do_not_get_ignored(self):
        base=fetch()
        for word,bit in ((0,0),(0,11),(0,18),(0,19),(1,14),(1,15),(1,22),(1,24),(1,31),(2,30),(2,31)):
            mutated=list(base);mutated[word]^=1<<bit
            with self.assertRaises(RawShaderError):analyze_copy_shader(pair(mutated),'vertex',allow_vertex_fetch=True)
        for first in (fetch(mini=True),fetch(stride=0),fetch(fmt=6),fetch(swizzle=0x688),
                      fetch(swizzle=0xC88)):
            with self.assertRaises(RawShaderError):analyze_copy_shader(pair(first),'vertex',allow_vertex_fetch=True)
        bad=list(base);bad[0]|=3<<25
        with self.assertRaises(RawShaderError):analyze_copy_shader(pair(bad),'vertex',allow_vertex_fetch=True)

    def test_float_formats_zero_one_keep_and_unresolved_exports(self):
        for fmt,components,swiz in ((36,1,0xB60),(37,2,0xB48),(38,4,0x688),(57,3,0xA88)):
            data=program([cf_exec(address=2,sequence=1,opcode=1),cf_alloc(1),cf_exec(address=3),0],
                [fetch(dst=0,fmt=fmt,swizzle=swiz,stride=components),alu(dest=62)])
            buffer=struct.pack('>'+str(components)+'f',*range(1,components+1))
            result=evaluate_float_vertex_copy(data,{0:buffer},0)['62']
            self.assertEqual(len(result),4)
            self.assertEqual(result[0],1.)
        # Keep preserves unknown initial components; do not initialize them to0.
        data=program([cf_exec(address=2,sequence=1,opcode=1),cf_alloc(1),cf_exec(address=3),0],
            [fetch(dst=0,fmt=37,swizzle=0xFC8,stride=2),alu(dest=62)])
        with self.assertRaises(RawShaderError):evaluate_float_vertex_copy(data,{0:struct.pack('>2f',2,3)},0)

    def test_compute_is_explicit_bounded_probe_not_vertex_pipeline(self):
        source,proof=translate_float_vertex_copy_compute(pair())
        self.assertIn('layout(local_size_x=1)',source)
        self.assertIn('source.words.length()',source)
        self.assertIn('result.words[1u+index*2u]',source)
        self.assertIn('result.words[2u+index*2u]',source)
        self.assertIn('isnan(fetch0_x) || isinf(fetch0_x)',source)
        self.assertTrue(proof['finite_input_required'])
        self.assertFalse(proof['native_vertex_pipeline']);self.assertFalse(proof['frame_presented'])
        self.assertEqual(proof['maximum_invocations'],3)
        with self.assertRaises(RawShaderError):translate_float_vertex_copy_compute(pair(fetch(slot=1)))

if __name__=='__main__':unittest.main()
