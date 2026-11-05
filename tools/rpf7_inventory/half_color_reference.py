"""Narrow standalone full-size binary16 storage expectation, not retail ROP.

Vulkan attachment narrowing permits either adjacent value. Each rounding policy
here is an explicit expectation, NOT a promise by Vulkan or Xbox hardware.
Only uniform binary textures, already validated full-quad stage inputs, and a
uniform existing float32 reference can be expanded to a different pixel grid.
"""
import math
import struct

WIDTH,HEIGHT=1280,720
OUTPUT_BYTES=WIDTH*HEIGHT*8+32
BLEND_PRECISIONS=('float32','binary16-products-toward-zero','binary16-inputs-rtz-products-rne')


def narrow_half(channels,rounding):
    if rounding not in ('nearest-even','toward-zero'):raise ValueError('explicit supported storage rounding required')
    if any(not math.isfinite(v) or (v==0 and math.copysign(1,v)<0) or
           (v!=0 and not 2**-14<=abs(v)<=65504) for v in channels):
        raise ValueError('finite normal binary16-range or positive-zero output required')
    half=struct.pack('<4e',*channels)
    if rounding=='toward-zero':
        narrowed=struct.unpack('<4e',half);words=struct.unpack('<4H',half)
        half=struct.pack('<4H',*(word-int(abs(stored)>abs(original))
            for word,stored,original in zip(words,narrowed,channels)))
    return half


def full_half_reference(profile,source,reference,rounding='nearest-even'):
    if rounding not in ('nearest-even','toward-zero'):raise ValueError('explicit supported storage rounding required')
    if not isinstance(source,bytes) or not isinstance(reference,bytes):
        raise ValueError('immutable input/reference bytes required')
    if profile=='r8_planes_vertex_pair_test':
        if len(source)!=1382720:raise ValueError('exact bounded R8 pair layout required')
        planes=(source[:921600],source[921600:1152000],source[1152000:1382400])
        if any(p[0] not in (0,255) or p!=p[:1]*len(p) for p in planes):
            raise ValueError('uniform binary R8 planes required at new pixel grid')
    elif profile=='texture32_vertex_pair_test':
        if len(source)!=4352:raise ValueError('exact bounded small pair layout required')
        texel=source[:4]
        if any(v not in (0,255) for v in texel) or source[:4096]!=texel*1024:
            raise ValueError('uniform binary RGBA texture required at new pixel grid')
    else:raise ValueError('unsupported bounded stage pair')
    guard=bytes([0x5A])*16
    if len(reference)!=16416 or reference[:16]!=guard or reference[-16:]!=guard:
        raise ValueError('exact guarded float32 reference required')
    pixel=reference[16:32]
    if reference[16:-16]!=pixel*1024:raise ValueError('uniform full-coverage or discarded reference required')
    if pixel==guard:
        return bytes([0x5A])*OUTPUT_BYTES,dict(discarded=True,rounding='not-applicable-clear-exact')
    channels=struct.unpack('<4f',pixel)
    half=narrow_half(channels,rounding)
    return guard+half*(WIDTH*HEIGHT)+guard,dict(discarded=False,
        rounding='explicit-'+rounding+'-test-expectation-not-vulkan-mandate',
        float32_rgba=list(channels),half_rgba=list(struct.unpack('<4e',half)),
        half_words=list(struct.unpack('<4H',half)))


def persistent_half_reference(profile,source,reference,rounding='nearest-even'):
    """Explicit RGBA src*srcA + dst*(1-srcA), half storage after each draw.

    0/1/2/3/0 draws across five distinct render passes, first CLEAR then LOAD.
    Float32 separate arithmetic is a test policy, not Vulkan blending precision.
    The tiny file contains one uniform pixel per pass, expanded by native test.
    """
    _,storage=full_half_reference(profile,source,reference,rounding)
    current=struct.pack('<4e',*(203.25,)*4);pixels=[]
    color=storage.get('float32_rgba');alpha=color[3] if color else 0.
    if not 0<=alpha<=1:raise ValueError('bounded blend source alpha in zero to one required')
    def f32(v):return struct.unpack('<f',struct.pack('<f',v))[0]
    for draws in (0,1,2,3,0):
        for _ in range(draws):
            if color:
                prior=struct.unpack('<4e',current)
                current=narrow_half([f32(f32(c*alpha)+f32(d*f32(1-alpha))) for c,d in zip(color,prior)],rounding)
        pixels.append(current)
    guard=bytes([0x5A])*16
    return guard+b''.join(pixels)+guard,dict(storage,blend='rgba-src-times-src-alpha-plus-dst-times-one-minus-src-alpha',
        blend_precision='explicit-separate-float32-test-policy-not-vulkan-mandate',
        store_precision='binary16-after-every-draw',draws_per_pass=[0,1,2,3,0],
        load_operations=['clear','load','load','load','load'],
        rgba_per_pass=[list(struct.unpack('<4e',p)) for p in pixels])


