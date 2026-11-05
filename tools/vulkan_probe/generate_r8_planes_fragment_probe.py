#!/usr/bin/env python3
"""Derived three-plane fragment stage plus authored vertex partner. No guest draw."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
from generate_pixel_sample_probe import load_pixel_shader
from generate_vertex_dataflow_probe import compile_compute
from generate_r8_planes_pixel_probe import EXTENTS,binary_linear_reference
from pixel_sample_dataflow import translate_r8_planes_pixel_fragment,evaluate_pixel_sample_dataflow
from rpf7_extract import _write_exclusive
from xenos_preflight_batch import (BatchLimits,_prepare_inputs,_read_private_input,
    _require_private_report,_require_validator,repository_root)


def make_inputs(code,planes,tint,packed_constants):
    source,proof=translate_r8_planes_pixel_fragment(code)
    if len(planes)!=3 or any(not isinstance(p,bytes) or len(p)!=w*h or any(v not in (0,255) for v in p)
            for p,(w,h) in zip(planes,EXTENTS)):
        raise ValueError('exact three tight binary-channel R8 planes required')
    if not isinstance(tint,bytes) or len(tint)!=16:raise ValueError('exact BE tint header required')
    if not isinstance(packed_constants,bytes) or len(packed_constants)!=16*len(proof['constant_registers']):
        raise ValueError('exact sorted BE constants required')
    constants={c:packed_constants[i*16:i*16+16] for i,c in enumerate(proof['constant_registers'])}
    expected=bytearray([0x5A]*16416);tint_values=struct.unpack('>4f',tint);rejected=0
    for index in range(1024):
        uv=((index%32+.5)/32,(index//32+.5)/32)*2
        samples={}
        for i,(p,(w,h)) in enumerate(zip(planes,EXTENTS)):
            lanes=proof['initial_coordinate_components'][i]
            v=binary_linear_reference(p,w,h,uv[lanes[0]],uv[lanes[1]],require_uniform=True)
            samples[i]=[v,v,v,1]
        try:color=evaluate_pixel_sample_dataflow(code,{0:uv,1:tint_values},constants,samples)['0']
        except ValueError:rejected+=1;continue
        struct.pack_into('<4f',expected,16+index*16,*color)
    return source,dict(proof,synthetic_uv='xyxy-at-32x32-pixel-centers',
        reference_filter_footprint='uniform-binary-only',reference_discarded_fragments=rejected,
        native_unorm_intermediate_precision_verified=False),b''.join(planes)+tint+packed_constants,bytes(expected)


def generate(code,provenance,planes,tint,constants,output,compiler,validator):
    source,proof,combined,expected=make_inputs(code,planes,tint,constants)
    output=_require_private_report(output,repository_root());output.mkdir(mode=0o700)
    for name,data in (('input.bin',combined),('expected.bin',expected)):_write_exclusive(output/name,data)
    fragment=output/'fragment';fragment.mkdir(mode=0o700)
    _write_exclusive(fragment/'dataflow.frag',source.encode())
    binary=compile_compute(fragment/'dataflow.frag',fragment,compiler,validator)
    vertex=output/'vertex';vertex.mkdir(mode=0o700)
    partner=compile_compute(Path(__file__).with_name('r8_planes_fullscreen.vert').resolve(),vertex,compiler,validator)
    record=dict(proof,provenance=provenance,input_sha256=hashlib.sha256(combined).hexdigest(),
        expected_sha256=hashlib.sha256(expected).hexdigest(),spirv_sha256=hashlib.sha256(binary).hexdigest(),
        synthetic_vertex_spirv_sha256=hashlib.sha256(partner).hexdigest(),spirv_validated=True,
        cpu_reference_fragments=1024,constant_gpu_time_contents_verified=False)
    _write_exclusive(output/'proof.json',(json.dumps(record,indent=2)+'\n').encode())
    return record


def run(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report',type=Path);parser.add_argument('output_directory',type=Path)
    parser.add_argument('--shader-index',type=int,required=True)
    for name in ('y','v','u','tint','constants','glslc','spirv-validator'):
        parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args(argv)
    code,provenance=load_pixel_shader(args.report,args.shader_index)
    def read(path,limit):return _read_private_input(_prepare_inputs([path],repository_root(),
        BatchLimits(max_files=1,max_file_bytes=limit))[0])
    planes=[read(path,w*h) for path,(w,h) in zip((args.y,args.v,args.u),EXTENTS)]
    generate(code,provenance,planes,read(args.tint,16),read(args.constants,80),args.output_directory,
        _require_validator(args.glslc),_require_validator(args.spirv_validator))
    print(f'Validated three-plane fragment and synthetic vertex; GPU not run. Private output: {args.output_directory}')


if __name__=='__main__':
    try:run()
    except (ValueError,KeyError,IndexError,TypeError,OSError) as error:
        print(f'error: {error}',file=sys.stderr);raise SystemExit(2)
