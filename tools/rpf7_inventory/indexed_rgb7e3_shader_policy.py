"""Bounded CPU reference for ABI30's unit minus tiny exponential.

This is not general denormal ADD support. For a binary32 unit operand and a
subnormal operand, preserving or flushing the latter yields the SAME rounded
unit. The original shader's EXP -> negate -> ADD1 sequence needs precisely
this case. Native GLSL keeps the original add, with no fabricated FTZ mode.
"""
import math,struct
from raw_shader_analysis import RawShaderError
from vertex_arithmetic_dataflow import arithmetic

def indexed_rgb7e3_add(left,right):
    tiny=lambda v:v!=0 and abs(v)<2**-126
    if not tiny(left) and not tiny(right):return arithmetic('add',left,right)
    if not all(math.isfinite(v) for v in (left,right)):
        raise RawShaderError('indexed unit/tiny add requires finite inputs')
    unit,t=(right,left) if tiny(left) else (left,right)
    if unit not in (-1.0,1.0) or not tiny(t):
        raise RawShaderError('only unit plus subnormal is proved for indexed RGB7e3')
    if struct.unpack('>f',struct.pack('>f',t))[0]!=t:
        raise RawShaderError('indexed unit/tiny add requires explicit binary32')
    result=struct.unpack('>f',struct.pack('>f',left+right))[0]
    if result!=unit:raise RawShaderError('unit/tiny rounding invariant failed')
    return result
