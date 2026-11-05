"""Prepare an ignored offline texture probe from owned complete/page captures.

Copies continuous evidence prefixes only. The native mip planner must still
validate every required extent. Does not install, launch, sample or admit a draw.
"""
import argparse
import hashlib
import json
from pathlib import Path
import stat
import struct

from submitted_texture_pages import audit_pages,captured_plane_prefix


def captured_payloads(draw,metadata,read,slot):
    if type(slot) is not int or not 0<=slot<32:raise ValueError('slot0..31 required')
    descriptors=draw.get('submitted_textures')
    if draw.get('submitted_texture_source')!='driver-upload' or not isinstance(descriptors,list) or len(descriptors)!=32:
        raise ValueError('submitted descriptor snapshot required')
    fetch=descriptors[slot]
    if (not isinstance(fetch,list) or len(fetch)!=6 or
            any(type(v) is not int or not 0<=v<=0xFFFFFFFF for v in fetch) or fetch[0]&3!=2):
        raise ValueError('submitted texture required')
    complete=[t for t in draw.get('textures',[]) if t.get('slot')==slot]
    if complete:
        if len(complete)!=1:raise ValueError('duplicate complete capture')
        texture=complete[0];size=texture.get('bytes')
        if (texture.get('fetch')!=fetch or texture.get('literal_zero') is not False or
                type(size) is not int or not 0<size<=64*1024*1024 or fetch[5]&0xFFFFF000):
            raise ValueError('exact non-mip complete submitted capture required')
        base=read(f'texture-{slot}.bin',size)
        if not isinstance(base,bytes) or len(base)!=size:raise ValueError('complete texture extent mismatch')
        mips=b'';kind='complete submitted capture; native layout still requires validation'
    else:
        audit=audit_pages(metadata,draw,read)
        base=captured_plane_prefix(audit,slot,False,read)
        mips=captured_plane_prefix(audit,slot,True,read) if fetch[5]&0xFFFFF000 else b''
        kind='audited owned page prefixes; no complete layout claim'
    return {'fetch.bin':struct.pack('>6I',*fetch),'base.bin':base,'mips.bin':mips},kind


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture',type=Path)
    parser.add_argument('output',type=Path,help='new directory under ignored local-analysis')
    parser.add_argument('--slot',type=int,required=True)
    args=parser.parse_args()
    private=Path(__file__).resolve().parents[2]/'local-analysis'
    capture=args.capture.resolve(strict=True);output=args.output.resolve()
    if (not capture.is_relative_to(private) or not output.is_relative_to(private) or
            output==private or output.exists() or not 0<=args.slot<32):
        parser.error('private input and new private output directory, slot0..31 required')
    def read(name,limit=4096):
        path=capture/name;info=path.stat()
        if (path.is_symlink() or not stat.S_ISREG(info.st_mode) or info.st_size>limit or
                getattr(info,'st_flags',0)&0x40000000):
            raise ValueError('bounded resident regular capture file required')
        return path.read_bytes()
    raw_draw=read('draw.json',65536);raw_meta=read('submitted-texture-pages.json',65536)
    draw=json.loads(raw_draw);metadata=json.loads(raw_meta)
    payloads,kind=captured_payloads(draw,metadata,read,args.slot)
    manifest={'classification':kind,
              'slot':args.slot,'capture':str(capture),'layout_verified':False,'sampling_verified':False,
              'draw_sha256':hashlib.sha256(raw_draw).hexdigest(),
              'page_metadata_sha256':hashlib.sha256(raw_meta).hexdigest(),
              'preparer_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'auditor_sha256':hashlib.sha256(Path(__file__).with_name('submitted_texture_pages.py').read_bytes()).hexdigest(),
              'payloads':{name:{'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}
                          for name,data in payloads.items()}}
    output.mkdir()
    for name,data in payloads.items():
        with (output/name).open('xb') as stream:stream.write(data)
    with (output/'manifest.json').open('x', encoding='utf-8', newline='\n') as stream:json.dump(manifest,stream,indent=2)
    print(json.dumps(manifest,indent=2))


if __name__=='__main__':main()
