"""Finite checked reference conversion, not retail ROP/blend/format12 policy."""
import bisect
import math
import struct


def word(v):
    if type(v) is not int or not 0 <= v <= 0xFFFFFFFF:raise ValueError('unsigned word required')
    return v


def bits(v):return struct.unpack('<I',struct.pack('<f',v))[0]
def value(v):return struct.unpack('<f',struct.pack('<I',word(v)))[0]
def f32(v):return value(bits(v))


def decode(code):
    word(code)
    if code>1023:raise ValueError('ten-bit component required')
    exponent,mantissa=divmod(code,128)
    return math.ldexp(mantissa if exponent==0 else 128+mantissa,-9 if exponent==0 else exponent-10)


LEVELS=tuple(decode(i) for i in range(1024))


def source_candidate(raw):
    """Pinned Xenia SPIR-V/DirectXTex RGB candidate, finite values only."""
    x=value(raw)
    if not math.isfinite(x):raise ValueError('nonfinite component unsupported')
    if x<=0:return 0
    if x>=31.875:return 1023
    biased=((0x800000|(raw&0x7FFFFF))>>min(125-(raw>>23),24)) if x<.25 else raw-(124<<23)
    return ((biased+0x7FFF+((biased>>16)&1))>>16)&1023


def encode(raw):
    x=value(raw)
    if not math.isfinite(x):raise ValueError('nonfinite component unsupported')
    if x<=0:return 0
    if x>=31.875:return 1023
    upper=bisect.bisect_left(LEVELS,x)
    lower=upper-1
    distances=(x-LEVELS[lower],LEVELS[upper]-x)
    result=lower if distances[0]<distances[1] or (distances[0]==distances[1] and lower%2==0) else upper
    if result!=source_candidate(raw):raise ValueError('pre-round sticky-bit disagreement with source candidate')
    return result


def pack(rgba):
    if len(rgba)!=4:raise ValueError('four float32 words required')
    r,g,b=[encode(c) for c in rgba[:3]]
    a=value(rgba[3])
    if not math.isfinite(a):raise ValueError('nonfinite alpha unsupported')
    alpha=int(f32(f32(min(1,max(0,a))*3)+.5))
    return r|(g<<10)|(b<<20)|(alpha<<30)


def unpack(packed):
    word(packed)
    return [bits(decode((packed>>(i*10))&1023)) for i in range(3)]+[bits((packed>>30)*f32(1/3))]
