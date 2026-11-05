"""Authored multi-export shader dataflow; not current game shader evidence."""
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from inline_shader_translation import translate_inline_pair
from pixel_sample_dataflow import evaluate_pixel_sample_dataflow
from raw_shader_analysis import RawShaderError
from test_raw_shader_analysis import program,cf_exec,cf_alloc,alu
from test_geometry_shader_translation import vertex,OPTIONS
from test_pixel_sample_dataflow import texture

def mrt_pixel(slots=(0,1,2,3)):
    return program([cf_alloc(size=4),cf_exec(count=len(slots))],
        [alu(dest=i,vector=i+1,scalar=i+1,zero=True) for i in slots])

def mrt_textured_pixel():
    return program([cf_exec(address=2,count=4,sequence=85,opcode=1),cf_alloc(size=4),
        cf_exec(address=6,count=4),0],
        [*(texture(src=0,dst=4+i,slot=s) for i,s in enumerate((5,6,3,4))),
         *(alu(dest=i,source=4+i) for i in range(4))])

class MrtShaderTranslationTests(unittest.TestCase):
    def test_all_written_output_subsets_preserve_slot_and_depth_once(self):
        for mask in range(1,16):
            slots=tuple(i for i in range(4) if mask&(1<<i));ps=mrt_pixel(slots)
            vs,fs,p=translate_inline_pair(vertex(),ps,**OPTIONS,geometry_mrt_formats=(0,1,0,1))
            self.assertEqual(p['abi_version'],14);self.assertEqual(p['geometry_color_outputs'],list(slots))
            self.assertTrue(p['geometry_depth20e4']);self.assertFalse(p['geometry_sample_depth'])
            self.assertFalse(p['geometry_resources_verified']);self.assertFalse(p['geometry_numeric_domain_verified'])
            self.assertIn('geometryStoreSampleDepth(viewport.depthLimits)',vs)
            self.assertEqual(fs.count('geometryStoreMrtDepth20e4();'),1)
            self.assertEqual(fs.count('atomicAdd(geometryCompletion.words[5],1u)'),1)
            self.assertNotIn('gl_SampleID','\n'.join(line.split('//')[0] for line in fs.splitlines()))
            self.assertNotIn('interpolateAtSample(',fs)
            for i in range(4):
                if i in slots:
                    self.assertIn(f'layout(location={i}) out vec4 color{i};',fs)
                    self.assertIn(f'geometryStoreMrtColor8(color{i},{i%2}u);',fs)
                else:self.assertNotIn(f'color{i}',fs)
            values=evaluate_pixel_sample_dataflow(ps,{}, {}, {},ui_operations=True,
                extended_math=True,loading_math=True,geometry_profile=True)
            self.assertEqual(values,{str(i):[float(bool((i+1)&(1<<c))) for c in range(4)] for i in slots})

    def test_four_independent_textures_and_pixel_centroid_interface(self):
        _,fs,p=translate_inline_pair(vertex(),mrt_textured_pixel(),**OPTIONS,
            geometry_mrt_formats=(0,0,0,0),geometry_centroid_mask=1)
        self.assertEqual(p['texture_slots'],[5,6,3,4]);self.assertEqual(fs.count('=texture('),4)
        self.assertIn('layout(location=0) centroid in vec4 packedInterpolator0;',fs)
        self.assertNotIn('interpolateAtCentroid(',fs);self.assertNotIn('interpolateAtOffset(',fs)
        for i in range(4):
            self.assertIn(f'color{i}=vec4(',fs)
            for lane,n in enumerate(p['pixel']['exports'][str(i)]):
                node=p['pixel']['nodes'][n]
                self.assertEqual(node[0],'maximum') # Original MAX of the same input, not a copy opcode.
                self.assertEqual(p['pixel']['nodes'][node[1]],('sample',i,lane))
                self.assertEqual(p['pixel']['nodes'][node[2]],('sample',i,lane))

    def test_completion_binding_cannot_alias_a_fifth_texture(self):
        code=program([cf_exec(address=2,count=5,sequence=341,opcode=1),cf_alloc(),
            cf_exec(address=7),0],[*(texture(src=0,dst=4+i,slot=i) for i in range(5)),alu(source=8)])
        with self.assertRaisesRegex(RawShaderError,'binding5'):
            translate_inline_pair(vertex(),code,**OPTIONS,geometry_mrt_formats=(0,0,0,0))

    def test_unknown_output_lane_or_unbound_attachment_is_not_zero_filled(self):
        for slot in range(4):
            missing=program([cf_alloc(size=4),cf_exec()],[alu(dest=slot,vector=1)])
            with self.assertRaises(RawShaderError):translate_inline_pair(vertex(),missing,**OPTIONS,geometry_mrt_formats=(0,0,0,0))
            formats=tuple(None if i==slot else 0 for i in range(4))
            with self.assertRaises(RawShaderError):translate_inline_pair(vertex(),mrt_pixel(),**OPTIONS,geometry_mrt_formats=formats)
        _,fs,_=translate_inline_pair(vertex(),mrt_pixel((3,)),**OPTIONS,geometry_mrt_formats=(None,None,None,0))
        self.assertIn('out vec4 color3;',fs);self.assertNotIn('out vec4 color0;',fs)

    def test_legacy_admission_and_profiles_remain_distinct(self):
        for opts in ({},OPTIONS,{**OPTIONS,'geometry_color8_format':0}):
            with self.assertRaises(RawShaderError):translate_inline_pair(vertex(),mrt_pixel(),**opts)
        for extra in ({'geometry_compute':True},{'geometry_profile':False},{'geometry_color8_format':0},
                      {'color8_source_clamp':True},{'indexed':False}):
            with self.assertRaises(RawShaderError):translate_inline_pair(vertex(),mrt_pixel(),
                **{**OPTIONS,**extra},geometry_mrt_formats=(0,0,0,0))
        for formats in ((),(0,),[0,0,0,0],(None,)*4,(0,0,0,True),(0,0,0,2),(0,0,0,'gamma')):
            with self.assertRaises(RawShaderError):translate_inline_pair(vertex(),mrt_pixel(),**OPTIONS,geometry_mrt_formats=formats)

if __name__=='__main__':unittest.main()
