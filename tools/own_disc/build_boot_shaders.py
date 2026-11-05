#!/usr/bin/env python3
"""Recreate the fixed boot modules from disc FXCs and the decoded executable.

Configuration holds hashes, extents and vertex-declaration fields only. No
captured code or reference module is a generation input. Retains all original
bounded translators and GLSL compilation/validation guards.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import struct
import sys
import time
from build_shader_pack import sha, write_json, validate_inventory

sys.dont_write_bytecode=True
# Field layout from XenosRecomp shader_code.h VertexFetchInstruction. Only
# canonical bind-time fields can change; opcode/source/ALU/control flow cannot.
FIELDS={
 'destination':(0,12,7),'constant_index':(0,20,5),'constant_select':(0,25,2),'prefetch':(0,27,3),
 'swizzle':(1,0,12),'format_comp_all':(1,12,1),'number_format':(1,13,1),'signed_rf_mode':(1,14,1),
 'index_rounded':(1,15,1),'format':(1,16,6),'reserved':(1,22,2),'exponent_adjust':(1,24,6),
 'mini_fetch':(1,30,1),'predicated':(1,31,1),'stride':(2,0,8),'offset':(2,8,23),'predicate_condition':(2,31,1)}


def vertex_slots(code):
    from generic_shader_pack import EXEC_OPCODES
    if not code or len(code)%12:raise ValueError('microcode framing refused')
    words=struct.unpack(f'>{len(code)//4}I',code);end=len(words)//3;slot=0;addresses=set()
    while slot<end:
        a,b,c=words[slot*3:slot*3+3]
        for lo,hi in ((a,b&65535),((b>>16)|((c<<16)&0xffffffff),c>>16)):
            if (hi>>12)&15 in EXEC_OPCODES:
                address,count,sequence=lo&4095,(lo>>12)&7,(lo>>16)&4095
                if address:end=min(end,address)
                if address+count>len(words)//3:raise ValueError('boot fetch clause extent refused')
                addresses.update(address+i for i in range(count) if sequence>>(2*i)&1)
        slot+=1
    return sorted(a for a in addresses if words[a*3]&31==0)


def binding_metadata(code):
    words=struct.unpack(f'>{len(code)//4}I',code)
    return [dict(address=a,**{name:(words[a*3+word]>>shift)&((1<<width)-1)
                for name,(word,shift,width) in FIELDS.items()}) for a in vertex_slots(code)]


def bind_vertex(code,bindings):
    from generic_shader_pack import canonical_microcode
    if [r['address'] for r in bindings]!=vertex_slots(code):raise ValueError('binding fetch selection refused')
    words=list(struct.unpack(f'>{len(code)//4}I',code))
    for row in bindings:
        if set(row)!=set(FIELDS)|{'address'}:raise ValueError('binding field set refused')
        for name,(word,shift,width) in FIELDS.items():
            value=row[name]
            if type(value) is not int or not 0<=value<(1<<width):raise ValueError('binding field range refused')
            at=row['address']*3+word;mask=((1<<width)-1)<<shift
            words[at]=(words[at]&~mask)|(value<<shift)
    result=struct.pack(f'>{len(words)}I',*words)
    if canonical_microcode(result)!=canonical_microcode(code):raise ValueError('binding changed shader instructions')
    return result


def source_for(item,inventory,items,image):
    import generic_shader_pack as g
    if item['source']=='decoded-image':
        offset,size=item['offset'],item['bytes']
        if offset<0 or size<=0 or offset+size>len(image):raise ValueError('image shader extent refused')
        result=image[offset:offset+size];provenance=dict(source='decoded-image',offset=offset,bytes=size)
    elif item['source']=='disc-fxc':
        matches=[r for r in items if r['canonical_sha256']==item['canonical_sha256'] and r['stage']==item['stage']]
        if not matches:raise ValueError('boot shader absent from disc inventory')
        candidates=[]
        for row in matches:
            raw,*_=g.parse_container((inventory/row['file']).read_bytes(),row['stage'])
            if item.get('bindings') is not None:raw=bind_vertex(raw,item['bindings'])
            candidates.append(raw)
        if any(raw!=candidates[0] for raw in candidates):raise ValueError('boot shader selection ambiguous')
        result=candidates[0];provenance=dict(source='disc-fxc',containers=[r['sha256'] for r in matches],
                                             origins=[o for r in matches for o in r['origins']])
    else:raise ValueError('unknown boot source')
    if len(result)!=item['bytes'] or sha(result)!=item['guest_sha256']:raise ValueError('boot producer identity refused')
    return result,provenance


def shader_source(role,raw,settings):
    from rectangle_vertex_stage import translate_rectangle_vertex
    from vertex_arithmetic_dataflow import translate_color1280_vertex_stage
    from pixel_sample_dataflow import translate_r8_planes_pixel_fragment,translate_texture32_pixel_fragment
    if role=='depth/vertex':return translate_rectangle_vertex(raw,settings['vertex_bytes'],settings['width'],settings['height'])
    if role in ('r8/vertex','small/vertex'):
        return translate_color1280_vertex_stage(raw,settings['vertex_bytes'],settings['base_bytes'],r8_planes=role.startswith('r8/'))
    if role=='r8/fragment':return translate_r8_planes_pixel_fragment(raw,vertex_tint=True)
    if role=='small/fragment':return translate_texture32_pixel_fragment(raw,vertex_tint=True)
    raise ValueError('unknown boot role')


def build(args):
    from xenos_preflight_batch import _prepare_inputs,_read_private_input,BatchLimits
    from generate_vertex_dataflow_probe import compile_compute
    start=time.monotonic();config=json.loads(args.config.read_text())
    inventory,items=validate_inventory(args.inventory)
    image=_read_private_input(_prepare_inputs([args.image],args.repo,BatchLimits(max_files=1,max_file_bytes=40*1024**2))[0])
    if sha(image)!=config['image_sha256']:raise ValueError('decoded image identity refused')
    paths=[args.config,args.inventory/'inventory.json',args.glslc,args.spirv_validator,Path(__file__),Path(__file__).with_name('build_shader_pack.py')]
    sources={str(p.resolve()):sha(p.read_bytes()) for p in paths}
    # Declare full project helper directories, preserving transitive parser/compiler rules.
    for folder in ('tools/rpf7_inventory','tools/vulkan_probe'):
        for p in sorted((args.repo/folder).glob('*.py')):sources[str(p)]=sha(p.read_bytes())
    for p in sorted((args.repo/'tools/vulkan_probe').glob('*.comp')):sources[str(p)]=sha(p.read_bytes())
    pending=[]
    for item in config['modules']:
        raw,provenance=source_for(item,args.inventory,items,image)
        source,proof=shader_source(item['role'],raw,item['settings'])
        pending.append((item,raw,source,proof,provenance))
    if len(pending)!=5 or len({item['role'] for item,*_ in pending})!=5:raise ValueError('boot module role set refused')
    if args.check:print('BOOT_CHECK PASS modules',len(pending),'companions',len(pending));return 0
    args.output.mkdir(parents=True,exist_ok=False);write_json(args.output/'run-inputs.json',dict(sources=sources,image_sha256=sha(image)))
    result=[]
    for item,raw,source,proof,provenance in pending:
        folder=args.output/item['role'];folder.mkdir(parents=True)
        (folder/'guest-code.bin').write_bytes(raw)
        path=folder/('dataflow.vert' if item['stage']=='vertex' else 'dataflow.frag');path.write_text(source, encoding='utf-8', newline='\n')
        blob=compile_compute(path,folder,args.glslc,args.spirv_validator)
        result.append(dict(role=item['role'],guest_bytes=len(raw),guest_sha256=sha(raw),module_bytes=len(blob),
          module_sha256=sha(blob),expected_module_sha256=item['module_sha256'],same=sha(blob)==item['module_sha256'],
          provenance=provenance,proof=proof))
    # Complete the same boot directory with the four authored compute modules.
    authored=[]
    for role,filename in [('store','half_color_store.comp'),('copy','edram_color_resolve.comp'),('load','half_color_load.comp'),('depth/store','edram_depth_store.comp')]:
        folder=args.output/role;folder.mkdir(parents=True);path=folder/'dataflow.comp'
        path.write_bytes((args.repo/'tools/vulkan_probe'/filename).read_bytes())
        blob=compile_compute(path,folder,args.glslc,args.spirv_validator)
        authored.append(dict(role=role,module_bytes=len(blob),module_sha256=sha(blob),same=sha(blob)==config['authored'][role]))
    if any(sha(Path(p).read_bytes())!=h for p,h in sources.items()):raise ValueError('boot inputs changed')
    complete=all(r['same'] for r in result+authored)
    write_json(args.output/'report.json',dict(status='complete' if complete else 'different',modules=result,authored=authored,
       sources=sources,image_sha256=sha(image),gpu_execution=False,elapsed_seconds=time.monotonic()-start))
    print('BOOT_RESULT', 'PASS' if complete else 'DIFFERENT','translated_same',sum(r['same'] for r in result),
          'guest_same',len(result),'authored_same',sum(r['same'] for r in authored))
    return 0 if complete else 1


def main():
    os.umask(0o077);p=argparse.ArgumentParser(description=__doc__)
    for name in ('repo','inventory','image','config','glslc','spirv-validator','output'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--check',action='store_true');args=p.parse_args()
    for name,value in vars(args).items():
        if isinstance(value,Path):setattr(args,name,value.resolve())
    for folder in ('tools/own_disc','tools/iter','tools/rpf7_inventory','tools/vulkan_probe'):sys.path.insert(0,str(args.repo/folder))
    from disc_builder import check_private_path
    check_private_path(args.output,'boot shader output')
    return build(args)

if __name__=='__main__':
    try:raise SystemExit(main())
    except (ValueError,OSError):
        print('BOOT_REFUSED; private partial output preserved',file=sys.stderr);raise SystemExit(2)
