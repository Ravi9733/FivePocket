"""Explicit single-sample, unscaled triangle PsParamGen XY contract.

Primary: Xenia/ReX SQ_PROGRAM_CNTL::param_gen (bit18), SQ_CONTEXT_MISC::
param_gen_pos (bits8..15), and SpirvShaderTranslator pixel-parameter prologue.
ZW for non-points are not established hardware barycentrics: leave unknown.
"""
import math
import struct
from raw_shader_analysis import RawShaderError

def geometry_parameter_register(program_control,context_misc):
    if any(type(v) is not int or not 0<=v<=0xFFFFFFFF for v in (program_control,context_misc)):
        raise RawShaderError('exact unsigned SQ register words required')
    if program_control&(1<<19):raise RawShaderError('pixel index generation unsupported')
    if not program_control&(1<<18):return None
    position=(context_misc>>8)&255
    if context_misc&~0xFF0F or position>=16 or position>((program_control>>8)&63):
        raise RawShaderError('unsupported pixel parameter register state')
    return position

def geometry_parameter_xy(x,y,front_facing):
    if type(front_facing) is not bool:raise RawShaderError('explicit primitive facing required')
    result=[]
    for coordinate in (x,y):
        if not math.isfinite(coordinate) or abs(coordinate)>=2**24 or struct.unpack('>f',struct.pack('>f',coordinate))[0]!=coordinate:
            raise RawShaderError('bounded exact float32 pixel coordinate required')
        result.append(float(abs(math.floor(coordinate))))
    bits=struct.unpack('>I',struct.pack('>f',result[0]))[0]
    result[0]=struct.unpack('>f',struct.pack('>I',bits^(0 if front_facing else 0x80000000)))[0]
    return [*result,None,None]

GEOMETRY_PARAMETER_XY_GLSL='''
    // Native logical-target coordinates, single-sample and unscaled only.
    vec2 generatedPixelXY=abs(floor(gl_FragCoord.xy));
    generatedPixelXY.x=uintBitsToFloat(floatBitsToUint(generatedPixelXY.x) ^ (gl_FrontFacing?0u:0x80000000u));
'''
