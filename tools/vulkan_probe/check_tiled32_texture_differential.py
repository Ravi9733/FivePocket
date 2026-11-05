#!/usr/bin/env python3
"""Byte-for-byte native/offline tiled texture checks. No GPU upload or sampler."""
import argparse
from pathlib import Path
import random
import subprocess
import sys
sys.path[:0]=[str(Path(__file__).resolve().parents[1]/'rpf7_inventory'),
             str(Path(__file__).resolve().parents[1]/'rpf7_inventory/tests')]
from tiled32_texture import decode_pixels
from test_tiled32_texture import DESCRIPTOR


def compare(probe,words,source):
    try:expected=decode_pixels(words,source)
    except ValueError:expected=None
    prefix=list(probe) if isinstance(probe,(list,tuple)) else [str(probe)]
    result=subprocess.run([*prefix,*(f'{w:x}' for w in words)],input=source,capture_output=True,timeout=3)
    if result.returncode not in (0,1):raise ValueError('native texture probe failed')
    if (result.returncode==0)!=(expected is not None):raise ValueError('texture acceptance disagreement')
    if expected is not None and result.stdout!=expected:raise ValueError('texture pixel bytes disagree')
    if expected is None and result.stdout:raise ValueError('failed texture decode published a prefix')
    return expected is not None


def run():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('native_probe',type=Path)
    probe=parser.parse_args().native_probe.resolve(strict=True)
    rng=random.Random(0x713232);source=bytes(rng.randrange(256) for _ in range(4096))
    cases=[(DESCRIPTOR,source)]
    for bit in range(192):
        changed=list(DESCRIPTOR);changed[bit//32]^=1<<(bit%32);cases.append((changed,source))
    for _ in range(256):
        changed=list(DESCRIPTOR);changed[3]=(changed[3]&~0x1FFE)|(rng.randrange(4096)<<1)
        cases.append((changed,source))
    for n in (0,4092,4095,4097):cases.append((DESCRIPTOR,bytes(n)))
    accepted=sum(compare(probe,*case) for case in cases)
    print(f'Tiled32 native/offline agreement: {len(cases)} cases ({accepted} accepted); GPU uploaded=0.')


if __name__=='__main__':run()
