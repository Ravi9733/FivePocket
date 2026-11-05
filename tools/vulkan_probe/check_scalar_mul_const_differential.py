#!/usr/bin/env python3
"""Native/offline scalar field and float32-bit agreement; no shader execution."""
import argparse
import json
from pathlib import Path
import random
import struct
import subprocess
import sys
sys.path[:0]=[str(Path(__file__).resolve().parents[1]/'rpf7_inventory'),
             str(Path(__file__).resolve().parents[1]/'rpf7_inventory/tests')]
from scalar_mul_const import decode_scalar_mul_const,evaluate_scalar_mul_const
from test_scalar_mul_const import scalar_instruction


def compare(probe,words,constant=None,temporary=None,known=15):
    if constant is None:constant=struct.pack('>4f',2,3,5,7)
    if temporary is None:temporary=struct.pack('>4f',11,13,17,19)
    try:proof=decode_scalar_mul_const(words)
    except ValueError:proof=None
    prefix=list(probe) if isinstance(probe,(list,tuple)) else [str(probe)]
    run=subprocess.run([*prefix,*(f'{w:x}' for w in words),f'{known:x}'],
        input=constant+temporary,capture_output=True,timeout=5)
    if run.returncode not in (0,1):raise ValueError(f'native scalar probe failed: {run.returncode}')
    observed=json.loads(run.stdout)
    if observed['decoded']!=(proof is not None):raise ValueError('scalar decode disagreement')
    if proof is None:return False
    if any(observed[k]!=proof[k] for k in ('constant_register','constant_component','temporary_register',
        'temporary_component','destination_register','write_mask','gpu_executed')):
        raise ValueError('scalar field disagreement')
    values=[v if known&(1<<i) else None for i,v in enumerate(struct.unpack('>4f',temporary))]
    try:
        if known&~15:raise ValueError('invalid known mask')
        value=evaluate_scalar_mul_const(words,constant,values)
    except ValueError:value=None
    bits=struct.unpack('>I',struct.pack('>f',1234 if value is None else value))[0]
    if observed['evaluated']!=(value is not None) or observed['result_bits']!=bits:
        raise ValueError('scalar evaluation/failure-preservation disagreement')
    return True


def cases():
    result=[]
    for register in range(64):
        for ca in range(4):result.append((scalar_instruction(register,register*3%256,ca,(register+ca)%4,
            (register+11)%64,(register+ca)%16),))
    base=scalar_instruction()
    for bit in range(96):
        changed=list(base);changed[bit//32]^=1<<(bit%32);result.append((changed,))
    rng=random.Random(0x5343414C)
    for _ in range(256):
        values=[rng.randrange(-100000,100001)*2.0**rng.randrange(-12,8) for _ in range(8)]
        result.append((scalar_instruction(rng.randrange(64),rng.randrange(256),rng.randrange(4),rng.randrange(4),
            rng.randrange(64),rng.randrange(16)),struct.pack('>4f',*values[:4]),struct.pack('>4f',*values[4:]),rng.randrange(32)))
    for left,right in ((0,-2),(-0.0,2),(1,float('nan')),(float('inf'),0),(1,2**-149),
        (2**-149,1),(2**-126,.5),(2**127,4),(1+2**-23,1-2**-23)):
        result.append((base,struct.pack('>4f',left,0,0,0),struct.pack('>4f',0,right,0,0)))
    return result


def run():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('native_probe',type=Path)
    probe=parser.parse_args().native_probe.resolve(strict=True);inputs=cases()
    accepted=sum(compare(probe,*case) for case in inputs)
    print(f'Scalar MUL_CONST native/offline agreement: {len(inputs)} cases ({accepted} decoded); GPU executed=0.')


if __name__=='__main__':run()
