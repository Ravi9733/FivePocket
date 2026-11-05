#!/usr/bin/env python3
"""Independent authored three-image byte-level sampling tests; not title shaders."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
from generate_vertex_dataflow_probe import compile_compute
from generate_r8_planes_pixel_probe import EXTENTS
from rpf7_extract import _write_exclusive
from xenos_preflight_batch import _require_private_report,_require_validator,repository_root


def sweep_planes():
    return [bytes(((x//5 if i==0 else x//2+37 if i==1 else (x//2)*3+11)%256)
                  for x in range(width))*height for i,(width,height) in enumerate(EXTENTS)]


def integer_filter_reference(data,width,height,u,v):
    if not all(math.isfinite(c) and abs(c)<=4 for c in (u,v)):
        raise ValueError('bounded finite reference coordinates required')
    x,y=(u-math.floor(u))*width-.5,(v-math.floor(v))*height-.5
    left,top=math.floor(x),math.floor(y);fx,fy=x-left,y-top
    values=[data[((top+dy)%height)*width+(left+dx)%width] for dy in (0,1) for dx in (0,1)]
    if len(set(values))==1:return values[0]
    if fx not in (0,.25,.5,.75) or fy not in (0,.25,.5,.75):
        raise ValueError('exact quarter-texel weights required')
    top_value=values[0]*(1-fx)+values[1]*fx
    bottom_value=values[2]*(1-fx)+values[3]*fx
    result=top_value*(1-fy)+bottom_value*fy
    if any(value!=int(value) for value in (top_value,bottom_value,result)):
        raise ValueError('integer intermediate and final byte averages required')
    return int(result)


def make_inputs(mode,planes,records=b''):
    if len(planes)!=3 or any(not isinstance(p,bytes) or len(p)!=w*h for p,(w,h) in zip(planes,EXTENTS)):
        raise ValueError('three exact tightly packed planes required')
    expected=bytearray([0x5A]*96)
    if mode=='sweep':
        if records:raise ValueError('sweep uses authored coordinates, not records')
        for i in range(4):struct.pack_into('<4I',expected,16+i*16,64,64,64,1)
    elif mode=='integer':
        if not isinstance(records,bytes) or len(records)!=128:raise ValueError('four exact BE r0/r1 records required')
        for index in range(4):
            uv=struct.unpack_from('>4f',records,index*32)
            if not all(math.isfinite(value) for value in uv):continue
            samples=[integer_filter_reference(p,w,h,*(uv[:2] if i==0 else uv[2:]))
                     for i,(p,(w,h)) in enumerate(zip(planes,EXTENTS))]
            struct.pack_into('<4I',expected,16+index*16,*samples,255)
    else:raise ValueError('unknown independent R8 sampling mode')
    return b''.join(planes)+records,bytes(expected)


def generate(mode,planes,records,output,compiler,validator):
    combined,expected=make_inputs(mode,planes,records)
    output=_require_private_report(output,repository_root());output.mkdir(mode=0o700)
    asset='r8_planes_byte_sweep.comp' if mode=='sweep' else 'r8_planes_integer_filter.comp'
    source=Path(__file__).with_name(asset).read_bytes()
    for name,data in (('input.bin',combined),('expected.bin',expected),('dataflow.comp',source)):
        _write_exclusive(output/name,data)
    binary=compile_compute(output/'dataflow.comp',output,compiler,validator)
    proof={'mode':mode,'authored_shader':True,'game_shader':False,'native_image_count':3,
        'byte_quantized_reference':True,'native_float_intermediates_verified':False,
        'input_sha256':hashlib.sha256(combined).hexdigest(),'expected_sha256':hashlib.sha256(expected).hexdigest(),
        'spirv_sha256':hashlib.sha256(binary).hexdigest(),'spirv_validated':True,
        'maximum_invocations':4,'values_per_plane':256 if mode=='sweep' else 4,
        'gpu_executed':False,'game_frame':False}
    _write_exclusive(output/'proof.json',(json.dumps(proof,indent=2)+'\n').encode())
    return proof


def run(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output_directory',type=Path)
    parser.add_argument('--glslc',type=Path,required=True);parser.add_argument('--spirv-validator',type=Path,required=True)
    args=parser.parse_args(argv)
    generate('sweep',sweep_planes(),b'',args.output_directory,_require_validator(args.glslc),_require_validator(args.spirv_validator))
    print(f'Independent 256-value-per-plane module validated; GPU not run. Private output: {args.output_directory}')


if __name__=='__main__':run()
