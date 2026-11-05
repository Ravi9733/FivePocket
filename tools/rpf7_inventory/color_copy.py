"""Full-origin reference conversion into32bpp macro-tiled storage, no PM4 replay."""
import struct
from color_7e3 import unpack,value,f32
from edram_alias_clear import footprint,sample_offset,EDRAM_BYTES,word
from tiled32_texture import texel_offset


def destination_offset(width,height,x,y):
    for v in (width,height,x,y):word(v)
    if not 0<width<=4096 or width%32 or not 0<height<=4096 or x>=width or y>=height:
        raise ValueError('bounded positive32-aligned pitch and in-range pixel required')
    macro=(y//32)*(width//32)+x//32
    return macro*4096+texel_offset(x%32,y%32)


def plan_copy(source,control,base,pitch,info):
    for v in (control,base,pitch,info):word(v)
    f=footprint(*source);width,height=source[2:4]
    if (source[4] or f['samples']!=1 or width>4096 or height>4096 or width%32 or
        control!=0x100000 or base%4096 or pitch!=width|(height<<16) or info&~0x1000000!=0x300):
        raise ValueError('exact full-origin single-sample7e3 toRGBA8 copy fields required')
    capacity=width*((height+31)//32*32)*4
    if capacity>16*1024*1024 or base+capacity>0x20000000:raise ValueError('destination storage exceeds bound')
    last=max(destination_offset(width,height,x,y) for y in range((height-1)//32*32,height) for x in range(width-32,width))+4
    return {'destination_base':base,'capacity':capacity,'touched_span':last,'pixel_bytes':width*height*4,
        'padding_bytes_unwritten':capacity-width*height*4,'swap_red_blue':bool(info&0x1000000),
        'source_footprint':f,'destination_tiled':True,'endian':'none','native_gpu_executed':False,
        'destination_ownership_verified':False,'retail_copy_verified':False,'guest_writeback':False}


def convert_word(packed,swap):
    if type(swap) is not bool:raise ValueError('explicit swap policy required')
    components=[value(v) for v in unpack(packed)]
    converted=[int(f32(f32(min(1,max(0,v))*255)+.5)) for v in components]
    if swap:converted[0],converted[2]=converted[2],converted[0]
    return sum(v<<(8*i) for i,v in enumerate(converted))


def copy_to_buffer(source,control,base,pitch,info,edram):
    plan=plan_copy(source,control,base,pitch,info)
    if not isinstance(edram,bytes) or len(edram)!=EDRAM_BYTES:raise ValueError('exact owned reference EDRAM bytes required')
    out=bytearray([0x5A])*plan['capacity'];cache={}
    for y in range(source[3]):
        for x in range(source[2]):
            packed=struct.unpack_from('<I',edram,sample_offset(source,x,y,0))[0]
            if packed not in cache:cache[packed]=struct.pack('<I',convert_word(packed,plan['swap_red_blue']))
            at=destination_offset(source[2],source[3],x,y);out[at:at+4]=cache[packed]
    return bytes(out),plan
