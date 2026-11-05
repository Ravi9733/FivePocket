#!/usr/bin/env python3
"""Captured rectangle vertex code with explicit test expansion/pixel mapping."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
sys.path[:0]=[str(Path(__file__).resolve().parents[1]/'host_guest_trace'),
    str(Path(__file__).resolve().parents[1]/'rpf7_inventory')]
from stopped_draw_dependencies import load_dependencies,_physical_match
from rectangle_vertex_stage import translate_rectangle_vertex,evaluate_rectangle,passthrough_fragment
from generate_vertex_dataflow_probe import compile_compute
from rpf7_extract import _write_exclusive
from xenos_preflight_batch import _require_private_report,_require_validator,repository_root


def make_inputs(code,source,width,height,pixel_code=None):
    vertex,proof=translate_rectangle_vertex(code,len(source),width,height)
    exports=evaluate_rectangle(code,source,width,height)
    fragment=passthrough_fragment(pixel_code)
    if pixel_code is not None and not exports['source_color_export_present']:
        raise ValueError('captured pixel shader requires captured parameter0, not synthetic color')
    if any(v==0 and math.copysign(1,v)<0 for v in exports['uniform_color']):
        raise ValueError('negative-zero interpolated color lacks a bit-exact native reference')
    expected=bytearray([0x5A]*16416);left,top,right,bottom=exports['source_bounds'];covered=0
    if any(((edge+.5)*32/size)%1 for edge,size in ((left,width),(right,width),(top,height),(bottom,height))):
        raise ValueError('exact native pixel-grid-aligned reference edges required')
    for y in range(32):
        for x in range(32):
            px=(x+.5)*width/32-.5;py=(y+.5)*height/32-.5
            if left<=px<right and top<=py<bottom:
                struct.pack_into('<4f',expected,16+(y*32+x)*16,*exports['uniform_color']);covered+=1
    return vertex,fragment,dict(proof,reference=exports,reference_covered_pixels=covered,output_bytes=16416,
        native_viewport=[0,0,32,32],depth_attachment=False,stencil_test=False,
        captured_pixel_partner=pixel_code is not None,pixel_sha256=hashlib.sha256(pixel_code).hexdigest() if pixel_code else None,
        reference_policy='pixel-center-half-open-axis-aligned-rectangle',rectangle_hardware_semantics_verified=False),source,bytes(expected)


def depth_clear_bytes():
    return bytes([0x5A])*16+struct.pack('<f',.125)*1024+bytes([0x5A])*16+bytes([0xA5])*1024+bytes([0x5A])*16


def make_depth_inputs(code,source,width,height):
    vertex,proof=translate_rectangle_vertex(code,len(source),width,height)
    exports=evaluate_rectangle(code,source,width,height)
    left,top,right,bottom=exports['source_bounds']
    if any(((edge+.5)*32/size)%1 for edge,size in ((left,width),(right,width),(top,height),(bottom,height))):
        raise ValueError('exact native pixel-grid-aligned reference edges required')
    expected=bytearray(depth_clear_bytes());covered=0;depth=exports['source_exports'][0]['62'][2]
    if depth==0 and math.copysign(1,depth)<0:
        raise ValueError('negative-zero rasterized depth lacks a bit-exact reference')
    for y in range(32):
        for x in range(32):
            px=(x+.5)*width/32-.5;py=(y+.5)*height/32-.5;index=y*32+x
            if left<=px<right and top<=py<bottom:
                struct.pack_into('<f',expected,16+index*4,depth)
                expected[4128+index]=0;covered+=1
    return vertex,passthrough_fragment(),dict(proof,reference=exports,reference_covered_pixels=covered,
        output_bytes=5168,native_viewport=[0,0,32,32],depth_attachment=True,stencil_test=True,
        native_depth_format='D32_SFLOAT_S8_UINT',depth_compare='always',depth_write=True,
        stencil_compare='always',stencil_pass='replace',stencil_reference=0,stencil_write_mask=255,
        native_depth_clear=.125,native_stencil_clear=165,pixel_shader_bound=False,
        fragment_module_used_only_for_cli_compatibility=True,xenos_depth_format_verified=False,
        xenos_edram_storage=False,resolve_executed=False),source,bytes(expected)


def load_rectangle_inputs(report,node,offset):
    if any(type(v) is not int or v<0 for v in (node,offset)):raise ValueError('nonnegative rectangle request location required')
    manifest,shaders,physical=load_dependencies(report)
    found=[d for d in manifest['draws'] if (d['node'],d['offset'])==(node,offset)]
    if len(found)!=1:raise ValueError('unique rectangle request required')
    draw=found[0];stage=draw['last_shader_load_candidates'].get('0')
    if draw['index_count']!=3 or draw['primitive_bits']!=8 or stage is None:
        raise ValueError('three-source-vertex rectangle request required')
    resources=stage['resource_candidates']
    if len(resources)!=1 or resources[0]['slot']!=0 or not resources[0].get('snapshot',{}).get('available'):
        raise ValueError('one captured slot0 vertex descriptor required')
    layout=resources[0]['layout']
    if layout.get('reserved_bits')!=0x10000000:raise ValueError('observed vertex descriptor reserved bits required')
    match=_physical_match(physical,layout['physical_address'],layout['source_bytes'])
    if match is None:raise ValueError('missing vertex source slice')
    code=next(data for meta,data in shaders if meta==stage['load'])
    mode=draw['register_candidates'][str(0x2208)]
    pixel_code=None
    if not mode['fully_known'] or mode['value_bits'] not in (5,6):raise ValueError('only explicit depth/copy rectangle test candidates')
    if mode['value_bits']==5:
        ps=draw['last_shader_load_candidates'].get('1')
        if ps is None:raise ValueError('captured copy-pixel candidate required for color diagnostic')
        pixel_code=next(data for meta,data in shaders if meta==ps['load'])
    return code,match[2],pixel_code,dict(draw,report_sha256=manifest['report_sha256'],
        title_pixel_execution_required_verified=False)


def run(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report',type=Path);parser.add_argument('output_directory',type=Path)
    for name in ('node','offset','width','height'):parser.add_argument('--'+name,type=lambda v:int(v,0),required=True)
    for name in ('glslc','spirv-validator'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--depth-stencil',action='store_true',help='explicit D32-float/S8 native test, not retail storage')
    args=parser.parse_args(argv);output=_require_private_report(args.output_directory,repository_root())
    compiler,validator=_require_validator(args.glslc),_require_validator(args.spirv_validator)
    code,source,pixel,selection=load_rectangle_inputs(args.report,args.node,args.offset)
    if args.depth_stencil:
        if selection['register_candidates'][str(0x2208)]['value_bits']!=5:raise ValueError('depth-mode request candidate required')
        # Match the requested depth/stencil operations, but do not claim the
        # chosen native format, viewport or sample storage is retail-equivalent.
        for register,value in ((0x2200,0x8777),(0x210D,0xFF0000)):
            state=selection['register_candidates'][str(register)]
            if not state['fully_known'] or state['value_bits']!=value:raise ValueError('exact depth/stencil request values required')
        vertex,fragment,proof,combined,expected=make_depth_inputs(code,source,args.width,args.height)
    else:vertex,fragment,proof,combined,expected=make_inputs(code,source,args.width,args.height,pixel)
    output.mkdir(mode=0o700)
    for name,data in (('input.bin',combined),('expected.bin',expected)):_write_exclusive(output/name,data)
    modules={}
    for stage,suffix,text in (('vertex','vert',vertex),('fragment','frag',fragment)):
        folder=output/stage;folder.mkdir(mode=0o700);path=folder/f'dataflow.{suffix}';_write_exclusive(path,text.encode())
        modules[stage]=hashlib.sha256(compile_compute(path,folder,compiler,validator)).hexdigest()
    _write_exclusive(output/'proof.json',(json.dumps(dict(proof,selection=selection,modules_sha256=modules,
        spirv_validated=True,input_sha256=hashlib.sha256(combined).hexdigest(),
        expected_sha256=hashlib.sha256(expected).hexdigest()),indent=2)+'\n').encode())
    print(f'Validated rectangle diagnostic; explicit mapping, GPU/resolve not run. Private output: {output}')


if __name__=='__main__':
    try:run()
    except (ValueError,KeyError,IndexError,TypeError,OSError) as error:
        print(f'error: {error}',file=sys.stderr);raise SystemExit(2)
