"""Bounded Xenia-style base-level POINT / LINEAR-weight sampling reference.

The pinned Xenia SPIR-V fetch translator adds 1.5/1024 texels to offsets and
computes getWeights as fract(uv*size+offset-0.5). Its source explicitly warns
that general LOD/filter behavior is incomplete. Here mipMin=mipMax=0, no mip
storage, normalized2D, LINEAR weights and POINT colors are mandatory. This is
an emulator policy, NOT Xbox filtering-precision or native draw admission.
"""
import math
import struct
from raw_shader_analysis import RawShaderError
from unorm8_vertex_fetch import f32

ROUNDING_OFFSET=1.5/1024

def raw10_base_binding(fetch):
    if not isinstance(fetch,(list,tuple)) or len(fetch)!=6 or any(type(w) is not int or not 0<=w<=0xFFFFFFFF for w in fetch):
        raise RawShaderError('six exact fetch words required')
    a,b,c,d,e,f=fetch
    width=(c&8191)+1;height=((c>>13)&8191)+1
    swizzle=[(d>>(1+3*i))&7 for i in range(4)]
    clamp=[(a>>(10+3*i))&7 for i in range(2)]
    if (a&3!=2 or a&0x3FC or b&63!=54 or b&1024 or not b&0xFFFFF000 or
            c>>26 or d&1 or max(swizzle)>5 or any(v not in (0,2) for v in clamp) or
            (f>>9)&3!=1 or f&0xFFFFF800 or e&~3 or
            d&0xFE000000 or f&0x1E4 or ((d>>19)&3)>1 or ((d>>21)&3)>1 or ((d>>23)&3)>1 or
            width>8192 or height>8192 or ((a>>22)&511)*32<width):
        raise RawShaderError('unsigned raw10 base-only 2D binding required')
    bias=(d>>13)&63;bias=bias if bias<32 else bias-64
    return dict(width=width,height=height,swizzle=swizzle,clamp=clamp,exponent_bias=bias)

def fetch_reference(metadata,coordinates,binding,read_word):
    """read_word returns one normalized-endian packed word, not sampled floats."""
    if not isinstance(coordinates,(list,tuple)) or len(coordinates)!=2 or any(
            not math.isfinite(v) or f32(v)!=v or abs(v)>65536 for v in coordinates):
        raise RawShaderError('bounded explicit float32 XY coordinates required')
    operation=metadata.get('operation');offset=metadata.get('texel_offset',(0,0))
    if len(offset)!=2 or any(not -8<=v<=7.5 or v*2!=int(v*2) for v in offset):
        raise RawShaderError('signed half-texel XY offsets required')
    dimensions=(binding['width'],binding['height'])
    if operation=='texture_weights':
        if not metadata.get('explicit_linear_filter') or any(v in (2,3) for v in metadata['destination_swizzle']):
            raise RawShaderError('only explicit LINEAR XY weights are implemented')
        value=[f32(f32(coordinates[i]*dimensions[i])+f32(offset[i]+ROUNDING_OFFSET-.5)) for i in range(2)]
        return [f32(v-math.floor(v)) for v in value]+[0.,0.]
    if operation!='texture_sample' or not metadata.get('explicit_point_filter'):
        raise RawShaderError('only explicit POINT colours are implemented')
    xy=[]
    for i in range(2):
        # Match the reference shader's normalized offset order, not the
        # algebraically equivalent but differently rounded uv*size+offset.
        shifted=f32(coordinates[i]+f32(f32(offset[i]+ROUNDING_OFFSET)/dimensions[i]))
        at=math.floor(f32(shifted*dimensions[i]))
        xy.append(min(dimensions[i]-1,max(0,at)) if binding['clamp'][i]==2 else at%dimensions[i])
    word=read_word(*xy)
    if type(word) is not int or not 0<=word<=0xFFFFFFFF:raise RawShaderError('packed32 texture word required')
    raw=[f32(((word>>(10*i))&1023)/1023) for i in range(3)]+[f32((word>>30)/3),0.,1.]
    return [f32(math.ldexp(raw[s],binding['exponent_bias'])) for s in binding['swizzle']]

