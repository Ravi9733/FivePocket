"""Experimental straight-line vertex dataflow, NOT an executable PM4 plan.

Float/unorm8 fetches, identical MAX copies, finite-normal-or-zero MUL and
unfused MAD only. Allocation instructions are retained as metadata: encoded
size is NOT treated as a verified export bound. Never used by runtime guards.
"""
import hashlib
import math
import struct
from raw_shader_analysis import RawShaderError, _float_vertex_fetch, _sint16_vertex_fetch
from unorm8_vertex_fetch import decode_unorm8_fetch, f32
from scalar_constant import decode_scalar_constant, scalar_constant_node


def schedule(data):
    if not isinstance(data, bytes) or not data or len(data)%12 or len(data)>4096:
        raise RawShaderError('invalid bounded vertex instruction extent')
    words=struct.unpack('>'+str(len(data)//4)+'I',data)
    extent=len(words)//3
    controls=[];body=extent;ended=False
    for pair in range(extent):
        if pair>=body:break
        a,b,c=words[pair*3:pair*3+3]
        for cf in (a|((b&65535)<<32),(b>>16)|(c<<16)):
            if len(controls)==128:raise RawShaderError('vertex control budget exceeded')
            op=cf>>44
            if op==0:
                if cf:raise RawShaderError('noncanonical vertex NOP')
                controls.append(('nop',))
            elif op==12:
                if cf&~((15<<44)|(7<<40)|7):raise RawShaderError('unsupported vertex ALLOC fields')
                kind=(cf>>41)&3
                if kind not in (1,2):raise RawShaderError('memory/unknown allocation unsupported')
                controls.append(('alloc',kind,cf&7,bool(cf&(1<<40))))
            elif op in (1,2):
                if cf&~((15<<44)|(1<<43)|(1<<41)|((1<<34)-1)):
                    raise RawShaderError('unsupported vertex EXEC fields')
                address,count,sequence=cf&4095,(cf>>12)&7,(cf>>16)&4095
                if not 1<=count<=6 or address+count>extent or sequence>>(count*2):
                    raise RawShaderError('invalid vertex EXEC range/sequence')
                body=min(body,address);controls.append(('exec',address,count,sequence))
                if op==2:ended=True;break
            else:raise RawShaderError('conditional/loop/unknown vertex control unsupported')
        if ended:break
    if not ended or (len(controls)+1)//2>body:
        raise RawShaderError('unterminated or overlapping vertex control storage')
    return words,controls


def analyze_vertex_dataflow(data, *, dot4=False, scalar_constants=False):
    words,controls=schedule(data)
    nodes=[];intern={}
    def node(op,*args):
        key=(op,*args)
        if key not in intern:
            if len(nodes)==4096:raise RawShaderError('vertex expression budget exceeded')
            intern[key]=len(nodes);nodes.append(key)
        return intern[key]
    registers=[[node('initial',r,c) for c in range(4)] for r in range(64)]
    outputs={};fetches=[];allocations=[];allocated=set();executed=[];previous=None;previous_scalar=None
    for control in controls:
        if control[0]=='alloc':
            if control[1] in allocated:raise RawShaderError('repeated allocation unsupported')
            allocated.add(control[1])
            allocations.append({'kind':control[1],'encoded_size':control[2],'unserialized':control[3]})
        if control[0]!='exec':continue
        for address in range(control[1],control[1]+control[2]):
            if len(executed)==256:raise RawShaderError('executed vertex instruction budget exceeded')
            executed.append(address)
            a,b,c=words[address*3:address*3+3]
            if control[3]&(1<<((address-control[1])*2)):
                decoder={6:decode_unorm8_fetch,25:_sint16_vertex_fetch}.get((b>>16)&63,_float_vertex_fetch)
                fetch,previous=decoder((a,b,c),previous,registers,address)
                if len(fetches)==32:raise RawShaderError('vertex fetch budget exceeded')
                fetch_id=len(fetches);fetches.append(fetch)
                target=registers[fetch['destination']]
                for lane,select in enumerate(fetch['destination_swizzle']):
                    if select<4:target[lane]=node('fetch',fetch_id,select)
                    elif select<6:target[lane]=node('literal',select-4)
                continue
            opcode=(c>>24)&31
            scalar_opcode=a>>26
            if (opcode not in ((1,2,11,15) if dot4 else (1,2,11)) or
                    scalar_opcode not in ((42,43,44,45,46,47,50) if scalar_constants else (50,))):
                raise RawShaderError('only MUL/MAX-copy/MAD with retained scalar supported')
            if a&((1<<6)|(1<<7)|(3<<24)) or b>>24:
                raise RawShaderError('relative/absolute/clamp/negate/predicate vertex ALU unsupported')
            export=bool(a&(1<<15));destination=a&63
            vector,scalar=(a>>16)&15,(a>>20)&15
            if export:
                vector_result,scalar_result=vector&~scalar,scalar&~vector
                zero=15&~(vector|scalar) if a&(1<<14) else 0
                one=vector&scalar;kind=1 if destination==62 else 2
                if kind not in allocated or (destination!=62 and destination>=16):
                    raise RawShaderError('unsupported/memory/unallocated vertex export')
                target=outputs.setdefault(destination,[None]*4)
            else:
                if a&(1<<14):raise RawShaderError('relative scalar destination unsupported')
                vector_result,scalar_result,zero,one=vector,scalar,0,0
                target=registers[destination]
            scalar_value=previous_scalar
            if scalar_opcode!=50:
                scalar_value=scalar_constant_node(decode_scalar_constant((a,b,c)),registers,node)
            if scalar_result and scalar_value is None:
                raise RawShaderError('previous scalar result dependency unsupported')
            scalar_destination=(a>>8)&63
            if not export and scalar_destination==destination and vector_result&scalar_result:
                raise RawShaderError('overlapping scalar/vector temporary writes unsupported')
            # Read all operands from pre-instruction registers before any writes.
            operands=[]
            if vector_result:
                for operand in range(3 if opcode==11 else 2):
                    shift=16-operand*8;source=(c>>shift)&255;swizzle=(b>>shift)&255
                    temporary=bool(c&(1<<(31-operand)))
                    if temporary and source>=64:raise RawShaderError('relative/absolute temporary source unsupported')
                    values=[]
                    for lane in range(4):
                        component=((swizzle>>(lane*2))+lane)&3
                        values.append(registers[source][component] if temporary else node('constant',source,component))
                    operands.append(values)
            dot=None
            if vector_result and opcode==15:
                for lane in range(4):
                    product=node('mul',operands[0][lane],operands[1][lane])
                    dot=product if dot is None else node('add',dot,product)
            candidate=target.copy()
            for lane in range(4):
                if vector_result&(1<<lane):
                    x,y=operands[0][lane],operands[1][lane]
                    if opcode==15:result=dot
                    elif opcode==2:
                        if x!=y:raise RawShaderError('MAX must copy identical operands')
                        result=x
                    else:
                        result=node('mul',x,y)
                        if opcode==11:result=node('add',result,operands[2][lane])
                    candidate[lane]=result
                if zero&(1<<lane):candidate[lane]=node('literal',0)
                if one&(1<<lane):candidate[lane]=node('literal',1)
                if export and scalar_result&(1<<lane):candidate[lane]=scalar_value
            target[:]=candidate
            if not export:
                for lane in range(4):
                    if scalar_result&(1<<lane):registers[scalar_destination][lane]=scalar_value
            previous_scalar=scalar_value
    if not outputs:raise RawShaderError('no vertex exports')
    return {'sha256':hashlib.sha256(data).hexdigest(),'analysis':'experimental-vertex-arithmetic-dataflow',
        'nodes':nodes,'exports':{str(k):v for k,v in sorted(outputs.items())},'fetches':fetches,
        'allocations':allocations,'executed_instruction_addresses':executed,
        'constant_registers':sorted({n[1] for n in nodes if n[0]=='constant'}),
        'allocation_extent_verified':False,'vertex_index_input_verified':False,
        'hardware_float_edge_cases_validated':False,'native_vertex_pipeline':False,'gpu_executed':False}


def arithmetic(op,left,right):
    """Explicit finite normal/zero software policy; no denormal/overflow oracle."""
    if any(not math.isfinite(v) or (v!=0 and abs(v)<2.0**-126) for v in (left,right)):
        raise RawShaderError('arithmetic requires finite normal-or-zero operands')
    try:
        if op=='mul':result=0.0 if left==0 or right==0 else f32(left*right)
        elif op=='add':result=f32(left+right)
        else:raise RawShaderError('unknown arithmetic operation')
    except OverflowError as error:raise RawShaderError('vertex arithmetic overflow') from error
    if not math.isfinite(result) or (result!=0 and abs(result)<2.0**-126):
        raise RawShaderError('vertex arithmetic underflow/non-finite result unsupported')
    return result


def evaluate_vertex_dataflow(data,buffers,constants,index, *, dot4=False, scalar_constants=False):
    if type(index) is not int or not 0<=index<1<<24:raise RawShaderError('invalid explicit vertex index')
    proof=analyze_vertex_dataflow(data,dot4=dot4,scalar_constants=scalar_constants);nodes=proof['nodes'];values={};fetched=[]
    for fetch in proof['fetches']:
        if nodes[fetch['index_expression']]!=('initial',0,0):
            raise RawShaderError('only explicit unchanged r0.x auto-index supported')
        source=buffers.get(fetch['fetch_slot'])
        if not isinstance(source,bytes) or not 0<len(source)<=(256*1024 if dot4 else 4096) or len(source)%4:
            raise RawShaderError('bounded owned vertex buffer required')
        start=(index*fetch['stride_words']+fetch['offset_words'])*4
        count=1 if fetch['format'] in (6,25) else fetch['components']
        if start+count*4>len(source):raise RawShaderError('vertex input exceeds captured extent')
        if fetch['format']==6:
            word=int.from_bytes(source[start:start+4],'big')
            result=[f32(((word>>(i*8))&255)*f32(1/255)) for i in range(4)]
        elif fetch['format']==25:
            word=int.from_bytes(source[start:start+4],'big')
            result=[float(((word>>(i*16))&65535)^32768)-32768.0 for i in range(2)]
        else:result=struct.unpack_from('>'+str(count)+'f',source,start)
        if not all(math.isfinite(v) for v in result):raise RawShaderError('non-finite vertex input')
        fetched.append(result)
    def resolve(i):
        if i in values:return values[i]
        n=nodes[i];op=n[0]
        if op=='literal':result=float(n[1])
        elif op=='fetch':result=fetched[n[1]][n[2]]
        elif op=='constant':
            vector=constants.get(n[1])
            if not isinstance(vector,bytes) or len(vector)!=16:raise RawShaderError('missing exact BE constant vector')
            result=struct.unpack_from('>f',vector,n[2]*4)[0]
            if not math.isfinite(result):raise RawShaderError('non-finite constant input')
        elif op in ('mul','add'):result=arithmetic(op,resolve(n[1]),resolve(n[2]))
        elif op=='negate':result=-resolve(n[1])
        elif op=='initial':
            if n[1:]!=(0,0):raise RawShaderError('uninitialized temporary dependency')
            result=float(index)
        else:raise RawShaderError('invalid dataflow node')
        values[i]=result;return result
    # Check even temporary math later overwritten, not just live final exports.
    for i,n in enumerate(nodes):
        if n[0] in ('mul','add'):resolve(i)
    return {slot:[None if i is None else resolve(i) for i in lanes] for slot,lanes in proof['exports'].items()}


def _translate_vertex_arithmetic(data,vertex_bytes,base_bytes=None,*,r8_planes=False,color1280_mapping=False):
    vertex_stage=base_bytes is not None
    if color1280_mapping and not vertex_stage:raise RawShaderError('color coordinate mapping requires a vertex stage')
    if r8_planes and not vertex_stage:raise RawShaderError('R8 prefix is only valid for a vertex stage')
    uv_lanes=4 if r8_planes else 2
    r8_prefix_bytes=1280*720+2*640*360
    proof=analyze_vertex_dataflow(data);nodes=proof['nodes'];fetches=proof['fetches']
    constants=proof['constant_registers'];total=vertex_bytes+len(constants)*16
    if (type(vertex_bytes) is not int or not 0<vertex_bytes<=4096 or vertex_bytes%4 or total>4096 or
            not fetches or len({f['fetch_slot'] for f in fetches})!=1 or
            any(nodes[f['index_expression']]!=('initial',0,0) for f in fetches) or
            '62' not in proof['exports'] or any(v is None for v in proof['exports']['62']) or
            set(proof['exports'])-{'0','1','62'}):
        raise RawShaderError('unsupported bounded four-vertex compute profile')
    if vertex_stage and (type(base_bytes) is not int or base_bytes<0 or base_bytes%4 or
        base_bytes+total>(r8_prefix_bytes+512 if r8_planes else 4608) or
        set(proof['exports'])!={'0','1','62'} or any(v is None for v in proof['exports']['0'][:uv_lanes]) or
        any(v is None for v in proof['exports']['1'])):
        raise RawShaderError('bounded explicit UVxy/tint vertex stage required')
    if r8_planes and (not r8_prefix_bytes+16<=base_bytes<=r8_prefix_bytes+96 or (base_bytes-r8_prefix_bytes-16)%16):
        raise RawShaderError('explicit R8 image/tint/pixel-constant prefix required')
    statements=[];expressions={}
    for i,f in enumerate(fetches):
        count=1 if f['format']==6 else f['components']
        base=f"(index*{f['stride_words']}u+{f['offset_words']}u)"
        statements.append(f'if ({base}+{count}u>{vertex_bytes//4}u) return;')
        if f['format']==6:statements.append(f'uint packed{i}=readBE({base});')
        for c in range(f['components']):
            value=(f'float((packed{i}>>{c*8}u)&255u)*(1.0/255.0)' if f['format']==6 else
                   f'uintBitsToFloat(readBE({base}+{c}u))')
            statements.append(f'precise float fetch{i}_{c}={value};')
            statements.append(f'if (isnan(fetch{i}_{c}) || isinf(fetch{i}_{c})) return;')
    def expression(i):
        if i in expressions:return expressions[i]
        n=nodes[i];op=n[0]
        if op=='initial':
            if n[1:]!=(0,0):raise RawShaderError('unknown initial compute dependency')
            value='float(index)'
        elif op=='literal':value=str(n[1])+'.0'
        elif op=='fetch':value=f'fetch{n[1]}_{n[2]}'
        elif op=='constant':
            word=vertex_bytes//4+constants.index(n[1])*4+n[2]
            value=f'n{i}'
            statements.append(f'float {value}=uintBitsToFloat(readBE({word}u));')
            statements.append(f'if (isnan({value}) || isinf({value})) return;')
        elif op in ('mul','add'):
            left,right=expression(n[1]),expression(n[2]);value=f'n{i}'
            statements.append(f'if (!regular({left}) || !regular({right})) return;')
            equation=f'(({left}==0.0 || {right}==0.0)?0.0:({left}*{right}))' if op=='mul' else f'({left}+{right})'
            statements.append(f'precise float {value}={equation};')
            statements.append(f'if (!regular({value})) return;')
        else:raise RawShaderError('unknown compute expression')
        expressions[i]=value;return value
    for i,n in enumerate(nodes):
        if n[0] in ('mul','add'):expression(i)
    stores=[]
    if vertex_stage:
        for slot,target,count in (('62','gl_Position',4),('0','coordinate',uv_lanes),('1','vertexTint',4)):
            stores.append(f'{target}=vec{count}({",".join(expression(n) for n in proof["exports"][slot][:count])});')
        if color1280_mapping:
            # Separate opt-in adapter, not a changed interpretation of shader
            # exports. A padded 2048x1024 integer positive-height viewport with
            # reverse Z is required, while the scissor/target remain 1280x720.
            # Power-of-two denominators represent the captured W1 rectangle's
            # half-pixel corners exactly and keep it entirely inside clip XY.
            # A 1280x720 shader-offset viewport cut tiny slivers from this strip
            # and produced observable seam interpolation error on llvmpipe.
            stores.extend(('if (gl_Position.w!=1.0) { gl_Position=vec4(0,0,2,1); return; }',
                'precise vec2 scaledXY=gl_Position.xy*vec2(0.625,-0.703125);',
                'precise vec2 mappedXY=scaledXY+vec2(-0.37451171875,-0.2958984375);',
                'gl_Position.xy=mappedXY;'))
    else:
        for slot,base in (('62',1),('0',2),('1',3)):
            for lane,node in enumerate(proof['exports'].get(slot,[])):
                if node is not None:stores.append(f'result.words[{base}u+index*3u][{lane}]=floatBitsToUint({expression(node)});')
    source='''#version 450
'''+(f'layout(location=0) out vec{uv_lanes} coordinate;\nlayout(location=1) out vec4 vertexTint;\n' if vertex_stage else
    'layout(local_size_x=1) in;\n')+'''
layout(set=0,binding=0,std430) readonly buffer Source { uint words[]; } source;
'''+('' if vertex_stage else 'layout(set=0,binding=1,std430) buffer Result { uvec4 words[]; } result;\n')+'''
uint readBE(uint at) {
'''+f'    uint v=source.words[at+{base_bytes//4 if vertex_stage else 0}u];\n'+'''
    return (v<<24)|((v&0xff00u)<<8)|((v>>8)&0xff00u)|(v>>24);
}
bool regular(float v) {
    uint bits=floatBitsToUint(v)&0x7fffffffu;
    return bits==0u || (bits>=0x00800000u && bits<0x7f800000u);
}
void main() {
'''+('    uint index=uint(gl_VertexIndex);\n'
     f'    gl_Position=vec4(0,0,2,1); coordinate=vec{uv_lanes}(0); vertexTint=vec4(0);\n'+
     f'    if(index>=4u || source.words.length()<{(base_bytes+total)//4}) return;\n' if vertex_stage else
     '    uint index=gl_GlobalInvocationID.x;\n'+f'    if(index>=4u || result.words.length()<14 || source.words.length()<{total//4}) return;\n')+''.join(
        '    '+line+'\n' for line in statements+stores)+'}\n'
    if vertex_stage:
        if color1280_mapping:
            proof=dict(proof,coordinate_adapter='color1280-padded-viewport-half-pixel-w1',
                required_native_viewport=[0,0,2048,1024,1,0],required_native_scissor=[0,0,1280,720],source_w_one_required=True,
                title_raster_state_replayed=False,original_clip_boundary_preservation_verified=False)
        return source,dict(proof,probe_stage='vertex',vertex_bytes=vertex_bytes,base_bytes=base_bytes,
            input_bytes=base_bytes+total,maximum_vertices=4,explicit_fetch_slot=fetches[0]['fetch_slot'],
            exported_interface={'position':'xyzw','parameter0':'xyzw' if r8_planes else 'xy','parameter1':'xyzw'},
            input_prefix_profile='three-R8-images' if r8_planes else 'bounded-small-input',
            rejection_position=[0,0,2,1],whole_primitive_rejection_guaranteed=False,
            title_linkage_verified=False,frame_presented=False,guest_writeback=False)
    return source,dict(proof,probe_stage='compute',vertex_bytes=vertex_bytes,input_bytes=total,
        output_bytes=224,maximum_invocations=4,explicit_fetch_slot=fetches[0]['fetch_slot'],
        input_byte_order='big-endian-captured-dwords',arithmetic_policy='finite-normal-zero-unfused-f32',
        frame_presented=False,guest_writeback=False)


def translate_vertex_arithmetic_compute(data,vertex_bytes):
    """Four-invocation compute references; no native vertex pipeline."""
    return _translate_vertex_arithmetic(data,vertex_bytes)


def translate_vertex_arithmetic_stage(data,vertex_bytes,base_bytes):
    """Diagnostic vertex stage: position, parameter0.xy and parameter1.xyzw.

    Owned vertex/constants input begins at explicit base_bytes. Invalid vertices
    return an outside-far-plane position, NOT a whole-primitive rejection proof.
    The fixture must prevalidate complete input; this is not a guest draw plan.
    """
    return _translate_vertex_arithmetic(data,vertex_bytes,base_bytes)


def translate_r8_vertex_arithmetic_stage(data,vertex_bytes,base_bytes):
    """Separate bounded three-R8 prefix, full parameter0.xyzw and vertex tint.
    Does not widen the small-texture stage cap or prove title linkage.
    """
    return _translate_vertex_arithmetic(data,vertex_bytes,base_bytes,r8_planes=True)


def translate_color1280_vertex_stage(data,vertex_bytes,base_bytes,*,r8_planes=False):
    """Explicit W1 color coordinate experiment, not general guest raster replay.

    Requires integer 2048x1024 positive-height viewport, 1280x720 scissor/target
    and reversed Z. Y/half-pixel correction happens after guest exports. Shifting
    before clipping is only tested for the bounded full rectangle, not arbitrary edges.
    This does not establish whole-primitive rejection or PM4 state admission.
    """
    if type(r8_planes) is not bool:raise RawShaderError('explicit boolean R8 interface required')
    return _translate_vertex_arithmetic(data,vertex_bytes,base_bytes,
        r8_planes=r8_planes,color1280_mapping=True)
