"""Authored bytecode and graph mutations, never embedded game shaders."""
import copy
import struct
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from exposure_scalar import (VERTEX_OPTIONS, PIXEL_OPTIONS, r32_single_texel_binding,
    validate_position_exports, validate_pixel_graph, exposure_fragment, translate_exposure_scalar)
from vertex_arithmetic_dataflow import analyze_vertex_dataflow, evaluate_vertex_dataflow
from pixel_sample_dataflow import analyze_pixel_sample_dataflow, evaluate_analyzed_pixel_dataflow
from raw_shader_analysis import RawShaderError
from test_raw_shader_analysis import program, cf_exec, cf_alloc, alu
from test_raw_vertex_fetch_analysis import fetch
from test_vertex_arithmetic_dataflow import math_alu
from test_pixel_sample_dataflow import texture

FETCH = (0x80404956, 0x010000A4, 0, 0x16D1, 0, 0x210)


def vertex():
    dead_texture = (1 | (1 << 19) | (17 << 20) | (1 << 28), 0x0F1FFE3F, 0x4000)
    return program([cf_exec(address=3, count=2, sequence=5, opcode=1), cf_alloc(1),
        cf_exec(address=5, opcode=1), cf_alloc(2), cf_exec(address=6, sequence=1), 0],
        [fetch(dst=1, slot=95, stride=6),
         fetch(dst=0, slot=95, stride=6, offset=4, fmt=37, swizzle=0xFC8, mini=True),
         alu(dest=62, source=1), dead_texture])


def pixel():
    t = list(texture(src=0, dst=0, slot=0, swizzle=0)); t[0] &= ~(3 << 28)
    return program([cf_exec(address=2, opcode=1), cf_alloc(),
        cf_exec(address=3, count=2, sequence=1), 0],
        [math_alu(op=2, dest=0, sources=(255,255,0), temps=(False,False,True),
                  swizzles=(0x6C,0x6C,0), export=False), tuple(t),
         math_alu(op=2, dest=0, sources=(0,0,0), temps=(True,True,True))])


class ExposureScalarTests(unittest.TestCase):
    def test_original_dead_texture_has_no_export_dependency(self):
        proof = analyze_vertex_dataflow(vertex(), **VERTEX_OPTIONS)
        live = validate_position_exports(proof)
        self.assertEqual(live['eliminated_texture_reads'], 1)
        self.assertFalse(any(proof['nodes'][i][0] == 'texture_sample' for i in live['export_nodes']))
        data = struct.pack('>6f', -.5, .5, .25, float('nan'), 0., 0.)
        # The NaN padding is not fetched. Position evaluation needs no dummy
        # texture result: the graph proves that result does not reach position.
        out = evaluate_vertex_dataflow(vertex(), {95:data}, {}, 0,
            **VERTEX_OPTIONS, position_only=True)
        self.assertEqual(out['62'], [-.5, .5, .25, 1.])

    def test_texture_in_position_or_another_export_cannot_be_eliminated(self):
        p = analyze_vertex_dataflow(vertex(), **VERTEX_OPTIONS)
        sample = next(i for i,n in enumerate(p['nodes']) if n[0] == 'texture_sample')
        changed = copy.deepcopy(p); changed['exports']['62'][0] = sample
        with self.assertRaises(RawShaderError): validate_position_exports(changed)
        changed = copy.deepcopy(p); changed['exports']['0'] = [sample]*4
        with self.assertRaises(RawShaderError): validate_position_exports(changed)

    def test_changed_position_layout_and_undefined_lane_refuse(self):
        p = analyze_vertex_dataflow(vertex(), **VERTEX_OPTIONS)
        for field,value in (('stride_words',7),('fetch_slot',0),('format',38),('offset_words',1)):
            q=copy.deepcopy(p); q['fetches'][0][field]=value
            with self.assertRaises(RawShaderError): validate_position_exports(q)
        p['exports']['62'][3]=None
        with self.assertRaises(RawShaderError): validate_position_exports(p)

    def test_dynamic_constant_and_numeric_sample_not_embedded(self):
        vs, ps, proof = translate_exposure_scalar(vertex(), pixel(), FETCH)
        self.assertIn('originalPosition', vs)
        self.assertNotIn('sampler2D', vs)
        self.assertIn('uintBitsToFloat(readBE(2048u))', ps)
        self.assertEqual(ps.count('textureLod('), 1)
        self.assertFalse(proof['native_game_pipeline'])
        for value in (0., 1., -4., 2.5):
            out=evaluate_analyzed_pixel_dataflow(proof['pixel'], {},
                {255:struct.pack('>4f', .5, 0, 0, 0)}, {0:[value]*4})
            self.assertEqual(out['0'], [value]*4)

    def test_pixel_side_effects_initial_reads_and_extra_samples_refuse(self):
        p = analyze_pixel_sample_dataflow(pixel(), **PIXEL_OPTIONS)
        changes=[]
        q=copy.deepcopy(p); q['kills']=[{}]; changes.append(q)
        q=copy.deepcopy(p); q['exports']['0'][0]=0; changes.append(q)
        q=copy.deepcopy(p); q['fetches']*=2; changes.append(q)
        q=copy.deepcopy(p); q['fetches'][0]['slot']=1; changes.append(q)
        q=copy.deepcopy(p); q['nodes'].append(('add',0,0)); changes.append(q)
        q=copy.deepcopy(p); q['nodes'].append(('sine',256)); changes.append(q)
        for q in changes:
            with self.assertRaises(RawShaderError): validate_pixel_graph(q)

    def test_single_texel_binding_rejects_near_misses(self):
        self.assertEqual(r32_single_texel_binding(FETCH)['source_bytes'],4096)
        for index,bit in ((0,2),(0,22),(1,0),(1,6),(2,0),(3,19),(4,0),(5,4)):
            q=list(FETCH); q[index]^=1<<bit
            with self.assertRaises(RawShaderError): r32_single_texel_binding(q)
        for address in (0,0x20000000,0xA0000000):
            q=list(FETCH); q[1]=address|0xA4
            with self.assertRaises(RawShaderError): r32_single_texel_binding(q)

    def test_expression_graph_retains_order_and_comparison_ties(self):
        p=analyze_pixel_sample_dataflow(pixel(), **PIXEL_OPTIONS)
        red=next(i for i,n in enumerate(p['nodes']) if n[0]=='sample')
        for op in ('log2','exp2','absolute','negate'):
            p['nodes'].append((op,red)); red=len(p['nodes'])-1
        other=p['nodes'].index(('constant',255,0))
        for op in ('mul','add','maximum','minimum','greater'):
            p['nodes'].append((op,red,other)); red=len(p['nodes'])-1
        p['exports']['0']=[red]*4
        text=exposure_fragment(p)
        self.assertIn('precise float',text)
        self.assertIn('>=',text)
        self.assertIn('?1.0:0.0',text)
        self.assertNotIn('fma(',text)


if __name__ == '__main__': unittest.main()
