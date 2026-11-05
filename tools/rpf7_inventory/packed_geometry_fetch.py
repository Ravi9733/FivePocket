"""Owned-buffer reference for the command shaders' packed normal/UV formats.

Independent implementation of Xenia's ucode.h field layout and
spirv_shader_translator_fetch.cc component extraction. No PM4 admission.
SNORM uses explicit float32 reciprocal-multiply/clamp, not a retail rounding
oracle. Half exponent31 rejects: Xenos extended range is not IEEE Inf/NaN.
"""
import math
import struct
from raw_shader_analysis import RawShaderError, _vertex_fetch_layout
from unorm8_vertex_fetch import f32


def decode_packed_geometry_fetch(words, previous=None, registers=None, address=0):
    if len(words)!=3 or any(type(w) is not int or not 0<=w<=0xFFFFFFFF for w in words):
        raise RawShaderError('invalid packed geometry instruction words')
    fmt=(words[1]>>16)&63
    modifiers={7:0x1000,31:0x3000,32:0x3000}
    if fmt not in modifiers or words[1]&0x3FC0F000!=modifiers[fmt]:
        raise RawShaderError('unsupported packed geometry format/modifiers')
    if registers is None:
        registers=[[f'initial_r{r}.{axis}' for axis in 'xyzw'] for r in range(64)]
    return _vertex_fetch_layout(words,previous,registers,address,fmt,2 if fmt==31 else 4)


def unpack_geometry_word(word, fmt):
    if type(word) is not int or not 0<=word<=0xFFFFFFFF:
        raise RawShaderError('invalid packed geometry source word')
    if fmt==7:
        values=[]
        for shift,width in ((0,10),(10,10),(20,10),(30,2)):
            raw=(word>>shift)&((1<<width)-1)
            signed=(raw^(1<<(width-1)))-(1<<(width-1))
            values.append(max(-1.0,f32(signed*f32(1/((1<<(width-1))-1)))))
        return values
    if fmt==31:
        halves=[word&65535,word>>16]
        if any(h&0x7C00==0x7C00 for h in halves):
            raise RawShaderError('Xenos half extended exponent is not yet implemented')
        return [struct.unpack('>e',h.to_bytes(2,'big'))[0] for h in halves]
    raise RawShaderError('unsupported packed geometry source format')


def read_packed_geometry_fetch(plan, source, index, prior=(None,None,None,None)):
    if (not isinstance(source,bytes) or not 0<len(source)<=256*1024 or len(source)%4 or
            type(index) is not int or not 0<=index<1<<24 or len(prior)!=4 or
            plan['format'] not in (7,31,32) or plan['components']!={7:4,31:2,32:4}[plan['format']]):
        raise RawShaderError('invalid bounded packed geometry source/index/format')
    for key,low,high in (('stride_words',1,256),('offset_words',0,0x400000),
                         ('fetch_slot',0,96),('destination',0,64)):
        if type(plan[key]) is not int or not low<=plan[key]<high:
            raise RawShaderError('invalid packed geometry layout')
    swizzle=plan['destination_swizzle']
    if len(swizzle)!=4 or any(type(s) is not int or s not in (0,1,2,3,4,5,7) or
            (s<4 and s>=plan['components']) for s in swizzle):
        raise RawShaderError('invalid packed geometry swizzle')
    start=(index*plan['stride_words']+plan['offset_words'])*4
    count=2 if plan['format']==32 else 1
    if start+count*4>len(source):raise RawShaderError('packed geometry fetch exceeds captured buffer')
    values=[]
    for offset in range(count):
        values.extend(unpack_geometry_word(int.from_bytes(source[start+offset*4:start+offset*4+4],'big'),
            31 if plan['format']==32 else plan['format']))
    result=list(prior)
    for lane,select in enumerate(swizzle):
        if select==7:
            if result[lane] is not None and not math.isfinite(result[lane]):
                raise RawShaderError('non-finite kept packed geometry lane')
        else:result[lane]=values[select] if select<4 else float(select-4)
    return result
