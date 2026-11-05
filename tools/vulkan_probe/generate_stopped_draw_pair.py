#!/usr/bin/env python3
"""Select one stopped color-draw candidate for an explicit offscreen test.

Resources/constants are derived from bounded command order, not manual artifact
indices. This does not evaluate its predicate or replay title fixed-function state.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
sys.path[:0]=[str(Path(__file__).resolve().parents[1]/'host_guest_trace'),
             str(Path(__file__).resolve().parents[1]/'rpf7_inventory')]
from stopped_draw_dependencies import load_dependencies,_physical_match
from linear_r8_plane import unpack_linear_r8_plane
from tiled32_texture import decode_pixels
from half_color_reference import full_half_reference,persistent_half_reference
from generate_r8_planes_vertex_pair import make_inputs as r8_pair
from generate_texture32_vertex_pair import make_inputs as small_pair
from generate_vertex_dataflow_probe import compile_compute
from rpf7_extract import _write_exclusive
from xenos_preflight_batch import _require_private_report,_require_validator,repository_root


def select_inputs(manifest,shaders,physical,node,offset):
    if any(type(v) is not int or v<0 for v in (node,offset)):raise ValueError('nonnegative explicit draw location required')
    found=[d for d in manifest['draws'] if (d['node'],d['offset'])==(node,offset)]
    if len(found)!=1:raise ValueError('unique explicit draw request required')
    draw=found[0];mode=draw['register_candidates'][str(0x2208)]
    if not mode['fully_known'] or mode['value_bits']!=4 or (draw['primitive_bits'],draw['index_count'])!=(6,4):
        raise ValueError('only four-vertex strip/color-mode candidate supported, not depth/resolve')
    stages=draw['last_shader_load_candidates']
    if set(stages)!={'0','1'}:raise ValueError('both captured stage candidates required')
    def code(stage):
        artifact=stages[stage]['load']
        values=[data for meta,data in shaders if meta==artifact]
        if len(values)!=1:raise ValueError('shader dependency differs from validated snapshot')
        return values[0]
    def constants(stage):
        words=[]
        for constant in stages[stage]['constant_candidates']:
            if not all(w['fully_known'] for w in constant['words']):raise ValueError('unknown constant candidate')
            words.extend(w['value_bits'] for w in constant['words'])
        return struct.pack('>'+str(len(words))+'I',*words)
    def source(resource):
        if not resource['layout_supported'] or not resource.get('snapshot',{}).get('available'):
            raise ValueError('missing bounded resource snapshot/layout')
        layout=resource['layout'];match=_physical_match(physical,layout['physical_address'],
            layout.get('source_bytes',layout.get('byte_count')))
        if match is None or hashlib.sha256(match[2]).hexdigest()!=resource['snapshot']['slice_sha256']:
            raise ValueError('resource dependency differs from validated snapshot')
        return match[2]
    vertices=stages['0']['resource_candidates']
    if len(vertices)!=1 or vertices[0]['slot']!=95 or vertices[0].get('layout',{}).get('reserved_bits')!=0x10000000:
        raise ValueError('explicit observed slot95 vertex descriptor required')
    vb=source(vertices[0]);vc=constants('0');pc=constants('1');resources=stages['1']['resource_candidates']
    descriptor=lambda r:[w['value_bits'] for w in r['descriptor']]
    if len(resources)==3 and [r['slot'] for r in resources]==[0,1,2]:
        planes=[unpack_linear_r8_plane(descriptor(r),source(r)) for r in resources]
        result=r8_pair(code('0'),code('1'),vb,vc,pc,planes);profile='r8_planes_vertex_pair_test'
    elif len(resources)==1 and resources[0]['slot']==2:
        rgba=decode_pixels(descriptor(resources[0]),source(resources[0]))
        result=small_pair(code('0'),code('1'),vb,vc,pc,rgba);profile='texture32_vertex_pair_test'
    else:raise ValueError('no supported bounded texture candidate profile')
    return result,dict(draw,report_sha256=manifest['report_sha256'],native_probe_profile=profile,
        resource_selection='command-order-candidates',title_draw_executed=False)


def run(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report',type=Path);parser.add_argument('output_directory',type=Path)
    parser.add_argument('--fullsize-half-policy',action='store_true',
        help='explicit uniform-texture 1280x720 binary16 storage expectation, not retail format12')
    parser.add_argument('--half-rounding',choices=('nearest-even','toward-zero'),default=None)
    parser.add_argument('--persistent-alpha-blend',action='store_true',
        help='explicit repeated same-stage alpha blending across five load/store passes, not the four title draws')
    for option in ('node','offset'):parser.add_argument('--'+option,type=lambda v:int(v,0),required=True)
    for option in ('glslc','spirv-validator'):parser.add_argument('--'+option,type=Path,required=True)
    args=parser.parse_args(argv);output=_require_private_report(args.output_directory,repository_root())
    if args.half_rounding and not args.fullsize_half_policy:raise ValueError('half rounding requires explicit fullsize half policy')
    if args.persistent_alpha_blend and not args.fullsize_half_policy:raise ValueError('persistent blending requires fullsize half policy')
    compiler,validator=_require_validator(args.glslc),_require_validator(args.spirv_validator)
    manifest,shaders,physical=load_dependencies(args.report)
    (vertex,fragment,proof,combined,expected),selection=select_inputs(manifest,shaders,physical,args.node,args.offset)
    if args.fullsize_half_policy:
        if args.persistent_alpha_blend:
            regs=selection['register_candidates']
            for register,value in ((0x2201,0x07060706),(0x2104,15)):
                if not regs[str(register)]['fully_known'] or regs[str(register)]['value_bits']!=value:
                    raise ValueError('bounded alpha-blend equation/write-mask candidate differs')
            control=regs[str(0x2202)]
            if not control['fully_known'] or control['value_bits']&0x18:
                raise ValueError('alpha test/alpha-to-mask candidate not disabled')
        ref_function=persistent_half_reference if args.persistent_alpha_blend else full_half_reference
        expected,storage=ref_function(selection['native_probe_profile'],combined,expected,args.half_rounding or 'nearest-even')
        selection['native_probe_profile']=selection['native_probe_profile'].replace('_vertex_pair_test',
            '_persistent_half_pair_test' if args.persistent_alpha_blend else '_full_half_pair_test')
        proof=dict(proof,output_bytes=len(expected),native_viewport=[0,0,1280,720],
            native_attachment='R16G16B16A16_SFLOAT',storage_expectation=storage,
            uniform_textures_required=True,blending_enabled=args.persistent_alpha_blend,
            native_readback_bytes=7372832,retail_format12_verified=False)
    output.mkdir(mode=0o700)
    for name,data in (('input.bin',combined),('expected.bin',expected)):_write_exclusive(output/name,data)
    modules={}
    for stage,suffix,text in (('vertex','vert',vertex),('fragment','frag',fragment)):
        folder=output/stage;folder.mkdir(mode=0o700);source=folder/f'dataflow.{suffix}'
        _write_exclusive(source,text.encode())
        modules[stage]=hashlib.sha256(compile_compute(source,folder,compiler,validator)).hexdigest()
    record=dict(proof,selection=selection,modules_sha256=modules,spirv_validated=True,
        input_sha256=hashlib.sha256(combined).hexdigest(),expected_sha256=hashlib.sha256(expected).hexdigest())
    _write_exclusive(output/'proof.json',(json.dumps(record,indent=2)+'\n').encode())
    print(f"Validated selected {selection['native_probe_profile']} inputs; no GPU/title draw executed. Private output: {output}")


if __name__=='__main__':
    try:run()
    except (ValueError,KeyError,IndexError,TypeError,OSError) as error:
        print(f'error: {error}',file=sys.stderr);raise SystemExit(2)
