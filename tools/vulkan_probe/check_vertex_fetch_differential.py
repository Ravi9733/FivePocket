#!/usr/bin/env python3
"""Independent CPU float-fetch checks. Synthetic data in pipes, never a GPU draw."""
import argparse
import json
import math
from pathlib import Path
import random
import struct
import subprocess
import sys
sys.path[:0]=[str(Path(__file__).resolve().parents[1]/'rpf7_inventory'),
             str(Path(__file__).resolve().parents[1]/'rpf7_inventory/tests')]
from raw_shader_analysis import _float_vertex_fetch,RawShaderError
from test_raw_vertex_fetch_analysis import fetch


def compare(probe,words,previous=None,data=None,index=0):
    if data is None:data=struct.pack('>28f',*range(28))
    registers=[[f'initial_r{i}.{c}' for c in 'xyzw'] for i in range(64)]
    prior=None
    if previous:_,prior=_float_vertex_fetch(previous,None,registers,0)
    try:expected,_=_float_vertex_fetch(words,prior,registers,1)
    except RawShaderError:expected=None
    prefix=list(probe) if isinstance(probe,(list,tuple)) else [str(probe)]
    command=[*prefix,f'{index:x}',*(f'{w:x}' for w in words),*(f'{w:x}' for w in (previous or []))]
    r=subprocess.run(command,input=data,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=3)
    if r.returncode not in (0,1):raise ValueError('native probe failed')
    observed=json.loads(r.stdout)
    if observed['accepted']!=bool(expected):raise ValueError('native/offline decode disagreement')
    if expected is None:return False
    source,axis=expected['index_expression'].removeprefix('initial_r').split('.')
    fields={'slot':expected['fetch_slot'],'stride':expected['stride_words'],'offset':expected['offset_words'],
        'dst':expected['destination'],'src':int(source),'src_component':'xyzw'.index(axis),
        'components':expected['components'],'mini':expected['mini'],'gpu_executed':False}
    if any(observed[k]!=v for k,v in fields.items()):raise ValueError('native/offline field disagreement')
    start=(index*expected['stride_words']+expected['offset_words'])*4
    valid=0<len(data)<=4096 and len(data)%4==0 and index<0x1000000 and start+expected['components']*4<=len(data)
    values=struct.unpack_from('>'+str(expected['components'])+'f',data,start) if valid else ()
    valid=valid and all(math.isfinite(v) for v in values)
    if observed['read']!=valid:raise ValueError('native/offline bound or finite-input disagreement')
    if valid:
        known=0;result=[0.0]*4
        for i,s in enumerate(expected['destination_swizzle']):
            if s==7:continue
            known|=1<<i;result[i]=values[s] if s<4 else float(s-4)
        if observed['known']!=known or any(not math.isclose(a,b,rel_tol=1e-8,abs_tol=1e-12) for a,b in zip(result,observed['values'])):
            raise ValueError('native/offline fetched result disagreement')
    return True


def run():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('native_probe',type=Path)
    args=parser.parse_args();probe=args.native_probe.resolve(strict=True)
    cases=[];full=fetch();mini=fetch(dst=0,stride=0,offset=3,fmt=38,swizzle=0x688,mini=True)
    for base,previous in ((full,None),(mini,full)):
        cases.append((base,previous,None,0))
        for bit in range(96):
            changed=list(base);changed[bit//32]^=1<<(bit%32);cases.append((changed,previous,None,0))
    rng=random.Random(0xF37C4)
    for _ in range(256):
        cases.append((fetch(slot=rng.randrange(96),src=rng.randrange(64),dst=rng.randrange(64),
            stride=rng.randrange(1,256),offset=rng.randrange(8),fmt=rng.choice([36,37,38,57]),
            swizzle=rng.randrange(4096)),None,None,rng.randrange(4)))
    for index in (0,3,0xFFFFFF,0x1000000):cases.append((full,None,None,index))
    for data in (b'',b'x',b'0'*4100,struct.pack('>3f',float('nan'),1,2),struct.pack('>3f',float('inf'),1,2)):
        cases.append((full,None,data,0))
    accepted=sum(compare(probe,*case) for case in cases)
    print(f'Float fetch native/offline agreement: {len(cases)} cases ({accepted} decoded); GPU executed=0.')

if __name__=='__main__':run()
