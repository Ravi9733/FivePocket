"""Bounded ABI32 IEEE reference exceptions, not a native draw admission.

Xenia ucode.h: LOG_IEEE16(0)=-INF, EXP_IEEE14(-INF)=0, and SM3 MUL
produces positive zero if either multiplicand is zero. The captured shader
uses EXP(LOG(x)*power), including x=0. Preserve these intermediates; replacing
LOG(0) with an arbitrary finite value changes that connected computation.
NaNs, subnormal multiplicands and unrelated operations remain unsupported.
"""
import math
import struct
from raw_shader_analysis import RawShaderError


def geometry_lod_infinite_product(left,right):
    if any(math.isnan(v) for v in (left,right)) or not (math.isinf(left)^math.isinf(right)):
        raise RawShaderError('one infinity and one finite normal-or-zero multiplicand required')
    finite=right if math.isinf(left) else left
    if finite!=0 and abs(finite)<2**-126:
        raise RawShaderError('subnormal multiplicand is outside geometry LOD policy')
    if struct.unpack('>f',struct.pack('>f',finite))[0]!=finite:
        raise RawShaderError('geometry LOD multiplicand must be binary32')
    if finite==0:return 0.0
    return math.copysign(math.inf,math.copysign(1.,left)*math.copysign(1.,right))


def geometry_lod_zero_log(value):
    if value!=0 or not math.isfinite(value):
        raise RawShaderError('only signed-zero LOG_IEEE is handled here')
    return -math.inf


def geometry_lod_infinite_exp(value):
    if not math.isinf(value):
        raise RawShaderError('only infinite EXP_IEEE is handled here')
    return 0.0 if value<0 else math.inf
