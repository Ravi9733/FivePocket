#!/usr/bin/env python3
"""Private pixel-derived compute with native 32x32 center samples, NOT a draw."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
from generate_pixel_sample_probe import load_pixel_shader
from generate_vertex_dataflow_probe import compile_compute
from pixel_sample_dataflow import (evaluate_pixel_sample_dataflow,translate_texture32_pixel_compute,
    translate_texture32_pixel_fragment)
from rpf7_extract import _write_exclusive
from xenos_preflight_batch import (BatchLimits,_prepare_inputs,_read_private_input,
    _require_private_report,_require_validator,repository_root)


def make_inputs(code,rgba,packed_tint,packed_constants):
    source,proof=translate_texture32_pixel_compute(code);registers=proof['constant_registers']
    if not isinstance(rgba,bytes) or len(rgba)!=4096 or any(v not in (0,255) for v in rgba):
        raise ValueError('exact binary-channel RGBA32 image required for bit-exact native sampling references')
    if not isinstance(packed_tint,bytes) or len(packed_tint)!=16:raise ValueError('exact BE tint vector required')
    if not isinstance(packed_constants,bytes) or len(packed_constants)!=16*len(registers):
        raise ValueError('exact sorted BE constant vectors required')
    tint=struct.unpack('>4f',packed_tint)
    constants={c:packed_constants[i*16:(i+1)*16] for i,c in enumerate(registers)}
    expected=bytearray([0x5A]*16416)
    for index in range(1024):
        coordinates=[(index%32+.5)/32,(index//32+.5)/32,None,None]
        sampled=[float(v==255) for v in rgba[index*4:index*4+4]]
        color=evaluate_pixel_sample_dataflow(code,{0:coordinates,1:tint},constants,{0:sampled})['0']
        struct.pack_into('<4f',expected,16+index*16,*color)
    return source,dict(proof,reference_image_channels='binary-0-or-255',
        native_unorm_intermediate_precision_verified=False),rgba+packed_tint+packed_constants,bytes(expected)


def run(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report',type=Path);parser.add_argument('output_directory',type=Path)
    parser.add_argument('--shader-index',type=int,required=True)
    parser.add_argument('--rgba',type=Path,required=True);parser.add_argument('--tint',type=Path,required=True)
    parser.add_argument('--constants',type=Path,required=True)
    parser.add_argument('--glslc',type=Path,required=True);parser.add_argument('--spirv-validator',type=Path,required=True)
    parser.add_argument('--fragment',action='store_true',help='synthetic vertex partner and derived fragment stage, not title linkage')
    args=parser.parse_args(argv);output=_require_private_report(args.output_directory,repository_root())
    compiler,validator=_require_validator(args.glslc),_require_validator(args.spirv_validator)
    code,provenance=load_pixel_shader(args.report,args.shader_index)
    def read(path):return _read_private_input(_prepare_inputs([path],repository_root(),BatchLimits(max_files=1,max_file_bytes=4096))[0])
    source,proof,combined,expected=make_inputs(code,read(args.rgba),read(args.tint),read(args.constants))
    if args.fragment:
        source,fragment_proof=translate_texture32_pixel_fragment(code);proof.update(fragment_proof)
    output.mkdir(mode=0o700)
    for name,data in (('input.bin',combined),('expected.bin',expected)):
        _write_exclusive(output/name,data)
    if args.fragment:
        fragment_dir=output/'fragment';fragment_dir.mkdir(mode=0o700)
        _write_exclusive(fragment_dir/'dataflow.frag',source.encode())
        binary=compile_compute(fragment_dir/'dataflow.frag',fragment_dir,compiler,validator)
        vertex_dir=output/'vertex';vertex_dir.mkdir(mode=0o700)
        vertex=compile_compute(Path(__file__).resolve().with_name('texture32_fullscreen.vert'),vertex_dir,compiler,validator)
        proof['synthetic_vertex_spirv_sha256']=hashlib.sha256(vertex).hexdigest()
    else:
        _write_exclusive(output/'dataflow.comp',source.encode())
        binary=compile_compute(output/'dataflow.comp',output,compiler,validator)
    record=dict(proof,provenance=provenance,input_sha256=hashlib.sha256(combined).hexdigest(),
        expected_sha256=hashlib.sha256(expected).hexdigest(),spirv_sha256=hashlib.sha256(binary).hexdigest(),
        spirv_validated=True,cpu_reference_invocations=1024,constant_gpu_time_contents_verified=False)
    _write_exclusive(output/'proof.json',(json.dumps(record,indent=2)+'\n').encode())
    print(f'Validated texture/pixel {"fragment" if args.fragment else "compute"} experiment; 1024 CPU references; GPU not run. Private output: {output}')


if __name__=='__main__':
    try:run()
    except (ValueError,KeyError,IndexError,TypeError,OSError) as error:
        print(f'error: {error}',file=sys.stderr);raise SystemExit(2)
