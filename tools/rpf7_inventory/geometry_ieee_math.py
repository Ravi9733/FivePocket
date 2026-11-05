"""Explicit geometry-reference infinity domain, not an Xbox precision oracle.

Xenia gpu/ucode.h identifies scalar14 as EXP_IEEE, not a finite/clamped
exponential. Its SPIR-V translator emits Exp2, with SM3 zero-multiply handling.
Finite captured inputs may legitimately produce infinity before a later select
or saturation. NaN and nonzero subnormal behavior remain deliberately unproved.
https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/ucode.h
https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/spirv_shader_translator_alu.cc
"""
import math
import struct
from loading_shader_math import evaluate_loading_operation
from raw_shader_analysis import RawShaderError


def checked(value):
    if math.isnan(value) or (value != 0 and abs(value) < 2**-126):
        raise RawShaderError('geometry IEEE NaN/subnormal behavior is not implemented')
    return value


def rounded(value):
    checked(value)
    try: result = struct.unpack('>f', struct.pack('>f', value))[0]
    except OverflowError: result = math.copysign(math.inf, value)
    return checked(result)


def evaluate_geometry_ieee(operation, operands):
    for value in operands:
        checked(value)
        if rounded(value) != value:
            raise RawShaderError('geometry IEEE requires explicit float32 inputs')
    if operation in ('add', 'mul'):
        if len(operands) != 2: raise RawShaderError('invalid geometry arithmetic arity')
        a, b = operands
        # SM3 zero multiplication is explicit in the existing GLSL emitter.
        return rounded(a+b if operation=='add' else 0.0 if a==0 or b==0 else a*b)
    if operation == 'exp2':
        if len(operands) != 1: raise RawShaderError('invalid geometry exponent arity')
        x = operands[0]
        if x >= 128.0: return math.inf
        if x == -math.inf: return 0.0
        # Keep the existing normal/zero underflow policy; no invented FTZ mode.
        return evaluate_loading_operation(operation, operands)
    # These operations have defined comparison/selection behavior for infinity;
    # do not reject an unselected infinity as an invalid final shader output.
    if operation == 'negate' and len(operands)==1:return -operands[0]
    if operation == 'absolute' and len(operands)==1:return abs(operands[0])
    if operation == 'maximum' and len(operands)==2:
        return operands[0] if operands[0]>=operands[1] else operands[1]
    if operation == 'minimum' and len(operands)==2:
        return operands[0] if operands[0]<operands[1] else operands[1]
    if operation == 'greater' and len(operands)==2:return float(operands[0]>operands[1])
    if operation == 'greater_equal' and len(operands)==2:return float(operands[0]>=operands[1])
    if operation == 'select_gt0' and len(operands)==3:return operands[1] if operands[0]>0 else operands[2]
    return evaluate_loading_operation(operation, operands)


GEOMETRY_EXP_GLSL = '''
// Binary32 inputs >=128 overflow EXP_IEEE. Make that defined explicitly,
// preserving infinity for downstream arithmetic/select/saturate operations.
float geometryExp2(float x) {
    return x>=128.0?uintBitsToFloat(0x7f800000u):exp2(x);
}
'''
