"""Opt-in CUBEv arithmetic, not a sampler or an Xbox numeric oracle.

Reference: pinned ReXGlue/Xenia ucode.h and dxbc_translator_alu.cpp. Require
the documented two equivalent zzxy/yxzz operands, select major axis in Z/Y/X
tie order, output TC/SC/twice-signed-major/face. Finite float32 normal-or-zero
policy matches the other offline loading arithmetic evaluators.
"""
import math
import struct
from raw_shader_analysis import RawShaderError

CUBE_OPERATIONS=('cube_tc','cube_sc','cube_ma','cube_face')

def cube_result_nodes(operands,node):
    if len(operands)!=2 or any(len(v)!=4 for v in operands):
        raise RawShaderError('two complete CUBE operands required')
    a,b=operands
    if a[0]!=a[1] or b!=[a[3],a[2],a[0],a[0]]:
        raise RawShaderError('CUBE requires equivalent documented zzxy/yxzz operands')
    return [node(op,a[2],a[3],a[0]) for op in CUBE_OPERATIONS]

def evaluate_cube_operation(operation,operands):
    if operation not in CUBE_OPERATIONS or len(operands)!=3:
        raise RawShaderError('explicit CUBE component and three coordinates required')
    for value in operands:
        if not math.isfinite(value) or (value!=0 and abs(value)<2**-126):
            raise RawShaderError('CUBE requires finite normal-or-zero inputs')
        try:rounded=struct.unpack('>f',struct.pack('>f',value))[0]
        except (OverflowError,struct.error) as error:raise RawShaderError('CUBE input outside float32') from error
        if rounded!=value:raise RawShaderError('CUBE requires explicit float32 inputs')
    x,y,z=operands
    if abs(z)>=abs(x) and abs(z)>=abs(y):result=(-y,-x if z<0 else x,2*z,5.0 if z<0 else 4.0)
    elif abs(y)>=abs(x):result=(-z if y<0 else z,x,2*y,3.0 if y<0 else 2.0)
    else:result=(-y,z if x<0 else -z,2*x,1.0 if x<0 else 0.0)
    try:out=struct.unpack('>f',struct.pack('>f',result[CUBE_OPERATIONS.index(operation)]))[0]
    except (OverflowError,struct.error) as error:raise RawShaderError('CUBE result outside float32') from error
    if not math.isfinite(out) or (out!=0 and abs(out)<2**-126):raise RawShaderError('CUBE result outside finite normal-or-zero policy')
    return out

CUBE_GLSL='''
vec4 xenosCube(vec3 direction) {
    precise float x=direction.x,y=direction.y,z=direction.z;
    if(abs(z)>=abs(x) && abs(z)>=abs(y))
        return vec4(-y,z<0.0?-x:x,2.0*z,z<0.0?5.0:4.0);
    if(abs(y)>=abs(x))
        return vec4(y<0.0?-z:z,x,2.0*y,y<0.0?3.0:2.0);
    return vec4(-y,x<0.0?z:-z,2.0*x,x<0.0?1.0:0.0);
}
'''
