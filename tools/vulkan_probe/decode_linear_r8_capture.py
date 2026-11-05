#!/usr/bin/env python3
"""Decode an explicitly selected stopped R8 plane, not a runtime binding/frame."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
sys.path[:0]=[str(Path(__file__).resolve().parents[1]/'host_guest_trace'),
             str(Path(__file__).resolve().parents[1]/'rpf7_inventory')]
from analyze_stopped_pm4 import load_capture
from stopped_pm4_artifacts import ordered_packets
from linear_r8_plane import decode_linear_r8_descriptor, unpack_linear_r8_plane
from rpf7_extract import _write_exclusive
from xenos_preflight_batch import (BatchLimits, _prepare_inputs, _read_private_input,
    _require_private_report, repository_root)


def load_plane(report_path, range_index, node_index, packet_offset, register):
    if any(type(v) is not int or v<0 for v in (range_index,node_index,packet_offset,register)) or range_index>=8:
        raise ValueError('explicit bounded artifact and descriptor locations required')
    entry=_prepare_inputs([report_path],repository_root(),
        BatchLimits(max_files=1,max_file_bytes=1024*1024))[0]
    encoded=_read_private_input(entry)
    physical=json.loads(encoded)['execution']['stopped_physical_capture']
    if physical['status']!=1 or not 1<=len(physical['artifacts'])<=8:
        raise ValueError('complete stopped physical capture required')
    artifact=physical['artifacts'][range_index]
    capture,graph=load_capture(entry.path)
    matches=[]
    for node,packet,payload in ordered_packets(capture,graph):
        offset,kind,count,opcode,first,second,predicated,repeat=packet
        if node==node_index and offset==packet_offset:
            if kind!=0 or predicated or repeat or not first<=register<=first+len(payload)-6:
                raise ValueError('unconditional sequential six-word descriptor required')
            matches.append(tuple(payload[register-first:register-first+6]))
    if len(matches)!=1:raise ValueError('unique selected descriptor packet required')
    words=matches[0];proof=decode_linear_r8_descriptor(words)
    if (artifact['physical_address'],artifact['byte_count'])!=(proof['physical_address'],proof['source_bytes']):
        raise ValueError('selected allocation-owned range differs from descriptor base/extent')
    relative=Path(artifact['path'])
    if relative.is_absolute() or '..' in relative.parts:raise ValueError('relative private artifact path required')
    data=_read_private_input(_prepare_inputs([entry.path.parent/relative],repository_root(),
        BatchLimits(max_files=1,max_file_bytes=2*1024*1024))[0])
    if len(data)!=proof['source_bytes'] or hashlib.sha256(data).hexdigest()!=artifact['sha256']:
        raise ValueError('stopped plane extent/hash mismatch')
    tight=unpack_linear_r8_plane(words,data)
    histogram=[0]*256
    for value in tight:histogram[value]+=1
    return tight,dict(proof,descriptor_words=words,descriptor_source={
        'node':node_index,'packet_offset':packet_offset,'register':register},
        report_sha256=hashlib.sha256(encoded).hexdigest(),artifact=artifact,
        tight_sha256=hashlib.sha256(tight).hexdigest(),histogram=histogram,
        same_report=True,selected_descriptor_is_draw_binding_verified=False,
        gpu_execution_time_contents_verified=False,frame_presented=False)


def run(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report',type=Path);parser.add_argument('output_directory',type=Path)
    for option in ('range-index','node-index','packet-offset','register'):
        parser.add_argument('--'+option,type=lambda v:int(v,0),required=True)
    args=parser.parse_args(argv)
    output=_require_private_report(args.output_directory,repository_root())
    tight,proof=load_plane(args.report,args.range_index,args.node_index,args.packet_offset,args.register)
    output.mkdir(mode=0o700)
    _write_exclusive(output/'plane.r8',tight)
    _write_exclusive(output/'proof.json',(json.dumps(proof,indent=2)+'\n').encode())
    print(f"Decoded {proof['width']}x{proof['height']} plane; native upload not run. Private output: {output}")


if __name__=='__main__':
    try:run()
    except (ValueError,KeyError,IndexError,TypeError,OSError) as error:
        print(f'error: {error}',file=sys.stderr);raise SystemExit(2)
