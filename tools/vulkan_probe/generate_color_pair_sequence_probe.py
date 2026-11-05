#!/usr/bin/env python3
"""Two command-selected color stage pairs in one explicit shared-target test."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
from generate_stopped_draw_pair import select_inputs,load_dependencies
from half_color_reference import mixed_half_reference,sequence_store_copy_reference,uniform_initial_edram,BLEND_PRECISIONS
from generate_edram_depth_resolve_probe import copy_selection
from generate_rectangle_vertex_probe import load_rectangle_inputs
from rectangle_vertex_stage import translate_rectangle_vertex
from edram_depth_probe_reference import uniform_depth_initial_pixel
from generate_vertex_dataflow_probe import compile_compute
from rpf7_extract import _write_exclusive
from xenos_preflight_batch import (BatchLimits,_prepare_inputs,_read_private_input,
    _require_private_report,_require_validator,repository_root)


def sequence_inputs(manifest,shaders,physical,node,first_offset,second_offset,rounding,initial_pixel=None,blend_precision='float32'):
    if first_offset>=second_offset:raise ValueError('ordered explicit color candidate offsets required')
    pairs=[];selections=[]
    for offset,profile in ((first_offset,'r8_planes_vertex_pair_test'),(second_offset,'texture32_vertex_pair_test')):
        result,selection=select_inputs(manifest,shaders,physical,node,offset)
        if selection['native_probe_profile']!=profile:raise ValueError('ordered R8/small color profiles required')
        regs=selection['register_candidates']
        for reg,value in ((0x2201,0x07060706),(0x2104,15),(0x2001,0xC02D0),(0x2000,0x14000500)):
            if not regs[str(reg)]['fully_known'] or regs[str(reg)]['value_bits']!=value:
                raise ValueError('shared target/blend/mask candidate differs from bounded sequence')
        cc=regs[str(0x2202)]
        if not cc['fully_known'] or cc['value_bits']&0x18:raise ValueError('alpha test/to-mask not disabled')
        pairs.append(result);selections.append(selection)
    expected,storage=mixed_half_reference(pairs[0][3],pairs[0][4],pairs[1][3],pairs[1][4],rounding,initial_pixel,blend_precision)
    return pairs,expected,dict(storage,selections=selections,native_profile='color_pair_sequence_test',
        native_target='R16G16B16A16_SFLOAT',native_extent=[1280,720],
        native_readback_bytes=7372832,reference_bytes=len(expected),
        depth_alias_initialized=False,retail_format12_verified=False,predicates_executed=False)


def run(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report',type=Path);parser.add_argument('output_directory',type=Path)
    for name in ('node','first-offset','second-offset'):parser.add_argument('--'+name,type=lambda v:int(v,0),required=True)
    parser.add_argument('--half-rounding',choices=('nearest-even','toward-zero'),required=True)
    parser.add_argument('--blend-precision',choices=BLEND_PRECISIONS,default='float32',help='explicit diagnostic arithmetic hypothesis, not a detected hardware policy')
    parser.add_argument('--with-store-copy',action='store_true',help='connect actual native color output to guarded packed-store and tiled-copy compute')
    parser.add_argument('--initial-edram',type=Path,help='explicit uniform initial EDRAM fixture, not a live native depth result')
    parser.add_argument('--depth-offset',type=lambda v:int(v,0),help='connect the selected full-view native depth pass before the color pairs')
    for name in ('glslc','spirv-validator'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args(argv);output=_require_private_report(args.output_directory,repository_root())
    compiler,validator=_require_validator(args.glslc),_require_validator(args.spirv_validator)
    manifest,shaders,physical=load_dependencies(args.report)
    initial_data=None;initial_pixel=None;initial_proof={}
    depth=None
    if args.depth_offset is not None:
        if args.initial_edram or not args.with_store_copy or args.depth_offset>=args.first_offset:
            raise ValueError('native depth requires ordered depth/color offsets and store/copy, without CPU EDRAM input')
        depth_code,depth_source,_,depth_selection=load_rectangle_inputs(args.report,args.node,args.depth_offset)
        for register,value in ((0x2000,0xA020280),(0x2002,0x2D0),(0x2200,0x8777),(0x210D,0xFF0000),(0x2208,5)):
            state=depth_selection['register_candidates'][str(register)]
            if not state['fully_known'] or state['value_bits']!=value:raise ValueError('exact depth-view candidates required')
        vertex,_=translate_rectangle_vertex(depth_code,len(depth_source),640,360)
        initial_pixel,initial_proof=uniform_depth_initial_pixel(depth_code,depth_source)
        depth=(vertex,depth_source,depth_selection)
    if args.initial_edram:
        if not args.with_store_copy:raise ValueError('initial EDRAM requires connected store/copy profile')
        path=_prepare_inputs([args.initial_edram],repository_root(),BatchLimits(max_files=1,max_file_bytes=10485792))[0]
        initial_data=_read_private_input(path);initial_pixel,initial_proof=uniform_initial_edram(initial_data)
    pairs,expected,proof=sequence_inputs(manifest,shaders,physical,args.node,args.first_offset,args.second_offset,args.half_rounding,initial_pixel,args.blend_precision)
    if args.with_store_copy:
        _,_,selection=copy_selection(args.report)
        expected,export=sequence_store_copy_reference(expected)
        proof=dict(proof,store_copy_reference=export,copy_selection=selection,
            native_profile='color_pair_sequence_resolve_test',reference_bytes=len(expected))
    if initial_data is not None:
        proof=dict(proof,initialization=initial_proof,native_profile='color_pair_initialized_test',
            initial_edram_sha256=hashlib.sha256(initial_data).hexdigest())
    if depth is not None:
        proof=dict(proof,initialization=initial_proof,native_profile='color_pair_depth_initialized_test',
            depth_alias_initialized=True,live_depth_connected=True)
    output.mkdir(mode=0o700);_write_exclusive(output/'expected.bin',expected);stages=[]
    if initial_data is not None:_write_exclusive(output/'initial-edram.bin',initial_data)
    for name,pair in zip(('r8','small'),pairs):
        vertex,fragment,pair_proof,source,_=pair;folder=output/name;folder.mkdir(mode=0o700)
        _write_exclusive(folder/'input.bin',source);modules={}
        for stage,suffix,text in (('vertex','vert',vertex),('fragment','frag',fragment)):
            stage_dir=folder/stage;stage_dir.mkdir(mode=0o700);path=stage_dir/f'dataflow.{suffix}'
            _write_exclusive(path,text.encode());modules[stage]=hashlib.sha256(compile_compute(path,stage_dir,compiler,validator)).hexdigest()
        stages.append(dict(pair_proof,input_sha256=hashlib.sha256(source).hexdigest(),modules_sha256=modules))
    compute={}
    if args.with_store_copy:
        programs=[('store','half_color_store.comp'),('copy','edram_color_resolve.comp')]
        if initial_pixel is not None:programs.append(('load','half_color_load.comp'))
        for name,filename in programs:
            folder=output/name;folder.mkdir(mode=0o700);path=folder/'dataflow.comp'
            _write_exclusive(path,Path(__file__).with_name(filename).read_bytes())
            compute[name]=hashlib.sha256(compile_compute(path,folder,compiler,validator)).hexdigest()
    if depth is not None:
        vertex,source,selection=depth;folder=output/'depth';folder.mkdir(mode=0o700)
        _write_exclusive(folder/'input.bin',source);modules={}
        for name,suffix,content in (('vertex','vert',vertex),('store','comp',Path(__file__).with_name('edram_depth_store.comp').read_text())):
            stage=folder/name;stage.mkdir(mode=0o700);path=stage/f'dataflow.{suffix}'
            _write_exclusive(path,content.encode());modules[name]=hashlib.sha256(compile_compute(path,stage,compiler,validator)).hexdigest()
        proof=dict(proof,depth=dict(selection=selection,input_sha256=hashlib.sha256(source).hexdigest(),modules_sha256=modules))
    record=dict(proof,pairs=stages,compute_modules_sha256=compute,expected_sha256=hashlib.sha256(expected).hexdigest(),spirv_validated=True,gpu_executed=False)
    _write_exclusive(output/'proof.json',(json.dumps(record,indent=2)+'\n').encode())
    print(f'Validated both selected color pairs; GPU not executed. Private output: {output}')


if __name__=='__main__':
    try:run()
    except (ValueError,KeyError,IndexError,TypeError,OSError) as error:
        print(f'error: {error}',file=sys.stderr);raise SystemExit(2)
