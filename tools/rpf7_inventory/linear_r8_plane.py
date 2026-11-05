"""Two exact observed linear R8 base layouts, not sampler/LOD/normalization proof."""


def decode_linear_r8_descriptor(words):
    if len(words)!=6 or any(type(w) is not int or not 0<=w<=0xFFFFFFFF for w in words):
        raise ValueError('six linear R8 descriptor words required')
    a,b,c,d,e,f=words
    if (a%(1<<22)!=0x24802 or a>>31 or b%4096!=2 or c>>26 or
        (d,e,f)!=(0x281400,3,0x210)):
        raise ValueError('unsupported R8 descriptor fields')
    base=b//4096*4096;width=c%8192+1;height=c//8192+1;pitch=(a//(1<<22))*32
    if (width,height,pitch) not in ((1280,720,1280),(640,360,768)) or base+pitch*height>0x20000000:
        raise ValueError('unsupported bounded R8 plane layout')
    return {'physical_address':base,'width':width,'height':height,'pitch_bytes':pitch,
        'source_bytes':pitch*height,'tight_bytes':width*height,'format':'R8_UNORM','endianness':'none',
        'descriptor_output_swizzle':['R','R','R','1'],'sampling_executed':False,'gpu_executed':False}


def unpack_linear_r8_plane(words,data):
    proof=decode_linear_r8_descriptor(words)
    if not isinstance(data,bytes) or len(data)!=proof['source_bytes']:
        raise ValueError('exact owned padded R8 plane required')
    width,pitch=proof['width'],proof['pitch_bytes']
    return b''.join(data[at:at+width] for at in range(0,len(data),pitch))
