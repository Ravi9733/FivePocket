from pathlib import Path
import math
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from vertex_arithmetic_dataflow import (analyze_vertex_dataflow,evaluate_vertex_dataflow,arithmetic,
    translate_vertex_arithmetic_compute)
from raw_shader_analysis import RawShaderError,analyze_copy_shader
from test_raw_shader_analysis import cf_alloc,cf_exec,alu,program
from test_raw_vertex_fetch_analysis import fetch


def math_alu(op=11,dest=62,mask=15,sources=(0,0,1),temps=(True,False,False),
             swizzles=(0,0,0),export=True):
    a=(50<<26)|dest|(mask<<16)|(int(export)<<15)
    b=sum(s<<(16-i*8) for i,s in enumerate(swizzles))
    c=(op<<24)|sum(s<<(16-i*8) for i,s in enumerate(sources))|sum(int(v)<<(31-i) for i,v in enumerate(temps))
    return a,b,c


def simple(instruction=None):
    return program([cf_exec(address=2,sequence=1,opcode=1),cf_alloc(1),cf_exec(address=3),0],
        [fetch(dst=0,fmt=38,stride=4,swizzle=0x688),instruction or math_alu()])


class VertexArithmeticTests(unittest.TestCase):
    def test_packed_vertex_log_ieee_reuses_bounded_scalar_math(self):
        options=dict(dot4=True,scalar_constants=True,loading_math=True,packed_geometry=True)
        instruction=((16<<26)|(15<<20)|(1<<15)|62,0,0)
        code=simple(instruction);source=struct.pack('>4f',2,3,4,5)
        proof=analyze_vertex_dataflow(code,**options)
        self.assertIn(('log2',proof['nodes'].index(('constant',0,3))),proof['nodes'])
        for value,expected in ((1,0),(2,1),(.5,-1),(16,4)):
            result=evaluate_vertex_dataflow(code,{0:source},{0:struct.pack('>4f',0,0,0,value)},0,**options)
            self.assertEqual(result['62'],[expected]*4)
        for value in (0.0,-0.0,-1.0,2**-149,float('inf'),float('nan')):
            with self.assertRaises(RawShaderError):
                evaluate_vertex_dataflow(code,{0:source},{0:struct.pack('>4f',0,0,0,value)},0,**options)
        for override in ({'packed_geometry':False},{'loading_math':False}):
            with self.assertRaises(RawShaderError):analyze_vertex_dataflow(code,**{**options,**override})
        # Do not admit LOG_CLAMP or unrelated pixel geometry scalar operations.
        for opcode in (8,10,11,15):
            edited=((instruction[0]&((1<<26)-1))|(opcode<<26),instruction[1],instruction[2])
            with self.assertRaises(RawShaderError):analyze_vertex_dataflow(simple(edited),**options)

    def test_packed_vertex_log_previous_result_is_not_destination_saturation(self):
        # LOG2(.25)=-2 is stored clamped to r0.x=0, but ADD_PREV must receive
        # the raw previous scalar -2, so 5+previous is3 rather than5.
        code=program([cf_exec(address=2,sequence=1,opcode=1),cf_alloc(1),cf_exec(address=3,count=2),0],
            [fetch(dst=0,fmt=38,stride=4,swizzle=0x688),
             ((16<<26)|(1<<25)|(1<<20),0,0),
             ((1<<26)|(15<<20)|(1<<15)|62,0,1)])
        result=evaluate_vertex_dataflow(code,{0:struct.pack('>4f',2,3,4,5)},
            {0:struct.pack('>4f',0,0,0,.25),1:struct.pack('>4f',0,0,0,5)},0,
            dot4=True,scalar_constants=True,loading_math=True,packed_geometry=True)
        self.assertEqual(result['62'],[3]*4)

    def test_unpacked_dot2add_reference_requires_the_explicit_existing_profile(self):
        code=simple(math_alu(op=17,mask=5,sources=(0,0,1)))
        source=struct.pack('>4f',2,3,100,200)
        constants={0:struct.pack('>4f',7,11,300,400),1:struct.pack('>4f',5,600,700,800)}
        options=dict(dot4=True,scalar_constants=True,loading_math=True,tonemap=True)
        # 2*7 + 3*11 + 5, replicated then write-masked. The other
        # source lanes must not contaminate this unpacked rectangle operation.
        self.assertEqual(evaluate_vertex_dataflow(code,{0:source},constants,0,**options)['62'],
                         [52,None,52,None])
        with self.assertRaises(RawShaderError):
            evaluate_vertex_dataflow(code,{0:source},constants,0,**{**options,'tonemap':False})
        for override in ({'tonemap':1},{'loading_math':False},{'dot4':False},
                         {'scalar_constants':False},{'packed_geometry':True},
                         {'predicate_profile':True},{'address_profile':True}):
            with self.assertRaises(RawShaderError):
                evaluate_vertex_dataflow(code,{0:source},constants,0,**{**options,**override})

    def test_ui_dot4_replicates_full_dot_before_masking(self):
        code=simple(math_alu(op=15,mask=5,sources=(0,0,0)))
        result=evaluate_vertex_dataflow(code,{0:struct.pack('>4f',2,3,4,5)},
            {0:struct.pack('>4f',7,11,13,17)},0,dot4=True)
        self.assertEqual(result['62'],[184,None,184,None])
        with self.assertRaises(RawShaderError):analyze_vertex_dataflow(code)

    def test_mad_constants_and_default_guards_unchanged(self):
        code=simple();source=struct.pack('>4f',2,3,4,5)
        constants={0:struct.pack('>4f',3,4,5,6),1:struct.pack('>4f',1,2,3,4)}
        self.assertEqual(evaluate_vertex_dataflow(code,{0:source},constants,0),{'62':[7,14,23,34]})
        proof=analyze_vertex_dataflow(code)
        self.assertEqual(proof['constant_registers'],[0,1])
        self.assertFalse(proof['allocation_extent_verified']);self.assertFalse(proof['gpu_executed'])
        with self.assertRaises(RawShaderError):analyze_copy_shader(code,'vertex',allow_vertex_fetch=True)

    def test_parallel_temporary_snapshot_and_relative_component_swizzles(self):
        code=program([cf_exec(address=2,sequence=1,opcode=1),cf_alloc(1),cf_exec(address=3,count=2),0],
            [fetch(dst=0,fmt=38,stride=4,swizzle=0x688),
             math_alu(op=1,dest=0,sources=(0,0,0),temps=(True,True,True),swizzles=(0x55,0,0),export=False),
             alu(dest=62)])
        self.assertEqual(evaluate_vertex_dataflow(code,{0:struct.pack('>4f',2,3,4,5)},{},0)['62'],[6,12,20,10])

    def test_partial_export_keeps_unwritten_lanes_unknown(self):
        code=simple(math_alu(op=1,mask=3))
        result=evaluate_vertex_dataflow(code,{0:struct.pack('>4f',2,3,4,5)},
            {0:struct.pack('>4f',1,1,1,1)},0)
        self.assertEqual(result['62'],[2,3,None,None])

    def test_missing_constants_input_bounds_and_changed_full_index_reject(self):
        source=struct.pack('>4f',2,3,4,5)
        for constants in ({},{0:bytes(16),1:bytes(15)},{0:bytes(16),1:struct.pack('>4f',float('inf'),0,0,0)}):
            with self.assertRaises(RawShaderError):evaluate_vertex_dataflow(simple(),{0:source},constants,0)
        code=program([cf_exec(address=2,count=2,sequence=5,opcode=1),cf_alloc(1),cf_exec(address=4),0],
            [fetch(dst=0,fmt=38,stride=4,swizzle=0x688),fetch(dst=1,fmt=38,stride=4,swizzle=0x688),alu(dest=62,source=1)])
        with self.assertRaises(RawShaderError):evaluate_vertex_dataflow(code,{0:source*3},{},0)
        for source,index in ((bytes(12),0),(bytes(16),1),(bytes(16),True),(bytes(4100),0)):
            with self.assertRaises(RawShaderError):evaluate_vertex_dataflow(simple(),{0:source},{},index)

    def test_math_profile_has_zero_rule_and_separate_rounding(self):
        self.assertEqual(struct.pack('>f',arithmetic('mul',-0.0,-5.0)),bytes(4))
        self.assertEqual(struct.pack('>f',arithmetic('add',0.0,-0.0)),bytes(4))
        a=1+2.0**-23;b=1-2.0**-23
        self.assertEqual(arithmetic('add',arithmetic('mul',a,b),-1.0),0.0)
        self.assertNotEqual(a*b-1.0,0.0)  # A fused/double policy would differ.
        for op,a,b in (('mul',2.0**-149,1),('mul',float('inf'),0),('add',float('nan'),0),
                       ('mul',2.0**127,4),('mul',2.0**-126,0.5),('div',1,2)):
            with self.assertRaises(RawShaderError):arithmetic(op,a,b)

    def test_control_alu_and_memory_exports_reject(self):
        for ins in (math_alu(op=0),math_alu(op=2,sources=(0,1,0),temps=(True,True,True)),
                    math_alu(dest=32),math_alu(sources=(64,0,1))):
            with self.assertRaises(RawShaderError):analyze_vertex_dataflow(simple(ins))
        base=math_alu()
        for w,b in ((0,6),(0,7),(0,24),(0,25),(1,24),(1,28),(1,29),(1,30),(1,31)):
            edited=list(base);edited[w]^=1<<b
            with self.assertRaises(RawShaderError):analyze_vertex_dataflow(simple(edited))
        for op in (3,4,7,9,14,15):
            with self.assertRaises(RawShaderError):analyze_vertex_dataflow(program([cf_alloc(1),cf_exec(opcode=op)],[alu(dest=62)]))
        with self.assertRaises(RawShaderError):analyze_vertex_dataflow(program([cf_alloc(3),cf_exec()],[alu(dest=32)]))

    def test_compute_profile_is_bounded_and_unfused(self):
        source,proof=translate_vertex_arithmetic_compute(simple(),64)
        self.assertIn('precise float n',source)
        self.assertIn('if(index>=4u',source)
        self.assertIn('result.words.length()<14',source)
        self.assertIn('source.words.length()<24',source)
        self.assertEqual(proof['input_bytes'],96);self.assertEqual(proof['output_bytes'],224)
        self.assertFalse(proof['native_vertex_pipeline']);self.assertFalse(proof['allocation_extent_verified'])
        self.assertNotIn('fma(',source)
        for length in (0,3,4096,True):
            with self.assertRaises(RawShaderError):translate_vertex_arithmetic_compute(simple(),length)
        with self.assertRaises(RawShaderError):translate_vertex_arithmetic_compute(simple(math_alu(mask=3)),64)


if __name__=='__main__':unittest.main()
