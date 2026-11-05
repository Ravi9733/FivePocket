#!/usr/bin/env python3
"""Explicit native depth/alias/copy fixture; intervening title color draws omitted."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'rpf7_inventory'))
from generate_rectangle_vertex_probe import load_rectangle_inputs
from rectangle_vertex_stage import translate_rectangle_vertex,evaluate_rectangle
from edram_depth_probe_reference import make_alias_reference,BACKING_BYTES
from color_copy import plan_copy,copy_to_buffer
from generate_vertex_dataflow_probe import compile_compute
from rpf7_extract import _write_exclusive
from xenos_preflight_batch import _require_private_report,_require_validator,repository_root


def copy_selection(report):
    code,source,_,draw=load_rectangle_inputs(report,1,2760)
    exports=evaluate_rectangle(code,source,1280,720)
    if exports['source_bounds']!=[-.5,-.5,1279.5,719.5]:raise ValueError('exact full-origin copy candidate required')
    def reg(index):
        value=draw['register_candidates'][str(index)]
        if not value['fully_known']:raise ValueError('unknown copy register candidate')
        return value['value_bits']
    if reg(0x2208)!=6 or reg(0x2081)!=0 or reg(0x2082)!=0x2D00500 or reg(0x2301)!=0:
        raise ValueError('observed full-copy mode/scissor/pixel-center candidates required')
    view=(reg(0x2000),reg(0x2001),1280,720,False)
    words=[reg(i) for i in (0x2318,0x2319,0x231A,0x231B)]
    return view,words,dict(plan_copy(view,*words),selection=draw,
        vertex_source_sha256=hashlib.sha256(source).hexdigest(),source_bounds=exports['source_bounds'],
        intervening_title_color_draws_replayed=False,title_raster_state_replayed=False)


def resolve_reference(alias_reference,view,words):
    if len(alias_reference)<BACKING_BYTES:raise ValueError('complete guarded alias reference required')
    converted,proof=copy_to_buffer(view,*words,alias_reference[16:BACKING_BYTES-16])
    return alias_reference[:BACKING_BYTES]+bytes([0x5A])*16+converted+bytes([0x5A])*16,proof


def run(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report',type=Path);parser.add_argument('output_directory',type=Path)
    for name in ('glslc','spirv-validator'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args(argv)
    output=_require_private_report(args.output_directory,repository_root())
    compiler,validator=_require_validator(args.glslc),_require_validator(args.spirv_validator)
    code,source,_,depth=load_rectangle_inputs(args.report,1,234)
    for register,value in ((0x2000,0xA020280),(0x2002,0x2D0),(0x2200,0x8777),(0x210D,0xFF0000),(0x2208,5)):
        state=depth['register_candidates'][str(register)]
        if not state['fully_known'] or state['value_bits']!=value:raise ValueError('exact depth request candidates required')
    view,words,selection=copy_selection(args.report)
    alias,proof=make_alias_reference(code,source,15)
    expected,copy=resolve_reference(alias,view,words)
    vertex,_=translate_rectangle_vertex(code,len(source),640,360)
    output.mkdir(mode=0o700)
    for name,data in (('input.bin',source),('expected.bin',expected)):_write_exclusive(output/name,data)
    modules={}
    for name,suffix,text in (('vertex','vert',vertex),('store','comp',Path(__file__).with_name('edram_depth_store.comp').read_text()),
        ('read','comp',Path(__file__).with_name('edram_color_resolve.comp').read_text())):
        folder=output/name;folder.mkdir(mode=0o700);path=folder/f'dataflow.{suffix}';_write_exclusive(path,text.encode())
        modules[name]=hashlib.sha256(compile_compute(path,folder,compiler,validator)).hexdigest()
    _write_exclusive(output/'proof.json',(json.dumps(dict(proof,copy_selection=selection,copy_reference=copy,
        expected_bytes=len(expected),modules_sha256=modules,spirv_validated=True,native_gpu_executed=False,
        input_sha256=hashlib.sha256(source).hexdigest(),expected_sha256=hashlib.sha256(expected).hexdigest(),
        intervening_title_color_draws_replayed=False),indent=2)+'\n').encode())
    print(f'Validated explicit depth/alias/tiled-copy fixture; no GPU run or title-frame replay: {output}')


if __name__=='__main__':
    try:run()
    except (ValueError,KeyError,IndexError,TypeError,OSError) as error:
        print(f'error: {error}',file=sys.stderr);raise SystemExit(2)
