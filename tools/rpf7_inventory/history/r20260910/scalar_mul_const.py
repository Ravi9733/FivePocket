"""Restricted scalar constant multiply analysis; not full co-issued ALU execution."""
import struct
from vertex_arithmetic_dataflow import arithmetic


def decode_scalar_mul_const(words):
    if len(words)!=3 or any(type(w) is not int or not 0<=w<=0xFFFFFFFF for w in words):
        raise ValueError('three scalar ALU dwords required')
    a,b,c=words;opcode=a>>26
    if opcode not in (42,43) or a&((1<<6)|(1<<7)|(1<<14)|(1<<15)|(3<<24)) or b>>24:
        raise ValueError('only unmodified temporary MUL_CONST_0/1 supported')
    return {'constant_register':c&255,'constant_component':((b//64)+3)%4,
        'temporary_register':(opcode%2)+((c//(1<<29))%2)*2+(b%64//4)*4,
        'temporary_component':b%4,'destination_register':(a>>8)&63,'write_mask':(a>>20)&15,
        'previous_scalar_updated':True,'vector_executed':False,'gpu_executed':False}


def evaluate_scalar_mul_const(words,constant,temporary):
    plan=decode_scalar_mul_const(words)
    if not isinstance(constant,bytes) or len(constant)!=16 or len(temporary)!=4:
        raise ValueError('exact BE constant vector and known temporary vector required')
    value=temporary[plan['temporary_component']]
    if value is None:raise ValueError('unknown scalar source component')
    left=struct.unpack_from('>f',constant,plan['constant_component']*4)[0]
    return arithmetic('mul',left,value)
