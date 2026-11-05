#!/usr/bin/env python3
"""Compile a bounded explicit half-color -> reference EDRAM store fixture."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'rpf7_inventory'))
from half_color_store import store_reference,INPUT_BYTES
from half_color_load import load_reference,INPUT_BYTES as LOAD_INPUT_BYTES
from generate_vertex_dataflow_probe import compile_compute
from rpf7_extract import _write_exclusive
from xenos_preflight_batch import (BatchLimits,_prepare_inputs,_read_private_input,
    _require_private_report,_require_validator,repository_root)


def run(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input',type=Path);parser.add_argument('output_directory',type=Path)
    parser.add_argument('--load',action='store_true',help='inverse bridge: packed EDRAM to half image')
    for name in ('glslc','spirv-validator'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args(argv);root=repository_root()
    source=_prepare_inputs([args.input],root,BatchLimits(max_files=1,max_file_bytes=LOAD_INPUT_BYTES if args.load else INPUT_BYTES))[0]
    output=_require_private_report(args.output_directory,root)
    data=_read_private_input(source);expected,proof=(load_reference if args.load else store_reference)(data)
    compiler,validator=_require_validator(args.glslc),_require_validator(args.spirv_validator)
    output.mkdir(mode=0o700)
    for name,payload in (('input.bin',data),('expected.bin',expected)):_write_exclusive(output/name,payload)
    shader=output/'dataflow.comp';_write_exclusive(shader,Path(__file__).with_name('half_color_load.comp' if args.load else 'half_color_store.comp').read_bytes())
    module=compile_compute(shader,output,compiler,validator)
    _write_exclusive(output/'proof.json',(json.dumps(dict(proof,spirv_validated=True,gpu_executed=False,
        input_sha256=hashlib.sha256(data).hexdigest(),expected_sha256=hashlib.sha256(expected).hexdigest(),
        module_sha256=hashlib.sha256(module).hexdigest()),indent=2)+'\n').encode())
    print(f'Validated half-color {"load" if args.load else "store"} fixture; GPU not run: {output}')


if __name__=='__main__':
    try:run()
    except (ValueError,OSError) as error:print(f'error: {error}',file=sys.stderr);raise SystemExit(2)
