#!/usr/bin/env python3
"""Captured vertex/three-plane pixel pair, explicit diagnostic linkage/raster."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
from generate_vertex_dataflow_probe import load_inputs,compile_compute
from generate_pixel_sample_probe import load_pixel_shader
from generate_r8_planes_fragment_probe import make_inputs as fragment_reference,EXTENTS
from pixel_sample_dataflow import translate_r8_planes_pixel_fragment
from vertex_arithmetic_dataflow import evaluate_vertex_dataflow,translate_r8_vertex_arithmetic_stage
from rpf7_extract import _write_exclusive
from xenos_preflight_batch import (BatchLimits,_prepare_inputs,_read_private_input,
    _require_private_report,_require_validator,repository_root)


def make_inputs(vertex_code,pixel_code,vertices,vertex_constants,pixel_constants,planes):
    fragment,ps=translate_r8_planes_pixel_fragment(pixel_code,vertex_tint=True)
    vertex,vs=translate_r8_vertex_arithmetic_stage(vertex_code,len(vertices),ps['input_bytes'])
    if not isinstance(vertex_constants,bytes) or len(vertex_constants)!=16*len(vs['constant_registers']):
        raise ValueError('exact sorted vertex constants required')
    if len(planes)!=3 or any(not isinstance(p,bytes) or len(p)!=w*h for p,(w,h) in zip(planes,EXTENTS)):
        raise ValueError('three exact tight R8 planes required')
    vconst={c:vertex_constants[i*16:i*16+16] for i,c in enumerate(vs['constant_registers'])}
    exports=[evaluate_vertex_dataflow(vertex_code,{vs['explicit_fetch_slot']:vertices},vconst,i) for i in range(4)]
    for i,(position,uv) in enumerate(zip(([-1,-1,0,1],[-1,1,0,1],[1,-1,0,1],[1,1,0,1]),
                                        ([0,1,0,1],[0,0,0,0],[1,1,1,1],[1,0,1,0]))):
        if exports[i]['62']!=position or exports[i]['0']!=uv or exports[i]['1']!=exports[0]['1']:
            raise ValueError('exact full rectangle/xyxy UV corners and uniform tint required')
    # Explicit raster oracle: flipping rows in the reference inputs is equivalent
    # to the verified vertex corners' inverted V. Actual uploaded planes are NOT
    # changed. Reference only accepts uniform binary filter footprints.
    mirrored=[b''.join(p[y*w:(y+1)*w] for y in reversed(range(h))) for p,(w,h) in zip(planes,EXTENTS)]
    _,reference,_,expected=fragment_reference(pixel_code,mirrored,struct.pack('>4f',*exports[0]['1']),pixel_constants)
    combined=b''.join(planes)+bytes(16)+pixel_constants+vertices+vertex_constants
    if len(combined)!=vs['input_bytes']:raise ValueError('combined pair input differs from bounded stage layout')
    proof={'vertex':vs,'pixel':ps,'vertex_exports':exports,'input_bytes':len(combined),'output_bytes':len(expected),
        'linkage':{'position':'export62.xyzw','location0':'parameter0.xyzw -> pixel r0.xyzw',
                   'location1':'parameter1.xyzw -> pixel r1.xyzw'},
        'topology':'explicit-four-vertex-triangle-strip','native_viewport':[0,0,32,32],
        'reference_texture_orientation':'vertical-flip','reference_filter_footprint':'uniform-binary-only',
        'reference_discarded_fragments':reference['reference_discarded_fragments'],'uniform_tint_required':True,
        'unused_pixel_header_bytes':16,'title_linkage_verified':False,'title_raster_state_replayed':False,
        'xenos_sampler_verified':False,'gpu_executed':False,'game_frame':False,'guest_writeback':False}
    return vertex,fragment,proof,combined,expected


def generate(vertex_code,pixel_code,vertices,vertex_constants,pixel_constants,planes,
        vertex_provenance,pixel_provenance,output,compiler,validator):
    vertex,fragment,proof,combined,expected=make_inputs(vertex_code,pixel_code,vertices,vertex_constants,pixel_constants,planes)
    output=_require_private_report(output,repository_root());output.mkdir(mode=0o700)
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
    return record


def run(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report',type=Path);parser.add_argument('output_directory',type=Path)
    for name in ('vertex-index','pixel-index','range-index'):parser.add_argument('--'+name,type=int,required=True)
    for name in ('vertex-constants','pixel-constants','y','v','u','glslc','spirv-validator'):
        parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args(argv)
    vertex_code,vertices,vertex_provenance=load_inputs(args.report,args.vertex_index,args.range_index)
    pixel_code,pixel_provenance=load_pixel_shader(args.report,args.pixel_index)
    def read(path,limit):return _read_private_input(_prepare_inputs([path],repository_root(),
        BatchLimits(max_files=1,max_file_bytes=limit))[0])
    planes=[read(path,w*h) for path,(w,h) in zip((args.y,args.v,args.u),EXTENTS)]
    generate(vertex_code,pixel_code,vertices,read(args.vertex_constants,4096),read(args.pixel_constants,80),planes,
        vertex_provenance,pixel_provenance,args.output_directory,_require_validator(args.glslc),_require_validator(args.spirv_validator))
    print(f'Validated captured three-plane vertex/fragment pair; GPU not run. Private output: {args.output_directory}')


if __name__=='__main__':
    try:run()
    except (ValueError,KeyError,IndexError,TypeError,OSError) as error:
        print(f'error: {error}',file=sys.stderr);raise SystemExit(2)
