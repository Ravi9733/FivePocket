#!/usr/bin/env python3
"""Three real R8 image inputs for a pixel-derived compute fixture, not a draw."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
from generate_pixel_sample_probe import load_pixel_shader
from generate_vertex_dataflow_probe import compile_compute
from pixel_sample_dataflow import (evaluate_pixel_sample_dataflow,
    translate_r8_planes_pixel_compute,R8_PLANE_IMAGE_BYTES)
from rpf7_extract import _write_exclusive
from xenos_preflight_batch import (BatchLimits,_prepare_inputs,_read_private_input,
    _require_private_report,_require_validator,repository_root)

EXTENTS=((1280,720),(640,360),(640,360))


def binary_linear_reference(data,width,height,u,v,*,require_uniform=False):
    """Independent ideal reference, binary texels only. Nonuniform footprints
    with quarter weights are useful math references, NOT bit-exact native R8
    filtering promises. Native pixel comparisons require uniform footprints.
    """
    if (not math.isfinite(u) or not math.isfinite(v) or abs(u)>4 or abs(v)>4):
        raise ValueError('bounded finite explicit reference coordinates required')
    x,y=(u-math.floor(u))*width-.5,(v-math.floor(v))*height-.5
    left,top=math.floor(x),math.floor(y);fx,fy=x-left,y-top
    values=[data[((top+dy)%height)*width+(left+dx)%width] for dy in (0,1) for dx in (0,1)]
    if any(value not in (0,255) for value in values):raise ValueError('binary reference texels required')
    if len(set(values))==1:return float(values[0]==255)
    if require_uniform:raise ValueError('bit-exact native reference requires a uniform binary filter footprint')
    if fx not in (0,.25,.5,.75) or fy not in (0,.25,.5,.75):
        raise ValueError('nonuniform reference needs exact quarter-texel weights')
    a,b,c,d=[float(value==255) for value in values]
    return (a*(1-fx)+b*fx)*(1-fy)+(c*(1-fx)+d*fx)*fy


def reference_bytes(code,combined):
    _,proof=translate_r8_planes_pixel_compute(code)
    output=bytearray([0x5A]*96)
    if len(combined)<proof['input_bytes']:return bytes(output),list(range(4))
    if len(combined)!=proof['input_bytes']:raise ValueError('unexpected trailing probe bytes')
    planes=[];offset=0
    for width,height in EXTENTS:
        plane=combined[offset:offset+width*height];offset+=width*height
        if any(value not in (0,255) for value in plane):raise ValueError('binary-channel R8 reference planes required')
        planes.append(plane)
    constants={c:combined[offset+128+i*16:offset+144+i*16] for i,c in enumerate(proof['constant_registers'])}
    rejected=[]
    for index in range(4):
        record=struct.unpack_from('>8f',combined,offset+index*32)
        if any(not math.isfinite(record[lane]) for pair in proof['initial_coordinate_components'] for lane in pair):
            rejected.append(index);continue
        samples={}
        for i,(width,height) in enumerate(EXTENTS):
            lanes=proof['initial_coordinate_components'][i]
            value=binary_linear_reference(planes[i],width,height,record[lanes[0]],record[lanes[1]],require_uniform=True)
            samples[i]=[value,value,value,1]
        try:color=evaluate_pixel_sample_dataflow(code,{0:record[:4],1:record[4:]},constants,samples)['0']
        except ValueError:rejected.append(index);continue
        struct.pack_into('<4f',output,16+index*16,*color)
    return bytes(output),rejected


def make_inputs(code,planes,records,constants):
    source,proof=translate_r8_planes_pixel_compute(code)
    if len(planes)!=3 or any(not isinstance(p,bytes) or len(p)!=w*h for p,(w,h) in zip(planes,EXTENTS)):
        raise ValueError('exact three tight R8 planes required')
    if not isinstance(records,bytes) or len(records)!=128:raise ValueError('four exact BE r0/r1 records required')
    if not isinstance(constants,bytes) or len(constants)!=16*len(proof['constant_registers']):
        raise ValueError('exact sorted BE constants required')
    combined=b''.join(planes)+records+constants
    expected,rejected=reference_bytes(code,combined)
    return source,dict(proof,reference_texels='binary-0-or-255',
        reference_filter_footprint='uniform-binary-only',reference_rejected_invocations=rejected,
        native_unorm_intermediate_precision_verified=False),combined,expected


def run(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report',type=Path);parser.add_argument('output_directory',type=Path)
    parser.add_argument('--shader-index',type=int,required=True)
    for name in ('y','v','u','records','constants','glslc','spirv-validator'):
        parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args(argv);output=_require_private_report(args.output_directory,repository_root())
    compiler,validator=_require_validator(args.glslc),_require_validator(args.spirv_validator)
    code,provenance=load_pixel_shader(args.report,args.shader_index)
    def read(path,limit):return _read_private_input(_prepare_inputs([path],repository_root(),
        BatchLimits(max_files=1,max_file_bytes=limit))[0])
    planes=[read(path,w*h) for path,(w,h) in zip((args.y,args.v,args.u),EXTENTS)]
    source,proof,combined,expected=make_inputs(code,planes,read(args.records,128),read(args.constants,80))
    output.mkdir(mode=0o700)
    for name,data in (('input.bin',combined),('expected.bin',expected),('dataflow.comp',source.encode())):
        _write_exclusive(output/name,data)
    binary=compile_compute(output/'dataflow.comp',output,compiler,validator)
    record=dict(proof,provenance=provenance,input_sha256=hashlib.sha256(combined).hexdigest(),
        expected_sha256=hashlib.sha256(expected).hexdigest(),spirv_sha256=hashlib.sha256(binary).hexdigest(),
        spirv_validated=True,cpu_reference_invocations=4,constant_gpu_time_contents_verified=False)
    _write_exclusive(output/'proof.json',(json.dumps(record,indent=2)+'\n').encode())
    print(f'Validated three-image pixel compute; GPU not run. Private output: {output}')


if __name__=='__main__':
    try:run()
    except (ValueError,KeyError,IndexError,TypeError,OSError) as error:
        print(f'error: {error}',file=sys.stderr);raise SystemExit(2)
