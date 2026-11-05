#!/usr/bin/env python3
"""Generate a private compute-only vertex arithmetic experiment, never a draw.

Explicit constant input is BE vec4 bytes in sorted referenced-register order.
It is not a claim about GPU execution-time constant contents or ALLOC semantics.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
from generate_vertex_dataflow_probe import load_inputs, compile_compute
from vertex_arithmetic_dataflow import evaluate_vertex_dataflow, translate_vertex_arithmetic_compute
from rpf7_extract import _write_exclusive
from xenos_preflight_batch import (BatchLimits, _prepare_inputs, _read_private_input,
    _require_private_report, _require_validator, repository_root)


def make_inputs(code,vertices,packed_constants):
    source,proof=translate_vertex_arithmetic_compute(code,len(vertices))
    registers=proof['constant_registers']
    if not isinstance(packed_constants,bytes) or len(packed_constants)!=16*len(registers):
        raise ValueError('exact sorted referenced constant-vector extent required')
    constants={r:packed_constants[i*16:(i+1)*16] for i,r in enumerate(registers)}
    expected=bytearray([0x5A]*224)
    for index in range(4):
        exports=evaluate_vertex_dataflow(code,{proof['explicit_fetch_slot']:vertices},constants,index)
        for slot,base in (('62',16),('0',32),('1',48)):
            for lane,value in enumerate(exports.get(slot,[])):
                if value is not None:struct.pack_into('<f',expected,base+index*48+lane*4,value)
    return source,proof,vertices+packed_constants,bytes(expected)


def run(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report',type=Path)
    parser.add_argument('output_directory',type=Path)
    parser.add_argument('--shader-index',type=int,required=True)
    parser.add_argument('--range-index',type=int,required=True)
    parser.add_argument('--constants',type=Path,required=True)
    parser.add_argument('--glslc',type=Path,required=True)
    parser.add_argument('--spirv-validator',type=Path,required=True)
    args=parser.parse_args(argv)
    output=_require_private_report(args.output_directory,repository_root())
    compiler,validator=_require_validator(args.glslc),_require_validator(args.spirv_validator)
    code,vertices,provenance=load_inputs(args.report,args.shader_index,args.range_index)
    packed=_read_private_input(_prepare_inputs([args.constants],repository_root(),
        BatchLimits(max_files=1,max_file_bytes=4096))[0])
    source,proof,combined,expected=make_inputs(code,vertices,packed)
    output.mkdir(mode=0o700)
    for name,data in (('input.bin',combined),('expected.bin',expected),('dataflow.comp',source.encode())):
        _write_exclusive(output/name,data)
    binary=compile_compute(output/'dataflow.comp',output,compiler,validator)
    record=dict(proof,provenance=provenance,constants_sha256=hashlib.sha256(packed).hexdigest(),
        constant_execution_time_contents_verified=False,spirv_sha256=hashlib.sha256(binary).hexdigest(),
        input_sha256=hashlib.sha256(combined).hexdigest(),expected_sha256=hashlib.sha256(expected).hexdigest(),
        cpu_reference_vertices=4,spirv_validated=True)
    _write_exclusive(output/'proof.json',(json.dumps(record,indent=2)+'\n').encode())
    print(f'Validated arithmetic compute module; 4 CPU references; no GPU execution. Private output: {output}')


if __name__=='__main__':
    try:run()
    except (ValueError,KeyError,IndexError,TypeError,OSError) as error:
        print(f'error: {error}',file=sys.stderr)
        raise SystemExit(2)
