#!/usr/bin/env python3
"""Explicit finite-input packed-color conversion fixture; no title shader."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'rpf7_inventory'))
from color_7e3 import pack,unpack
from generate_vertex_dataflow_probe import compile_compute
from rpf7_extract import _write_exclusive
from xenos_preflight_batch import (BatchLimits,_prepare_inputs,_read_private_input,
    _require_private_report,_require_validator,repository_root)


def make_reference(data):
    if not 0<len(data)<=4096 or len(data)%16:raise ValueError('1..256 aligned little-endian RGBA float32 records required')
    result=bytearray([0x5A])*8224;accepted=0
    for index,rgba in enumerate(struct.iter_unpack('<4I',data)):
        try:packed=pack(rgba)
        except ValueError:continue
        struct.pack_into('<5I',result,16+index*32,packed,*unpack(packed));accepted+=1
    return bytes(result),{'source_records':len(data)//16,'accepted_records':accepted,
        'output_records':256,'output_record_bytes':32,'output_bytes':8224,
        'record_fields':['packed_word','decoded_r_bits','decoded_g_bits','decoded_b_bits','decoded_a_bits'],
        'trailing_record_canary_bytes':12,'rgb_policy':'finite-clamped-nearest-even-and-source-candidate-agreement',
        'alpha_policy':'finite-clamp-separate-float32-multiply3-add-half-truncate',
        'retail_rop_verified':False,'format12_intermediate_precision_verified':False,
        'native_gpu_executed':False,'title_draw_executed':False,'game_frame':False}


def run(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input',type=Path);parser.add_argument('output_directory',type=Path)
    for name in ('glslc','spirv-validator'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args(argv)
    root=repository_root()
    source=_prepare_inputs([args.input],root,BatchLimits(max_files=1,max_file_bytes=4096))[0]
    output=_require_private_report(args.output_directory,root)
    data=_read_private_input(source);expected,proof=make_reference(data)
    compiler,validator=_require_validator(args.glslc),_require_validator(args.spirv_validator)
    output.mkdir(mode=0o700)
    for name,payload in (('input.bin',data),('expected.bin',expected)):_write_exclusive(output/name,payload)
    shader=output/'dataflow.comp';_write_exclusive(shader,Path(__file__).with_name('color_7e3.comp').read_bytes())
    module=compile_compute(shader,output,compiler,validator)
    _write_exclusive(output/'proof.json',(json.dumps(dict(proof,spirv_validated=True,
        input_sha256=hashlib.sha256(data).hexdigest(),expected_sha256=hashlib.sha256(expected).hexdigest(),
        module_sha256=hashlib.sha256(module).hexdigest()),indent=2)+'\n').encode())
    print(f'Validated bounded color-format fixture, not run: {output}')


if __name__=='__main__':
    try:run()
    except (ValueError,OSError) as error:
        print(f'error: {error}',file=sys.stderr);raise SystemExit(2)
