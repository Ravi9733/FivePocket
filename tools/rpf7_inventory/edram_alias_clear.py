"""Explicit full-tile32bpp reference backing; no executed draw or retail oracle."""
import struct
EDRAM_BYTES=10485760


def word(v):
    if type(v) is not int or not 0<=v<=0xFFFFFFFF:raise ValueError('unsigned dword required')
    return v


def footprint(surface,info,width,height,depth):
    for v in (surface,info,width,height):word(v)
    if type(depth) is not bool:raise ValueError('explicit depth/color view required')
    if (not 0<width<=8192 or not 0<height<=8192 or surface&0xC000 or surface%16384!=width or
        info&~0xF07FF):raise ValueError('unsupported exact full-tile surface fields')
    mode=(surface//65536)%4;fmt=(info//65536)%16
    if mode>2 or (fmt!=0 if depth else fmt not in (3,12)):raise ValueError('only D24S8 and32bpp floating color storage supported')
    sx=width*(2 if mode==2 else 1);sy=height*(2 if mode else 1)
    if sx%80 or sy%16:raise ValueError('whole tiles required, no edge overfill')
    base=info%2048;pitch=sx//80;rows=sy//16;count=pitch*rows
    if base+count>2048:raise ValueError('wrapping EDRAM ranges unsupported')
    return {'begin':base*5120,'bytes':count*5120,'pitch_tiles':pitch,'tile_rows':rows,'samples':2**mode,
            'reference_layout_only':True,'retail_semantics_verified':False}


def sample_offset(view,x,y,sample):
    f=footprint(*view)
    for v in (x,y,sample):word(v)
    if x>=view[2] or y>=view[3] or sample>=f['samples']:raise ValueError('sample outside bounded view')
    sx=x*(2 if f['samples']==4 else 1)+sample//2
    sy=y*(2 if f['samples']>=2 else 1)+sample%2
    row,localy=divmod(sy,16);column,localx=divmod(sx,80)
    if view[4]:localx=(localx+40)%80
    return f['begin']+(row*f['pitch_tiles']+column)*5120+(localy*80+localx)*4


def canonical_depth_bits(n):
    word(n)
    if n>=2**24:raise ValueError('24-bit depth required')
    return struct.unpack('>I',struct.pack('>f',(n+(n//2**23))/2**24))[0]


def pack_canonical_depth(bits,stencil):
    word(bits);word(stencil)
    if bits>0x3F800000 or stencil>255:raise ValueError('positive canonical depth and8-bit stencil required')
    value=struct.unpack('>f',struct.pack('>I',bits))[0];scaled=value*2**24
    if not scaled.is_integer() or scaled==2**23:raise ValueError('not an exact canonical UNORM24 clear value')
    n=int(scaled)-(1 if scaled>2**23 else 0)
    if canonical_depth_bits(n)!=bits:raise ValueError('noncanonical depth bits')
    return n*256+stencil


def plan_alias(depth,color,bits,stencil):
    d=footprint(*depth);c=footprint(*color);packed=pack_canonical_depth(bits,stencil)
    if not depth[4] or color[4] or (d['begin'],d['bytes'])!=(c['begin'],c['bytes']) or packed!=0xC0000000:
        raise ValueError('exact aliased opaque-black full-tile views required')
    return {'begin':d['begin'],'bytes':d['bytes'],'word':packed,'color_rgba':[0,0,0,1],
        'depth_view':d,'color_view':c,'reference_layout_only':True,'native_gpu_executed':False,'guest_writeback':False}
