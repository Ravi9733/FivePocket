#!/usr/bin/env python3
"""Independent packed-fetch field and float32-bit agreement, never GPU execution."""
import argparse
import json
from pathlib import Path
import random
import struct
import subprocess
import sys
sys.path[:0] = [str(Path(__file__).resolve().parents[1]/'rpf7_inventory'),
               str(Path(__file__).resolve().parents[1]/'rpf7_inventory/tests')]
from raw_shader_analysis import _float_vertex_fetch, RawShaderError
from unorm8_vertex_fetch import decode_unorm8_fetch, read_unorm8_fetch
from test_unorm8_vertex_fetch import packed
from test_raw_vertex_fetch_analysis import fetch


def compare(probe, words, previous=None, data=None, index=0):
    if data is None: data = bytes(range(144))
    registers = [[f'initial_r{r}.{c}' for c in 'xyzw'] for r in range(64)]
    prior = None
    if previous:
        if (previous[1]>>16)&63 == 6: _,prior = decode_unorm8_fetch(previous)
        else: _,prior = _float_vertex_fetch(previous,None,registers,0)
    try: expected,_ = decode_unorm8_fetch(words,prior,registers,1)
    except RawShaderError: expected = None
    command = list(probe) if isinstance(probe,(list,tuple)) else [str(probe)]
    run = subprocess.run([*command,f'{index:x}',*(f'{w:x}' for w in words),
        *(f'{w:x}' for w in (previous or []))],input=data,capture_output=True,timeout=3)
    if run.returncode not in (0,1): raise ValueError('native packed probe failed')
    observed = json.loads(run.stdout)
    if observed['accepted'] != bool(expected): raise ValueError('packed decode disagreement')
    if expected is None: return False
    src,axis = expected['index_expression'].removeprefix('initial_r').split('.')
    fields = {'slot':expected['fetch_slot'],'stride':expected['stride_words'],'offset':expected['offset_words'],
        'src':int(src),'src_component':'xyzw'.index(axis),'dst':expected['destination'],
        'components':4,'mini':expected['mini'],'gpu_executed':False}
    if any(observed[k] != v for k,v in fields.items()): raise ValueError('packed field disagreement')
    try: result = read_unorm8_fetch(expected,data,index)
    except RawShaderError: result = None
    if observed['read'] != (result is not None): raise ValueError('packed bounds disagreement')
    if result is not None:
        known = sum(1<<i for i,v in enumerate(result) if v is not None)
        bits = [struct.unpack('>I',struct.pack('>f',v or 0.0))[0] for v in result]
        if observed['known'] != known or observed['value_bits'] != bits:
            raise ValueError('packed known-lane/float32 bit disagreement')
    return True


def run():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('native_probe',type=Path)
    probe=parser.parse_args().native_probe.resolve(strict=True)
    cases=[];full=fetch(slot=95,stride=9)
    for base,previous in ((packed(stride=1),None),(packed(stride=0,offset=6,mini=True),full)):
        cases.append((base,previous,None,0))
        for bit in range(96):
            changed=list(base);changed[bit//32]^=1<<(bit%32);cases.append((changed,previous,None,0))
    for lane in range(4):
        for value in range(256): cases.append((packed(stride=1),None,(value<<(lane*8)).to_bytes(4,'big'),0))
    rng=random.Random(0xA8A8)
    for _ in range(256):
        cases.append((packed(slot=rng.randrange(96),src=rng.randrange(64),dst=rng.randrange(64),
            stride=rng.randrange(1,256),offset=rng.randrange(8),swizzle=rng.randrange(4096)),None,None,rng.randrange(4)))
    for source,index in ((b'',0),(b'x',0),(bytes(4100),0),(bytes(4),1),(bytes(4),0xFFFFFF),(bytes(4),0x1000000)):
        cases.append((packed(stride=1),None,source,index))
    accepted=sum(compare(probe,*case) for case in cases)
    print(f'Unorm8 fetch native/offline agreement: {len(cases)} cases ({accepted} decoded); GPU executed=0.')


if __name__=='__main__':run()
