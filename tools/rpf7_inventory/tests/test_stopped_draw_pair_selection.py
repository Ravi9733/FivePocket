from copy import deepcopy
import hashlib
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch
sys.path[:0]=[str(Path(__file__).resolve().parents[2]/'vulkan_probe'),
    str(Path(__file__).resolve().parents[2]/'host_guest_trace')]
import generate_stopped_draw_pair as selection


def fixture(small=False):
    physical=[]
    def resource(slot,address,data,layout,descriptor):
        digest=hashlib.sha256(data).hexdigest()
        physical.append(({'physical_address':address,'byte_count':len(data),'sha256':digest},data))
        return {'slot':slot,'layout_supported':True,'layout':dict(layout,physical_address=address,source_bytes=len(data)),
                'descriptor':[{'fully_known':True,'value_bits':v} for v in descriptor],
                'snapshot':{'available':True,'slice_sha256':digest}}
    vb=resource(95,0x1000,bytes(range(144)),{'reserved_bits':0x10000000},[0x1003,0x10000092])
    textures=[]
    if small:
        textures.append(resource(2,0x2000,bytes([255])*4096,{},[0x80400002,0x2086,0x3E01F,0xA80C14,3,0x210]))
    else:
        for i,(address,w,h,pitch) in enumerate(((0x10000,1280,720,1280),(0x100000,640,360,768),(0x180000,640,360,768))):
            row=bytes([i])*w+bytes([199])*(pitch-w)
            textures.append(resource(i,address,row*h,{},[(pitch//32)<<22|0x24802,address|2,(h-1)<<13|(w-1),0x281400,3,0x210]))
    shaders=[({'stage':0},b'vertex'),({'stage':1},b'pixel')]
    stage=lambda i,resources:{'load':shaders[i][0],'resource_candidates':resources,
        'constant_candidates':[{'constant':i,'words':[{'fully_known':True,'value_bits':v+i} for v in range(4)]}]}
    draw={'node':1,'offset':12,'index_count':4,'primitive_bits':6,'predicate_requested':True,'predicate_result':None,
        'register_candidates':{str(0x2208):{'fully_known':True,'value_bits':4}},
        'last_shader_load_candidates':{'0':stage(0,[vb]),'1':stage(1,textures)}}
    return {'draws':[draw],'report_sha256':'fixture-report-hash'},shaders,physical


class PairSelectionTests(unittest.TestCase):
    def test_r8_order_padding_constants_and_predicate_are_preserved(self):
        m,s,p=fixture()
        with patch.object(selection,'r8_pair',return_value='translated') as translator:
            result,proof=selection.select_inputs(m,s,p,1,12)
        self.assertEqual(result,'translated');args=translator.call_args.args
        self.assertEqual(args[:3],(b'vertex',b'pixel',bytes(range(144))))
        self.assertEqual(args[3:5],(struct.pack('>4I',0,1,2,3),struct.pack('>4I',1,2,3,4)))
        self.assertEqual(args[5],[bytes(921600),bytes([1])*230400,bytes([2])*230400])
        self.assertTrue(proof['predicate_requested']);self.assertIsNone(proof['predicate_result'])
        self.assertFalse(proof['title_draw_executed'])
        self.assertEqual(proof['native_probe_profile'],'r8_planes_vertex_pair_test')

    def test_small_texture_decodes_in_selected_profile(self):
        m,s,p=fixture(small=True)
        with patch.object(selection,'small_pair',return_value='translated') as translator:
            _,proof=selection.select_inputs(m,s,p,1,12)
        self.assertEqual(translator.call_args.args[5],bytes([255])*4096)
        self.assertEqual(proof['native_probe_profile'],'texture32_vertex_pair_test')

    def test_depth_resolve_missing_or_duplicate_draws_reject(self):
        for mode in (0,5,6):
            m,s,p=fixture();m['draws'][0]['register_candidates'][str(0x2208)]['value_bits']=mode
            with self.assertRaises(ValueError):selection.select_inputs(m,s,p,1,12)
        m,s,p=fixture()
        for node,offset in ((1,13),(-1,12),(True,12)):
            with self.assertRaises(ValueError):selection.select_inputs(m,s,p,node,offset)
        m['draws']*=2
        with self.assertRaises(ValueError):selection.select_inputs(m,s,p,1,12)

    def test_missing_stale_and_unknown_dependencies_reject(self):
        for change in ('constant','shader','resource','hash','slot','reserved','texture_order','missing_stage'):
            m,s,p=fixture();stages=m['draws'][0]['last_shader_load_candidates']
            if change=='constant':stages['1']['constant_candidates'][0]['words'][0]['fully_known']=False
            elif change=='shader':s=[]
            elif change=='resource':p=p[1:]
            elif change=='hash':stages['0']['resource_candidates'][0]['snapshot']['slice_sha256']='0'*64
            elif change=='slot':stages['0']['resource_candidates'][0]['slot']=94
            elif change=='reserved':stages['0']['resource_candidates'][0]['layout']['reserved_bits']=0
            elif change=='texture_order':stages['1']['resource_candidates'].reverse()
            elif change=='missing_stage':del stages['1']
            with self.subTest(change=change),self.assertRaises(ValueError):selection.select_inputs(m,s,p,1,12)


if __name__=='__main__':unittest.main()
