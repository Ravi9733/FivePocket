"""Audit bounded submitted texture page evidence, NOT mip layout or ownership.

Checks file sizes and submitted-address identity. Actual allocation ownership
was enforced by the capture runtime, not independently proved by this reader.
No game/source mutation, texture decoding, sampling or device access.
"""
import argparse
import hashlib
import json
from pathlib import Path
import stat


def audit_pages(metadata, draw, read):
    if (metadata.get('version') != 1 or metadata.get('source') != 'submitted-fetch-owned-pages' or
            metadata.get('layout_verified') is not False or metadata.get('complete_texture') is not False or
            metadata.get('page_bytes') != 4096 or metadata.get('pages_per_plane_limit') != 16 or
            metadata.get('total_byte_limit') != 4194304):
        raise ValueError('unsupported bounded owned-page capture contract')
    descriptors = draw.get('submitted_textures')
    if draw.get('submitted_texture_source') != 'driver-upload' or not isinstance(descriptors,list) or len(descriptors)!=32:
        raise ValueError('exact submitted descriptor snapshot required')
    planes = metadata.get('planes')
    if not isinstance(planes,list) or len(planes)>64:
        raise ValueError('invalid plane inventory')
    result = {'classification':'bounded storage evidence, not complete textures',
              'layout_verified':False,'sampling_verified':False,'planes':[],'issues':[]}
    seen=set();total=0;unavailable=0
    for plane in planes:
        slot,mip,address=plane.get('slot'),plane.get('mip'),plane.get('address')
        if (type(slot) is not int or not 0<=slot<32 or type(mip) is not bool or
                type(address) is not int or not 0<address<=0xFFFFF000 or address%4096 or (slot,mip) in seen):
            raise ValueError('invalid/duplicate plane identity')
        seen.add((slot,mip));fetch=descriptors[slot]
        if (not isinstance(fetch,list) or len(fetch)!=6 or
                any(type(w) is not int or not 0<=w<=0xFFFFFFFF for w in fetch) or fetch[0]&3!=2 or
                address!=(fetch[5 if mip else 1]&0xFFFFF000)):
            raise ValueError('plane does not match submitted texture address')
        sizes=plane.get('pages');truncated=plane.get('address_space_truncated')
        if (not isinstance(sizes,list) or len(sizes)!=16 or type(truncated) is not bool or
                any(type(n) is not int or n not in (0,4096) for n in sizes)):
            raise ValueError('invalid page sizes')
        expected_truncation=address+65536>0x100000000
        if truncated!=expected_truncation:raise ValueError('incorrect address-space truncation')
        row={'slot':slot,'mip':mip,'address':address,'captured_pages':[],
             'unavailable_pages':[],'outside_address_space':[],'page_sha256':{}}
        for index,size in enumerate(sizes):
            if address+index*4096+4096>0x100000000:
                if size:raise ValueError('page address wraps guest namespace')
                row['outside_address_space'].append(index);continue
            if not size:
                row['unavailable_pages'].append(index);unavailable+=1;continue
            name=f'raw-texture-{slot}-{"mip" if mip else "base"}-page-{index}.bin'
            data=read(name)
            if not isinstance(data,bytes) or len(data)!=4096:
                raise ValueError('captured page has wrong extent')
            row['captured_pages'].append(index);row['page_sha256'][str(index)]=hashlib.sha256(data).hexdigest();total+=size
        result['planes'].append(row)
    if metadata.get('captured_bytes')!=total or metadata.get('unavailable_pages')!=unavailable:
        raise ValueError('capture totals disagree with page inventory')
    result.update(captured_bytes=total,unavailable_pages=unavailable,
                  ownership_enforced_by_capture=True,ownership_independently_verified=False)
    return result


def captured_plane_prefix(audit, slot, mip, read):
    """Return only the continuous captured prefix; never fill an absent page.

    Recheck each page against the audit hash so a changed input cannot silently
    be reused. This is bounded evidence, not a declaration of texture extent.
    """
    matches=[p for p in audit['planes'] if p['slot']==slot and p['mip']==mip]
    if len(matches)!=1:raise ValueError('one audited plane required')
    plane=matches[0];parts=[]
    for index in range(16):
        if index not in plane['captured_pages']:break
        name=f'raw-texture-{slot}-{"mip" if mip else "base"}-page-{index}.bin'
        data=read(name)
        if (not isinstance(data,bytes) or len(data)!=4096 or
                hashlib.sha256(data).hexdigest()!=plane['page_sha256'].get(str(index))):
            raise ValueError('page changed after audit')
        parts.append(data)
    return b''.join(parts)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('capture',type=Path)
    args=parser.parse_args();root=Path(__file__).resolve().parents[2];capture=args.capture.resolve()
    if not capture.is_relative_to(root/'local-analysis'):parser.error('private local-analysis capture required')
    def read(name,limit=4096):
        p=capture/name;info=p.stat()
        if p.is_symlink() or not stat.S_ISREG(info.st_mode) or info.st_size>limit or getattr(info,'st_flags',0)&0x40000000:
            raise ValueError('bounded resident regular file required')
        return p.read_bytes()
    metadata=json.loads(read('submitted-texture-pages.json',65536));draw=json.loads(read('draw.json',65536))
    result=audit_pages(metadata,draw,read);result['capture']=str(capture)
    result['audit_sha256']=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    print(json.dumps(result,indent=2))


if __name__=='__main__':main()
