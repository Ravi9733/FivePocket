#!/usr/bin/env python3
"""Captured depth vertex code + explicit native MSAA/reference alias test inputs."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'rpf7_inventory'))
from generate_rectangle_vertex_probe import load_rectangle_inputs
from rectangle_vertex_stage import translate_rectangle_vertex
from edram_depth_probe_reference import make_alias_reference
from generate_vertex_dataflow_probe import compile_compute
from rpf7_extract import _write_exclusive
from xenos_preflight_batch import _require_private_report, _require_validator, repository_root


def run(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report',type=Path);parser.add_argument('output_directory',type=Path)
    for name in ('node','offset'):parser.add_argument('--'+name,type=lambda v:int(v,0),required=True)
    parser.add_argument('--sample-mask',type=int,default=15)
    for name in ('glslc','spirv-validator'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args(argv)
    output=_require_private_report(args.output_directory,repository_root())
    compiler,validator=_require_validator(args.glslc),_require_validator(args.spirv_validator)
    code,source,_,selection=load_rectangle_inputs(args.report,args.node,args.offset)
    for register,value in ((0x2000,0xA020280),(0x2002,0x2D0),(0x2200,0x8777),(0x210D,0xFF0000),(0x2208,5)):
        state=selection['register_candidates'][str(register)]
        if not state['fully_known'] or state['value_bits']!=value:raise ValueError('exact depth-view request candidates required')
    vertex,_=translate_rectangle_vertex(code,len(source),640,360)
    expected,proof=make_alias_reference(code,source,args.sample_mask)
    output.mkdir(mode=0o700)
    for name,data in (('input.bin',source),('expected.bin',expected)):_write_exclusive(output/name,data)
    modules={}
    for name,suffix,text in (('vertex','vert',vertex),('store','comp',Path(__file__).with_name('edram_depth_store.comp').read_text()),
                            ('read','comp',Path(__file__).with_name('edram_color_read.comp').read_text())):
        folder=output/name;folder.mkdir(mode=0o700);path=folder/f'dataflow.{suffix}'
        _write_exclusive(path,text.encode());modules[name]=hashlib.sha256(compile_compute(path,folder,compiler,validator)).hexdigest()
    _write_exclusive(output/'proof.json',(json.dumps(dict(proof,selection=selection,modules_sha256=modules,
        spirv_validated=True,native_gpu_executed=False,input_sha256=hashlib.sha256(source).hexdigest(),
        expected_sha256=hashlib.sha256(expected).hexdigest()),indent=2)+'\n').encode())
    print(f'Validated explicit MSAA alias diagnostic inputs, not run: {output}')


if __name__=='__main__':
    try:run()
    except (ValueError,KeyError,IndexError,TypeError,OSError) as error:
        print(f'error: {error}',file=sys.stderr);raise SystemExit(2)