def blend_half_pixel(color,prior,rounding,precision='float32'):
    """Explicit candidate arithmetic; no policy is a retail/device promise.

    The binary16-products candidate narrows BOTH weighted terms before adding.
    The inputs-rtz/products-rne candidate additionally narrows source channels
    (including alpha) toward zero and the inverse-alpha factor nearest-even,
    then rounds both products nearest-even. Final storage rounding is separate.
    Its bounded normal-or-zero domain deliberately rejects intermediate
    underflow/overflow instead of inventing untested flush/rounding behavior.
    """
    if precision not in BLEND_PRECISIONS:raise ValueError('explicit supported blend precision required')
    if len(color)!=4 or len(prior)!=4 or not 0<=color[3]<=1:
        raise ValueError('four channels and bounded source alpha required')
    def f32(v):return struct.unpack('<f',struct.pack('<f',v))[0]
    def terms(values,mode):
        # Multiplication by a zero factor can create signed zero even
        # though final accepted attachment pixels exclude negative zero.
        narrowed=struct.unpack('<4e',narrow_half([v if v else 0. for v in values],mode))
        return [v if v==0 else n for v,n in zip(values,narrowed)]
    if precision=='binary16-inputs-rtz-products-rne':color=terms(color,'toward-zero')
    alpha=color[3];inverse=f32(1-alpha)
    if precision=='binary16-inputs-rtz-products-rne':inverse=terms([inverse]*4,'nearest-even')[0]
    source=[f32(c*alpha) for c in color];destination=[f32(d*inverse) for d in prior]
    if precision=='binary16-products-toward-zero':
        source=terms(source,'toward-zero');destination=terms(destination,'toward-zero')
    elif precision=='binary16-inputs-rtz-products-rne':
        source=terms(source,'nearest-even');destination=terms(destination,'nearest-even')
    return narrow_half([f32(s+d) for s,d in zip(source,destination)],rounding)


def mixed_half_reference(r8_source,r8_reference,small_source,small_reference,rounding='nearest-even',initial_pixel=None,blend_precision='float32'):
    """CLEAR -> actual R8 pair -> actual small pair -> empty LOAD, uniform only."""
    if blend_precision not in BLEND_PRECISIONS:raise ValueError('explicit supported blend precision required')
    colors=[]
    for profile,source,ref in (('r8_planes_vertex_pair_test',r8_source,r8_reference),
        ('texture32_vertex_pair_test',small_source,small_reference)):
        _,storage=full_half_reference(profile,source,ref,rounding)
        color=storage.get('float32_rgba')
        if color and not 0<=color[3]<=1:raise ValueError('bounded blend source alpha required')
        colors.append(color)
    if initial_pixel is not None:
        if not isinstance(initial_pixel,bytes) or len(initial_pixel)!=8:raise ValueError('one explicit half pixel required')
        current=narrow_half(struct.unpack('<4e',initial_pixel),rounding)
        if current!=initial_pixel:raise ValueError('initial half pixel must be represented exactly')
    else:current=struct.pack('<4e',*(203.25,)*4)
    pixels=[current]
    for color in colors:
        if color:
            current=blend_half_pixel(color,struct.unpack('<4e',current),rounding,blend_precision)
        pixels.append(current)
    pixels.append(current);guard=bytes([0x5A])*16
    return guard+b''.join(pixels)+guard,dict(rgba_per_pass=[list(struct.unpack('<4e',p)) for p in pixels],
        pass_order=['initialized-load-no-draw' if initial_pixel is not None else 'clear-no-draw','r8-pair','small-pair','load-no-draw'],
        blend='rgba-src-times-src-alpha-plus-dst-times-one-minus-src-alpha',
        blend_precision='explicit-'+blend_precision+'-test-policy-not-hardware-mandate',half_rounding=rounding,
        source_float32_rgba=colors,retail_state_replayed=False,game_frame=False,guest_writeback=False)


def uniform_initial_edram(source):
    """Narrow uniform fixture: defined5A exterior, complete uniform active region."""
    from half_color_load import unpack_half,INPUT_BYTES
    if not isinstance(source,bytes) or len(source)!=INPUT_BYTES:raise ValueError('exact guarded initial EDRAM required')
    begin,end=16+3686400,16+7372800
    if source[:begin]!=bytes([0x5A])*begin or source[end:]!=bytes([0x5A])*(len(source)-end):
        raise ValueError('explicit5A initial exterior required')
    pixel=source[begin:begin+4]
    if source[begin:end]!=pixel*921600:raise ValueError('uniform initial EDRAM footprint required by compact reference')
    packed=struct.unpack('<I',pixel)[0]
    return unpack_half(packed),dict(initial_packed_word=packed,initial_half_words=list(struct.unpack('<4H',unpack_half(packed))),
        cpu_supplied_initial_edram=True,live_depth_connected=False)


def sequence_store_copy_reference(reference):
    """Add independently packed EDRAM and tiled BGRA pixel to each uniform pass."""
    from half_color_store import packed_half
    from color_copy import convert_word
    guard=bytes([0x5A])*16
    if not isinstance(reference,bytes) or len(reference)!=64 or reference[:16]!=guard or reference[-16:]!=guard:
        raise ValueError('exact guarded four-pass half reference required')
    records=[];packed_words=[];copy_words=[]
    for index in range(4):
        pixel=reference[16+index*8:24+index*8];packed=packed_half(struct.unpack('<4H',pixel))
        if packed is None:raise ValueError('complete finite normal-or-zero half store required')
        converted=convert_word(packed,True);records.append(pixel+struct.pack('<2I',packed,converted))
        packed_words.append(packed);copy_words.append(converted)
    return guard+b''.join(records)+guard,dict(packed_words=packed_words,copy_words=copy_words,
        checked_native_output_bytes=21626976,copy_capacity_bytes=3768320,copy_padding_bytes_unwritten=81920,
        depth_alias_initialized=False,guest_writeback=False)
