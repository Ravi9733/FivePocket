#!/usr/bin/env python3
"""Read-only command-order candidates, NOT executed GPU register/binding state.

Predicated DRAW requests are retained as unresolved; other conditional commands
reject. Waits, invalidations, earlier draws and events remain obligations. Literal
stores and stopped LOAD sources never prove retail side effects or GPU-time data.
No runtime analyzer imports this module. Field reference: pinned Xenia
95a5c3ee250f80c3b9d139658649d9ffb6db3eec, command_processor.cc, registers.h, xenos.h.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

from analyze_stopped_pm4 import load_capture
from stopped_pm4_artifacts import ordered_packets
from register_knowledge import RegisterKnowledge
from bin_predicate import retained_bins
from coherency_wait import describe_coherency_wait,REGISTERS as COHERENCY_REGISTERS
from event_write import describe_event_write
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'rpf7_inventory'))
from vertex_arithmetic_dataflow import analyze_vertex_dataflow
from pixel_sample_dataflow import analyze_pixel_sample_dataflow
from linear_r8_plane import decode_linear_r8_descriptor
from tiled32_texture import decode_descriptor as decode_tiled32
from rpf7_extract import _write_exclusive
from xenos_preflight_batch import (BatchLimits,_prepare_inputs,_read_private_input,
    _require_private_report,repository_root)

MAX_PHYSICAL_BYTES=2*1024*1024
STATE_REGISTERS=tuple(sorted({*range(0x2000,0x2008),0x200D,*range(0x2080,0x2083),
    *range(0x2100,0x2109),0x210D,*range(0x210F,0x2115),*range(0x2180,0x2183),
    *range(0x2200,0x220C),0x2293,0x2301,0x2302,0x2312,*range(0x2318,0x231C)}))


def _word(value):
    if type(value) is not int or not 0<=value<=0xFFFFFFFF:raise ValueError('unsigned dword required')
    return value


def _physical_match(ranges,address,size):
    matches=[]
    for record in ranges:
        artifact,data=record
        start=artifact['physical_address']
        if start<=address and address+size<=start+len(data):
            matches.append((artifact,address-start,data[address-start:address-start+size]))
    if len(matches)>1:raise ValueError('ambiguous overlapping physical snapshots')
    return matches[0] if matches else None


def _range_reference(match):
    if match is None:return {'available':False,'gpu_time_contents_verified':False}
    artifact,offset,data=match
    return {'available':True,'artifact':artifact,'offset_bytes':offset,'byte_count':len(data),
        'slice_sha256':hashlib.sha256(data).hexdigest(),'gpu_time_contents_verified':False}


def analyze_requests(packets,shader_records,physical_records,initial_bin_state=None):
    """All-or-nothing bounded syntax walk. Inputs are explicit snapshot bytes."""
    if not 1<=len(shader_records)<=8 or not 0<=len(physical_records)<=8:
        raise ValueError('bounded shader/physical snapshot records required')
    shader_map={};size=0
    for artifact,data in shader_records:
        key=(artifact['node'],artifact['offset']);size+=len(data)
        if (key in shader_map or artifact['stage'] not in (0,1) or not isinstance(data,bytes) or
            not data or len(data)%12 or size>4096 or len(data)!=artifact['instruction_words']*4 or
            hashlib.sha256(data).hexdigest()!=artifact['sha256']):
            raise ValueError('invalid or duplicate shader snapshot')
        shader_map[key]=(artifact,data)
    size=0;intervals=[]
    for artifact,data in physical_records:
        start=_word(artifact['physical_address']);size+=len(data)
        if (not isinstance(data,bytes) or not data or len(data)%4 or start%4 or
            start+len(data)>0x20000000 or size>MAX_PHYSICAL_BYTES or
            len(data)!=artifact['byte_count'] or hashlib.sha256(data).hexdigest()!=artifact['sha256']):
            raise ValueError('invalid physical snapshot')
        if any(start<end and begin<start+len(data) for begin,end in intervals):
            raise ValueError('overlapping physical snapshots')
        intervals.append((start,start+len(data)))
    registers={};shaders={};used=set();draws=[];obligations=[];visited=set();words=0
    bins=retained_bins(initial_bin_state)
    bin_sources=[{'kind':'retained-completed-state-batch'} if w is not None else None for w in bins.words]
    def store(register,value,origin):
        if not 0<=register<0x8000:raise ValueError('register range exceeded')
        registers[register]=(RegisterKnowledge.assigned(value),origin)
    def record(register):
        state,origin=registers.get(register,(RegisterKnowledge(),None))
        return dict(state.metadata(),register=register,source=origin)
    def vector(register,count):return [record(register+i) for i in range(count)]
    def complete(records):
        return [r['value_bits'] for r in records] if all(r['fully_known'] for r in records) else None
    def stage_dependencies(stage,loaded):
        artifact,code=loaded
        proof=(analyze_vertex_dataflow if stage==0 else analyze_pixel_sample_dataflow)(code)
        constants=[{'constant':c,'words':vector((0x4000 if stage==0 else 0x4400)+c*4,4)}
                   for c in proof['constant_registers']]
        resources=[]
        for slot in sorted({f['fetch_slot' if stage==0 else 'slot'] for f in proof['fetches']}):
            descriptor=vector(0x4800+slot*(2 if stage==0 else 6),2 if stage==0 else 6)
            values=complete(descriptor);resource={'slot':slot,'descriptor':descriptor,'layout_supported':False}
            if values is not None:
                if stage==0:
                    a,b=values;address=a&~3;extent=((b>>2)&0xFFFFFF)*4
                    # Preserve all reserved bits; this is field extraction, not
                    # acceptance of a native vertex resource descriptor.
                    layout={'physical_address':address,'source_bytes':extent,'type_bits':a&3,
                        'endianness_bits':b&3,'reserved_bits':b&0xFC000000,'layout_only':True}
                    supported=a&3==3 and b&3==2 and 0<extent<=4096 and address+extent<=0x20000000
                else:
                    try:layout=decode_linear_r8_descriptor(values)
                    except ValueError:
                        try:layout=decode_tiled32(values)
                        except ValueError:layout=None
                    supported=layout is not None
                if layout is not None:resource['layout']=layout
                if supported:
                    extent=layout.get('source_bytes',layout.get('byte_count'))
                    resource.update(layout_supported=True,snapshot=_range_reference(
                        _physical_match(physical_records,layout['physical_address'],extent)))
            resources.append(resource)
        return {'load':artifact,'constant_candidates':constants,'resource_candidates':resources,
            'shader_execution_required_verified':False,'allocation_extent_verified':False}
    for node,packet,payload in packets:
        if type(node) is not int or not 0<=node<17 or len(packet)!=8:raise ValueError('invalid packet location')
        for value in packet:_word(value)
        for value in payload:_word(value)
        offset,kind,count,opcode,first,second,predicated,repeat=packet
        words+=count+1
        if ((node,offset) in visited or words>4096 or len(visited)>=4096 or len(payload)!=count or
            kind>3 or predicated>1 or repeat>1 or (kind!=3 and predicated)):
            raise ValueError('invalid bounded packet framing')
        visited.add((node,offset));origin={'node':node,'offset':offset,'kind':'literal-request'}
        if kind in (0,1):
            if (kind==0 and not count) or (kind==1 and count!=2):raise ValueError('invalid register packet')
            for i,value in enumerate(payload):store(first+(0 if repeat else i) if kind==0 else (first if i==0 else second),value,origin)
            continue
        if kind==2:
            if count:raise ValueError('filler has payload')
            continue
        if predicated and opcode not in (0x22,0x36):raise ValueError('conditional state/control packet unsupported')
        if opcode==0x10:continue
        if 0x60<=opcode<=0x63:
            if count!=1:raise ValueError('one literal bin word required')
            bins=bins.assign(opcode-0x60,payload[0]);bin_sources[opcode-0x60]=origin
            obligations.append(dict(origin,kind='bin-write-not-executed',word=opcode-0x60))
        elif opcode==0x21:
            if count!=3 or payload[0]&~0x1FFF:raise ValueError('only immediate REG_RMW supported')
            at,a,b=payload;old=registers.get(at,(RegisterKnowledge(),None))[0]
            registers[at]=(old.read_modify_write(a,b),dict(origin,kind='rmw-request'))
        elif opcode==0x2F:
            if count!=3:raise ValueError('invalid constant load framing')
            address,selector,size=payload
            if address%4 or not 0<size<=2048 or selector&~0x7FF or selector+size>2048 or address+size*4>0x20000000:
                raise ValueError('only canonical bounded ALU constant loads supported')
            match=_physical_match(physical_records,address,size*4)
            load_origin=dict(origin,kind='stopped-constant-load-source',snapshot=_range_reference(match))
            values=struct.unpack('>'+str(size)+'I',match[2]) if match else [None]*size
            for i,value in enumerate(values):
                at=0x4000+selector+i
                registers[at]=(RegisterKnowledge() if value is None else RegisterKnowledge.assigned(value),load_origin)
            obligations.append(dict(origin,kind='constant-load-not-executed',physical_address=address,byte_count=size*4))
        elif opcode in (0x27,0x2B):
            loaded=shader_map.get((node,offset))
            if loaded is None or count<2:raise ValueError('shader load lacks exact captured program')
            artifact,data=loaded;immediate=opcode==0x2B;stage=payload[0] if immediate else payload[0]&3
            size=payload[1];address=0 if immediate else payload[0]&~3
            if (stage>1 or not size or size%3 or size*4>4096 or size>>16 or
                (count!=size+2 if immediate else count!=2) or
                (artifact['stage'],artifact['immediate'],artifact['physical_address'],artifact['instruction_words'])!=
                (stage,immediate,address,size) or
                (immediate and data!=struct.pack('>'+str(size)+'I',*payload[2:]))):
                raise ValueError('shader capture metadata differs from command')
            if not immediate and address+size*4>0x20000000:raise ValueError('noncanonical shader source')
            used.add((node,offset));shaders[stage]=loaded
        elif opcode in (0x22,0x36):
            prefix=1 if opcode==0x22 else 0
            if count!=prefix+1 or (prefix and payload[0]):raise ValueError('only zero-query autoindex draw framing supported')
            control=payload[prefix]
            if (control&0xFF00)!=0 or (control>>6)&3!=2 or not 0<control>>16<=4096:
                raise ValueError('only bounded implicit autoindex requests supported')
            if len(draws)==64 or 0 not in shaders:raise ValueError('draw budget or missing vertex load')
            candidates={str(stage):stage_dependencies(stage,loaded) for stage,loaded in sorted(shaders.items())}
            draws.append({'node':node,'offset':offset,'index_count':control>>16,'primitive_bits':control&63,
                'source_select_bits':(control>>6)&3,'predicate_requested':bool(predicated),'predicate_result':None,
                'bin_predicate_reference':dict(bins.metadata(bool(predicated)),word_sources=list(bin_sources)),
                'register_candidates':{str(at):record(at) for at in STATE_REGISTERS},
                'last_shader_load_candidates':candidates,'prefix_obligations':list(obligations),
                'executed':False,'binding_verified':False,'gpu_time_contents_verified':False})
            obligations.append(dict(origin,kind='draw-not-executed',predicate_requested=bool(predicated)))
        elif opcode in (0x3B,0x3C,0x46,0x58):
            expected={0x3B:1,0x3C:5,0x46:1,0x58:3}[opcode]
            if count!=expected:raise ValueError('invalid opaque obligation framing')
            obligation=dict(origin,kind={0x3B:'invalidation-not-executed',0x3C:'wait-not-evaluated',
                0x46:'event-not-executed',0x58:'timestamp-not-written'}[opcode],opcode=opcode)
            if opcode==0x3C and (payload[0]&0x10)==0 and payload[1]==0xA31:
                obligation['coherency_reference']=describe_coherency_wait(payload,
                    {r:registers.get(r,(RegisterKnowledge(),None))[0] for r in COHERENCY_REGISTERS})
                obligation['coherency_register_sources']={str(r):record(r)['source'] for r in COHERENCY_REGISTERS}
                obligation['preceding_draw_requests']=[dict(node=d['node'],offset=d['offset']) for d in draws]
            elif opcode in (0x46,0x58):
                obligation['event_reference']=describe_event_write(opcode,payload)
                obligation['preceding_draw_requests']=[dict(node=d['node'],offset=d['offset']) for d in draws]
                obligation['preceding_ordering_requests']=[dict(node=o['node'],offset=o['offset'],kind=o['kind'])
                    for o in obligations if o['kind'] in ('wait-not-evaluated','event-not-executed')]
            obligations.append(obligation)
        else:raise ValueError('unknown command may alter subsequent control/state')
    if not draws or used!=set(shader_map):raise ValueError('missing draw or unmatched shader snapshot')
    return {'classification':'implementation-work','scope':'syntactic-last-assignment-candidates',
        'draws':draws,'remaining_obligations':obligations,'guest_commands_executed':False,
        'retail_register_side_effects_verified':False,'predicates_evaluated':False,'game_frame':False}


def load_dependencies(report_path):
    entry=_prepare_inputs([report_path],repository_root(),BatchLimits(max_files=1,max_file_bytes=1024*1024))[0]
    encoded=_read_private_input(entry);execution=json.loads(encoded)['execution']
    def artifacts(name,limit,max_count):
        metadata=execution[name]
        if metadata['status']!=1 or not 1<=len(metadata['artifacts'])<=max_count:raise ValueError('complete captured artifacts required')
        records=[]
        for artifact in metadata['artifacts']:
            relative=Path(artifact['path'])
            if relative.is_absolute() or '..' in relative.parts:raise ValueError('relative private artifact path required')
            data=_read_private_input(_prepare_inputs([entry.path.parent/relative],repository_root(),
                BatchLimits(max_files=1,max_file_bytes=limit))[0])
            records.append((artifact,data))
        return records
    shaders=artifacts('stopped_shader_capture',4096,8)
    physical=artifacts('stopped_physical_capture',MAX_PHYSICAL_BYTES,8)
    capture,graph=load_capture(entry.path)
    result=analyze_requests(ordered_packets(capture,graph),shaders,physical,execution.get('native_bin_state'))
    # Detect a report rewrite between the independently bounded readers.
    if _read_private_input(entry)!=encoded:raise ValueError('report changed while loading dependencies')
    return dict(result,report_sha256=hashlib.sha256(encoded).hexdigest()),shaders,physical


def run(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report',type=Path);parser.add_argument('output',type=Path)
    args=parser.parse_args(argv);output=_require_private_report(args.output,repository_root())
    result,_,_=load_dependencies(args.report)
    _write_exclusive(output,(json.dumps(result,indent=2)+'\n').encode())
    print(f"Mapped {len(result['draws'])} unexecuted draw requests. Private output: {output}")


if __name__=='__main__':
    try:run()
    except (ValueError,KeyError,IndexError,TypeError,OSError) as error:
        print(f'error: {error}',file=sys.stderr);raise SystemExit(2)