def _translate_filter_weights(data,constants,fetch,*,fragment=False):
    """Private diagnostic kernel from original dataflow, not a guest pipeline.

    Exact constants are embedded in the returned source, which therefore belongs
    under ignored private analysis. A future runtime must use current bound
    constants and resources and validate stage/target ownership independently.
    """
    from pixel_sample_dataflow import analyze_pixel_sample_dataflow
    proof=analyze_pixel_sample_dataflow(data,ui_operations=True,extended_math=True,
                                      loading_math=True,filter_weights_profile=True)
    binding=raw10_base_binding(fetch);nodes=proof['nodes'];lines=[];done={};sampled=set()
    if (set(proof['exports'])!={'0'} or any(n is None for n in proof['exports']['0']) or proof['kills'] or
            not proof['fetches'] or len({f['slot'] for f in proof['fetches']})!=1):
        raise RawShaderError('one texture and complete colour0 kernel required')
    def sample(i):
        if i in sampled:return
        f=proof['fetches'][i];coords=[expression(n) for n in f['coordinates']]
        offset=f.get('texel_offset',(0,0));uv=f'vec2({coords[0]},{coords[1]})'
        if f['operation']=='texture_weights':
            if any(s in (2,3) for s in f['destination_swizzle']):raise RawShaderError('XYZ/mip weights are not implemented')
            lines.append(f'precise vec2 weightPixel{i}={uv}*vec2({binding["width"]}.0,{binding["height"]}.0);')
            lines.append(f'precise vec2 weightCentered{i}=weightPixel{i}+vec2({offset[0]+ROUNDING_OFFSET-.5},{offset[1]+ROUNDING_OFFSET-.5});')
            lines.append(f'vec4 sample{i}=vec4(fract(weightCentered{i}),0.0,0.0);')
        else:
            lines.append(f'precise vec2 shifted{i}={uv}+vec2({offset[0]+ROUNDING_OFFSET},{offset[1]+ROUNDING_OFFSET})/vec2({binding["width"]}.0,{binding["height"]}.0);')
            lines.append(f'precise vec4 sample{i}=textureLod(sourceTexture,shifted{i},0.0)*{math.ldexp(1.,binding["exponent_bias"])};')
        sampled.add(i)
    def expression(i):
        if i in done:return done[i]
        n=nodes[i];op=n[0]
        if op=='initial':
            if n[1]!=0 or n[2] not in (0,1):raise RawShaderError('kernel requires only original interpolator0 XY')
            value=f'interpolatorXY[{n[2]}]' if fragment else f'queries[index][{n[2]}]'
        elif op=='constant':
            if fragment:
                value=f'n{i}';lines.append(f'precise float {value}=uintBitsToFloat(readBE({1028+n[1]*4+n[2]}u));')
                lines.append(f'if(isnan({value}) || isinf({value})) return;')
            else:
                b=constants.get(n[1])
                if not isinstance(b,bytes) or len(b)!=16:raise RawShaderError('exact BE constant vector required')
                bits=struct.unpack_from('>I',b,n[2]*4)[0]
                if not math.isfinite(struct.unpack('>f',struct.pack('>I',bits))[0]):raise RawShaderError('nonfinite embedded constant')
                value=f'uintBitsToFloat(0x{bits:08X}u)'
        elif op=='literal':value=f'{float(n[1])}'
        elif op=='sample':sample(n[1]);value=f'sample{n[1]}[{n[2]}]'
        else:
            args=[expression(j) for j in n[1:]]
            if op=='mul':rhs=f'(({args[0]}==0.0 || {args[1]}==0.0)?0.0:({args[0]}*{args[1]}))'
            elif op=='add':rhs=f'({args[0]}+{args[1]})'
            elif op=='maximum':rhs=f'({args[0]}>={args[1]}?{args[0]}:{args[1]})'
            elif op=='negate':rhs=f'(-{args[0]})'
            elif op=='select_eq0':rhs=f'({args[0]}==0.0?{args[1]}:{args[2]})'
            elif op in ('floor','fraction','exp2'):rhs=f'{"fract" if op=="fraction" else op}({args[0]})'
            else:raise RawShaderError('unimplemented filtered-colour expression '+op)
            value=f'n{i}';lines.append(f'precise float {value}={rhs};')
            lines.append(f'if(isnan({value}) || isinf({value})) return;')
        done[i]=value;return value
    for i in range(len(proof['fetches'])):sample(i)
    for i,n in enumerate(nodes):
        if n[0] not in ('initial','constant','literal','sample'):expression(i)
    color=[expression(i) for i in proof['exports']['0']]
    text='''#version 450
layout(local_size_x=64) in;
layout(set=0,binding=0,std430) readonly buffer Queries {vec4 queries[];};
layout(set=0,binding=1,std430) buffer Results {vec4 results[];};
layout(set=0,binding=2) uniform sampler2D sourceTexture;
layout(push_constant) uniform Count {uint count;};
void main(){uint index=gl_GlobalInvocationID.x;if(index>=count)return;
'''+ '\n'.join(lines)+'\nresults[index+1]=vec4('+','.join(color)+');\n}\n'
    if fragment:
        from inline_shader_translation import COMMON
        text=COMMON+'''layout(location=0) in vec2 interpolatorXY;
layout(location=0) out vec4 colour;
layout(set=0,binding=1) uniform sampler2D sourceTexture;
void main(){
// A refused fragment poisons the float readback; never silently stores zero.
colour=vec4(uintBitsToFloat(0x7FC00000u));
'''+ '\n'.join(lines)+'\ncolour=vec4('+','.join(color)+');\n}\n'
    return text,dict(proof=proof,binding=binding,weight_policy='xenia-base-linear-epsilon-1.5/1024',
        runtime_constants=fragment,native_game_pipeline=False,hardware_filter_conformance=False)


def translate_filter_weights_compute(data,constants,fetch):
    """Captured-constant diagnostic only; generated source belongs in private analysis."""
    return _translate_filter_weights(data,constants,fetch)


def translate_filter_weights_fragment(data,fetch):
    """Original PS arithmetic with CURRENT constants. Native admission is separate.

    Fetch layout/swizzle/bias specialization must match the current submitted
    descriptor; the native owner supplies the actual swizzled packed image.
    Only base-level filtering is represented, not a general LOD implementation.
    """
    return _translate_filter_weights(data,None,fetch,fragment=True)
