#!/usr/bin/env python3
"""Private native texture sampling reference, not a translated game pixel shader."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
from generate_vertex_dataflow_probe import compile_compute
from rpf7_extract import _write_exclusive
from xenos_preflight_batch import (BatchLimits,_prepare_inputs,_read_private_input,
    _require_private_report,_require_validator,repository_root)

MODES={'center':0,'periodic':1,'quarter':2}


def expected_samples(source,mode):
    if mode not in MODES or not isinstance(source,bytes) or len(source)!=4096:
        raise ValueError('exact32x32 RGBA8 source andknown native sample mode required')
    output=bytearray([0x5A]*16416)
    for y in range(32):
        for x in range(32):
            at=(y*32+x)*4
            if mode!='quarter':values=source[at:at+4]
            else:
                values=[]
                for lane in range(4):
                    total=sum(source[(((y+dy)%32)*32+(x+dx)%32)*4+lane] for dx,dy in ((0,0),(1,0),(0,1),(1,1)))
                    if total%4:raise ValueError('quarter fixture requires exact integer channel averages')
                    values.append(total//4)
            struct.pack_into('<4I',output,16+(y*32+x)*16,*values)
    return bytes(output)


def generate(source,output,mode,compiler,validator):
    expected=expected_samples(source,mode)
    output=_require_private_report(output,repository_root())
    output.mkdir(mode=0o700)
    shader=(Path(__file__).parent/'texture32_sample.comp').read_text().replace('#define SAMPLE_MODE 0',f'#define SAMPLE_MODE {MODES[mode]}')
    for name,data in (('input.bin',source),('expected.bin',expected),('dataflow.comp',shader.encode())):
        _write_exclusive(output/name,data)
    binary=compile_compute(output/'dataflow.comp',output,compiler,validator)
    proof={'mode':mode,'input_sha256':hashlib.sha256(source).hexdigest(),
        'expected_sha256':hashlib.sha256(expected).hexdigest(),'spirv_sha256':hashlib.sha256(binary).hexdigest(),
        'width':32,'height':32,'source_bytes':4096,'output_bytes':16416,'reference_samples':1024,
        'native_profile':'linear-repeat-explicit-lod0','spirv_validated':True,
        'xenos_sampler_semantics_verified':False,'game_shader':False,'gpu_executed':False,'game_frame':False}
    _write_exclusive(output/'proof.json',(json.dumps(proof,indent=2)+'\n').encode())
    return proof


def run(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input_rgba',type=Path);parser.add_argument('output_directory',type=Path)
    parser.add_argument('--mode',choices=MODES,required=True)
    parser.add_argument('--glslc',type=Path,required=True);parser.add_argument('--spirv-validator',type=Path,required=True)
    args=parser.parse_args(argv)
    source=_read_private_input(_prepare_inputs([args.input_rgba],repository_root(),BatchLimits(max_files=1,max_file_bytes=4096))[0])
    generate(source,args.output_directory,args.mode,_require_validator(args.glslc),_require_validator(args.spirv_validator))
    print(f'Validated native-only1024-sample fixture; GPU notrun. Private output: {args.output_directory}')


if __name__=='__main__':
    try:run()
    except (ValueError,KeyError,TypeError,OSError) as error:
        print(f'error: {error}',file=sys.stderr);raise SystemExit(2)
