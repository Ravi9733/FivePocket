#!/usr/bin/env python3
"""Private pixel arithmetic COMPUTE probe with explicit sample results, no sampling."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
from generate_vertex_dataflow_probe import read_artifact,compile_compute
from pixel_sample_dataflow import evaluate_pixel_sample_dataflow,translate_pixel_sample_compute
from rpf7_extract import _write_exclusive
from xenos_preflight_batch import (BatchLimits,_prepare_inputs,_read_private_input,
    _require_private_report,_require_validator,repository_root)


def load_pixel_shader(report_path,shader_index):
    if type(shader_index) is not int or not 0<=shader_index<8:raise ValueError('bounded pixel shader index required')
    entry=_prepare_inputs([report_path],repository_root(),BatchLimits(max_files=1,max_file_bytes=1024*1024))[0]
    encoded=_read_private_input(entry);capture=json.loads(encoded)['execution']['stopped_shader_capture']
    if capture['status']!=1 or not 1<=len(capture['artifacts'])<=8:raise ValueError('complete shader capture required')
    artifact=capture['artifacts'][shader_index]
    if artifact['stage']!=1:raise ValueError('pixel-stage artifact required')
    code=read_artifact(entry.path.parent,artifact,artifact['instruction_words']*4)
    return code,{'report_sha256':hashlib.sha256(encoded).hexdigest(),'shader':artifact,'gpu_time_contents_verified':False}


def make_inputs(code,records,packed_constants):
    source,proof=translate_pixel_sample_compute(code);registers=proof['constant_registers']
    stride=proof['record_bytes']
    if not isinstance(records,bytes) or len(records)!=stride*4:raise ValueError('four exact BE r0/r1/sample records required')
    if not isinstance(packed_constants,bytes) or len(packed_constants)!=len(registers)*16:
        raise ValueError('exact sorted pixel constant vectors required')
    constants={c:packed_constants[i*16:(i+1)*16] for i,c in enumerate(registers)}
    expected=bytearray([0x5A]*96)
    for index in range(4):
        values=struct.unpack_from('>'+str(stride//4)+'f',records,index*stride)
        samples={s:values[8+s*4:12+s*4] for s in range(proof['sample_count'])}
        result=evaluate_pixel_sample_dataflow(code,{0:values[:4],1:values[4:8]},constants,samples)
        struct.pack_into('<4f',expected,16+index*16,*result['0'])
    return source,proof,records+packed_constants,bytes(expected)


def run(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report',type=Path);parser.add_argument('output_directory',type=Path)
    parser.add_argument('--shader-index',type=int,required=True)
    parser.add_argument('--records',type=Path,required=True);parser.add_argument('--constants',type=Path,required=True)
    parser.add_argument('--glslc',type=Path,required=True);parser.add_argument('--spirv-validator',type=Path,required=True)
    args=parser.parse_args(argv);output=_require_private_report(args.output_directory,repository_root())
    compiler,validator=_require_validator(args.glslc),_require_validator(args.spirv_validator)
    code,provenance=load_pixel_shader(args.report,args.shader_index)
    def read(path):
        return _read_private_input(_prepare_inputs([path],repository_root(),BatchLimits(max_files=1,max_file_bytes=4096))[0])
    source,proof,combined,expected=make_inputs(code,read(args.records),read(args.constants))
    output.mkdir(mode=0o700)
    for name,data in (('input.bin',combined),('expected.bin',expected),('dataflow.comp',source.encode())):
        _write_exclusive(output/name,data)
    binary=compile_compute(output/'dataflow.comp',output,compiler,validator)
    record=dict(proof,provenance=provenance,input_sha256=hashlib.sha256(combined).hexdigest(),
        expected_sha256=hashlib.sha256(expected).hexdigest(),spirv_sha256=hashlib.sha256(binary).hexdigest(),
        spirv_validated=True,cpu_reference_invocations=4,constant_gpu_time_contents_verified=False)
    _write_exclusive(output/'proof.json',(json.dumps(record,indent=2)+'\n').encode())
    print(f'Validated explicit-sample pixel arithmetic compute; no texture sampling or GPU run. Private output: {output}')


if __name__=='__main__':
    try:run()
    except (ValueError,KeyError,IndexError,TypeError,OSError) as error:
        print(f'error: {error}',file=sys.stderr);raise SystemExit(2)
