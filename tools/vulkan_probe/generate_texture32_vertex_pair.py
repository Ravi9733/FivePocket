#!/usr/bin/env python3
"""Captured vertex/pixel pair under EXPLICIT native test linkage/raster state."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
from generate_vertex_dataflow_probe import load_inputs,compile_compute
from generate_pixel_sample_probe import load_pixel_shader
from pixel_sample_dataflow import evaluate_pixel_sample_dataflow,translate_texture32_pixel_fragment
from vertex_arithmetic_dataflow import evaluate_vertex_dataflow,translate_vertex_arithmetic_stage
from rpf7_extract import _write_exclusive
from xenos_preflight_batch import (BatchLimits,_prepare_inputs,_read_private_input,
    _require_private_report,_require_validator,repository_root)


def make_inputs(vertex_code,pixel_code,vertices,vertex_constants,pixel_constants,rgba):
    fragment,ps=translate_texture32_pixel_fragment(pixel_code,vertex_tint=True)
    vertex,vs=translate_vertex_arithmetic_stage(vertex_code,len(vertices),ps['input_bytes'])
    if (not isinstance(rgba,bytes) or len(rgba)!=4096 or any(v not in (0,255) for v in rgba) or
        not isinstance(vertex_constants,bytes) or len(vertex_constants)!=16*len(vs['constant_registers']) or
        not isinstance(pixel_constants,bytes) or len(pixel_constants)!=16*len(ps['constant_registers'])):
        raise ValueError('exact binary-channel texture and BE constants required')
    vconst={c:vertex_constants[i*16:(i+1)*16] for i,c in enumerate(vs['constant_registers'])}
    pconst={c:pixel_constants[i*16:(i+1)*16] for i,c in enumerate(ps['constant_registers'])}
    exports=[evaluate_vertex_dataflow(vertex_code,{vs['explicit_fetch_slot']:vertices},vconst,i) for i in range(4)]
    # Deliberately narrow CPU raster reference: this one full-viewport strip,
    # unit W, flipped-V coordinates and constant tint at all four vertices.
    for i,(position,uv) in enumerate(zip(([-1,-1,0,1],[-1,1,0,1],[1,-1,0,1],[1,1,0,1]),
                                        ([0,1],[0,0],[1,1],[1,0]))):
        if exports[i]['62']!=position or exports[i]['0'][:2]!=uv or exports[i]['1']!=exports[0]['1']:
            raise ValueError('CPU raster reference requires exact full rectangle/UV corners and uniform tint')
    expected=bytearray([0x5A]*16416)
    for index in range(1024):
        x,y=index%32,index//32;at=((31-y)*32+x)*4
        color=evaluate_pixel_sample_dataflow(pixel_code,{0:[(x+.5)/32,1-(y+.5)/32,None,None],1:exports[0]['1']},
            pconst,{0:[float(v==255) for v in rgba[at:at+4]]})['0']
        struct.pack_into('<4f',expected,16+index*16,*color)
    combined=rgba+bytes(16)+pixel_constants+vertices+vertex_constants
    assert len(combined)==vs['input_bytes']
    proof={'vertex':vs,'pixel':ps,'vertex_exports':exports,'input_bytes':len(combined),'output_bytes':len(expected),
        'linkage':{'position':'export62.xyzw','location0':'parameter0.xy -> pixel r0.xy',
                   'location1':'parameter1.xyzw -> pixel r1.xyzw'},
        'topology':'explicit-four-vertex-triangle-strip','native_viewport':[0,0,32,32],
        'reference_texture_orientation':'vertical-flip','uniform_tint_required':True,'unused_pixel_header_bytes':16,
        'title_linkage_verified':False,'title_raster_state_replayed':False,'xenos_sampler_verified':False,
        'gpu_executed':False,'game_frame':False,'guest_writeback':False}
    return vertex,fragment,proof,combined,bytes(expected)


def run(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('vertex_report',type=Path);parser.add_argument('pixel_report',type=Path)
    parser.add_argument('output_directory',type=Path)
    parser.add_argument('--vertex-index',type=int,required=True);parser.add_argument('--pixel-index',type=int,required=True)
    parser.add_argument('--range-index',type=int,required=True)
    parser.add_argument('--vertex-constants',type=Path,required=True);parser.add_argument('--pixel-constants',type=Path,required=True)
    parser.add_argument('--rgba',type=Path,required=True)
    parser.add_argument('--glslc',type=Path,required=True);parser.add_argument('--spirv-validator',type=Path,required=True)
    args=parser.parse_args(argv);output=_require_private_report(args.output_directory,repository_root())
    compiler,validator=_require_validator(args.glslc),_require_validator(args.spirv_validator)
    vertex_code,vertices,vertex_provenance=load_inputs(args.vertex_report,args.vertex_index,args.range_index)
    pixel_code,pixel_provenance=load_pixel_shader(args.pixel_report,args.pixel_index)
    def read(path):return _read_private_input(_prepare_inputs([path],repository_root(),BatchLimits(max_files=1,max_file_bytes=4096))[0])
    vertex,fragment,proof,combined,expected=make_inputs(vertex_code,pixel_code,vertices,
        read(args.vertex_constants),read(args.pixel_constants),read(args.rgba))
    output.mkdir(mode=0o700)
    for name,data in (('input.bin',combined),('expected.bin',expected)):_write_exclusive(output/name,data)
    modules={}
    for name,extension,source in (('vertex','vert',vertex),('fragment','frag',fragment)):
        folder=output/name;folder.mkdir(mode=0o700);path=folder/f'dataflow.{extension}'
        _write_exclusive(path,source.encode());modules[name]=hashlib.sha256(compile_compute(path,folder,compiler,validator)).hexdigest()
    record=dict(proof,vertex_provenance=vertex_provenance,pixel_provenance=pixel_provenance,
        same_report=vertex_provenance['report_sha256']==pixel_provenance['report_sha256'],
        gpu_time_contents_verified=False,input_sha256=hashlib.sha256(combined).hexdigest(),
        expected_sha256=hashlib.sha256(expected).hexdigest(),modules_sha256=modules,spirv_validated=True)
    _write_exclusive(output/'proof.json',(json.dumps(record,indent=2)+'\n').encode())
    print(f'Validated captured vertex/fragment test pair; explicit raster state; GPU not run. Private output: {output}')


if __name__=='__main__':
    try:run()
    except (ValueError,KeyError,IndexError,TypeError,OSError) as error:
        print(f'error: {error}',file=sys.stderr);raise SystemExit(2)
