"""Summarize the original position computation from ONE private draw capture.

Read-only CPU diagnostic, not native admission or an Xbox/GPU oracle. In
particular, do not interpret perspective-divided negative-W coordinates as
visible geometry, or combine constants and vertices from different captures.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path

from vertex_arithmetic_dataflow import analyze_vertex_dataflow, evaluate_vertex_dataflow
from xenos_preflight_batch import BatchLimits, _require_private_input, _read_private_input


def summarize_positions(positions, primitive):
    if primitive not in (5,13) or type(primitive) is not int:
        raise ValueError('fan or quad-list topology required')
    if not 3<=len(positions)<=16384 or (primitive==13 and len(positions)%4):
        raise ValueError('complete bounded primitive list required')
    if any(len(p)!=4 or any(type(f) not in (int,float) or not math.isfinite(f) for f in p) for p in positions):
        raise ValueError('complete finite position vectors required')
    positive=all(p[3]>0 for p in positions)
    ndc=[[p[c]/p[3] for p in positions] for c in range(3)] if positive else None
    divide_finite=ndc is not None and all(math.isfinite(v) for lane in ndc for v in lane)
    result=dict(vertex_count=len(positions),primitive=primitive,
        homogeneous_ranges=[[min(p[c] for p in positions),max(p[c] for p in positions)] for c in range(4)],
        all_positive_w=positive,all_negative_w=all(p[3]<0 for p in positions),
        positive_w_ndc_ranges=[[min(lane),max(lane)] for lane in ndc] if divide_finite else None,
        positive_w_divide_finite=divide_finite,
        classification='CPU position-only diagnostic; NO clipping or draw admission',
        gpu_executed=False,xbox_numeric_oracle=False)
    if primitive==5:result['ring_closed']=positions[1]==positions[-1]
    else:
        result['quads']=[dict(index=i//4,point_degenerate=all(p==positions[i] for p in positions[i:i+4]),
            constant_axes=[c for c in range(4) if all(p[c]==positions[i][c] for p in positions[i:i+4])],
            all_positive_w=all(p[3]>0 for p in positions[i:i+4]),all_negative_w=all(p[3]<0 for p in positions[i:i+4]))
            for i in range(0,len(positions),4)]
    return result


def capture_position_report(meta,shader,vertices,constants):
    if not isinstance(meta,dict):raise ValueError('captured metadata object required')
    count,stride,primitive=(meta.get(k) for k in ('vertex_count','stride','primitive'))
    if (type(count) is not int or not 3<=count<=16384 or type(stride) is not int or
        not 4<=stride<=256 or stride%4 or count*stride!=len(vertices) or
        len(vertices)>256*1024 or len(constants)!=4096 or type(meta.get('index_count')) is not int or meta.get('index_count')!=0):
        raise ValueError('exact bounded nonindexed captured inputs required')
    proof=analyze_vertex_dataflow(shader,dot4=True,scalar_constants=True,loading_math=True)
    if not proof['fetches'] or any(f['fetch_slot']!=95 or f['stride_words']*4!=stride for f in proof['fetches']):
        raise ValueError('original inline slot95 and matching stride required')
    vectors={i:constants[i*16:i*16+16] for i in range(256)}
    positions=[evaluate_vertex_dataflow(shader,{95:vertices},vectors,i,dot4=True,scalar_constants=True,
        loading_math=True,position_only=True)['62'] for i in range(count)]
    return summarize_positions(positions,primitive)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('capture',type=Path);args=parser.parse_args()
    root=Path(__file__).resolve().parents[2];limits=BatchLimits(max_file_bytes=256*1024)
    def read(name):return _read_private_input(_require_private_input(args.capture/name,root,limits))
    names=('draw.json','vertex-shader.bin','vertices.bin','vertex-constants.bin')
    data={name:read(name) for name in names}
    report=capture_position_report(json.loads(data['draw.json']),data['vertex-shader.bin'],data['vertices.bin'],data['vertex-constants.bin'])
    report['capture']=str(args.capture.resolve());report['input_sha256']={n:hashlib.sha256(b).hexdigest() for n,b in data.items()}
    report['tool_sha256']=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    print(json.dumps(report,indent=2,allow_nan=False))


if __name__=='__main__':main()
