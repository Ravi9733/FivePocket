"""Bounded ABI29 EXP_IEEE underflow -> legacy SM3 multiply reference.

Pinned Xenia 779680ad gpu/ucode.h:971 describes zero OR denormal multiplicands
producing positive zero. The observed original PS has EXP_IEEE(-143.08145)
feeding MUL directly. Preserve the possible finite subnormal exponential;
the multiply's explicit input rule makes a driver's EXP flush immaterial.
This is not a general subnormal arithmetic or Xbox transcendental oracle.
"""
import math
import struct
from loading_shader_math import evaluate_loading_operation
from raw_shader_analysis import RawShaderError


def world_strip_exp2(value):
    if not math.isfinite(value) or struct.unpack('>f',struct.pack('>f',value))[0]!=value:
        raise RawShaderError('world strip exponent requires finite binary32 input')
    if -150.0 < value < -126.0:
        return struct.unpack('>f',struct.pack('>f',2.0**value))[0]
    return evaluate_loading_operation('exp2',[value])
