"""Exact one-tile CPU texture snapshot profile, not a resource/sampler binding."""


def decode_descriptor(words):
    if len(words)!=6 or any(type(w) is not int or not 0<=w<=0xFFFFFFFF for w in words):
        raise ValueError('six texture descriptor dwords required')
    a,b,c,d,e,f=words
    if (a!=0x80400002 or b&0xFFF!=0x86 or c!=0x3E01F or d&~0x1FFE!=0xA80000 or e!=3 or f!=0x210):
        raise ValueError('descriptor outside explicit one-tile RGBA8 profile')
    base=b&~4095;swizzle=[(d>>(1+3*i))&7 for i in range(4)]
    if base+4096>0x20000000 or any(s>5 for s in swizzle):
        raise ValueError('invalid texture physical extent or swizzle')
    return {'physical_address':base,'byte_count':4096,'width':32,'height':32,'pitch':32,
        'format':'UNORM8_8_8_8','endianness':'8-in-32','tiled':True,'swizzle':swizzle,
        'layout_only':True,'sampler_semantics_verified':False,'gpu_executed':False}


def texel_offset(x,y):
    if any(type(v) is not int or not 0<=v<32 for v in (x,y)):
        raise ValueError('texel outside the single32x32 tile')
    # Independently expressed bank/pipe decomposition; no native helper called.
    inner=(((y//2)%8)*8+x%8)*4
    bank=(y//16)%2
    pipe=(x//8)%4 ^ (((y//8)%2)*2)
    return (inner%16)+((inner//16)%2)*32+((inner//32)%8)*256+bank*2048+pipe*64+(y%2)*16


def decode_pixels(words,source):
    plan=decode_descriptor(words)
    if not isinstance(source,bytes) or len(source)!=4096:raise ValueError('exact owned4096-byte tile required')
    output=bytearray(4096)
    for y in range(32):
        for x in range(32):
            offset=texel_offset(x,y);word=int.from_bytes(source[offset:offset+4],'big')
            for lane,select in enumerate(plan['swizzle']):
                output[(y*32+x)*4+lane]=(word>>(select*8))&255 if select<4 else 0 if select==4 else 255
    return bytes(output)
