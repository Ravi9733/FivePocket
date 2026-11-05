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
from packed_geometry_fetch import decode_packed_geometry_fetch, unpack_geometry_word
from scalar_constant import decode_scalar_constant, scalar_constant_node
from loading_shader_math import loading_scalar_node, evaluate_loading_operation, LOADING_OPERATIONS


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
                # A terminating EXEC_END may carry no instructions at all: it
                # ends the control block without executing anything, which is
                # what the stencil pass's vertex stage does. An ordinary EXEC
                # still has to execute something, so count 0 stays refused there.
                if not (count<=6 and (count>=1 or op==2)) or address+count>extent or sequence>>(count*2):
                    raise RawShaderError('invalid vertex EXEC range/sequence')
                body=min(body,address);controls.append(('exec',address,count,sequence))
                if op==2:ended=True;break
            else:raise RawShaderError('conditional/loop/unknown vertex control unsupported')
        if ended:break
    if not ended or (len(controls)+1)//2>body:
        raise RawShaderError('unterminated or overlapping vertex control storage')
    return words,controls


def vertex_supported_opcodes(*, dot4=False, scalar_constants=False, loading_math=False, predicate_profile=False,packed_geometry=False,tonemap=False,scalar_gt_zero=False):
    """Shared encoding inventory; operand/modifier checks remain in the parser."""
    vector = (0,1,2,5,11,14,15,16) if loading_math else (1,2,11,15) if dot4 else (1,2,11)
    scalar = (0,1,2,3,5,12,14,19,22,25,40,42,43,44,45,46,47,50) if loading_math else (42,43,44,45,46,47,50) if scalar_constants else (50,)
    if predicate_profile and loading_math:scalar=(*scalar,28)
    if packed_geometry and loading_math:
        vector=(*vector,6,17)
        # LOG_IEEE is scalar16 in both stages. Reuse the existing finite
        # logarithm policy; LOG_CLAMP15 and other pixel-only operations stay
        # rejected. Offline arithmetic support is not native draw admission.
        scalar=(*scalar,16)
    # DOT2ADD is ordinary ALU, not packed geometry: it has been implemented
    # alongside DP4/DP3 all along and only ever reached through the packed
    # profile. The tonemap vertex stage needs it without any of the indexed
    # geometry machinery, so it is admitted on its own here. SGE stays behind
    # the packed profile, which is the only thing that has evidence for it.
    if tonemap and loading_math and 17 not in vector:vector=(*vector,17)
    if scalar_gt_zero and loading_math:scalar=(*scalar,8)
    return vector, scalar


def analyze_vertex_dataflow(data, *, dot4=False, scalar_constants=False, loading_math=False,
                           packed_geometry=False, predicate_profile=False, address_profile=False,
                           tonemap=False,scalar_gt_zero=False,base_texture_profile=False):
    if type(base_texture_profile) is not bool or (base_texture_profile and
            (not loading_math or packed_geometry or predicate_profile or address_profile or tonemap)):
        raise RawShaderError('base vertex texture fetch requires separate straight-line loading analysis')
    # SETGTs is a scalar 0/1 result, not a predicate-register mutation.
    # Keep this offline extension separate from all existing native profiles.
    if type(scalar_gt_zero) is not bool or (scalar_gt_zero and
            (not loading_math or not dot4 or not scalar_constants or packed_geometry or
             predicate_profile or address_profile or tonemap)):
        raise RawShaderError('scalar SETGT requires separate unpacked straight-line loading analysis')
    if type(tonemap) is not bool or (tonemap and (packed_geometry or predicate_profile or
            address_profile or not loading_math or not dot4 or not scalar_constants)):
        raise RawShaderError('tonemap vertex requires the separate unpacked straight-line loading profile')
    if loading_math and not (dot4 and scalar_constants):
        raise RawShaderError('loading math requires explicit dot/scalar profiles')
    if predicate_profile and not (packed_geometry and loading_math):
        raise RawShaderError('vertex predication requires explicit packed geometry/loading profile')
    if type(address_profile) is not bool or (address_profile and
            (not packed_geometry or not loading_math or predicate_profile)):
        raise RawShaderError('vertex address register requires separate unpredicated packed loading profile')
    words,controls=schedule(data)
    vector_ops,scalar_ops=vertex_supported_opcodes(dot4=dot4,scalar_constants=scalar_constants,loading_math=loading_math,
        predicate_profile=predicate_profile,packed_geometry=packed_geometry,tonemap=tonemap,scalar_gt_zero=scalar_gt_zero)
    if address_profile:scalar_ops=(*scalar_ops,23,24)
    nodes=[];intern={}
    validation_nodes=None;validation_groups=[];predicate=None
    def node(op,*args):
        key=(op,*args)
        if key not in intern:
            if len(nodes)==4096:raise RawShaderError('vertex expression budget exceeded')
            intern[key]=len(nodes);nodes.append(key)
        result=intern[key]
        if validation_nodes is not None and op not in ('initial','constant','literal','predicate_condition','predicate_select'):
            validation_nodes.append(result)
        return result
    registers=[[node('initial',r,c) for c in range(4)] for r in range(64)]
    outputs={};fetches=[];texture_fetches=[];allocations=[];allocated=set();executed=[];previous=None;previous_scalar=None
    address_register=None;address_writes=[]
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
                if a&31==1 and base_texture_profile:
                    variable=(63<<5)|(63<<12)|(31<<20)|(63<<26)
                    if a&~variable!=1|(1<<19) or b&~4095!=0x0F1FF000 or c!=0x4000:
                        raise RawShaderError('only normalized inherited-filter base-LOD 2D vertex texture fetch supported')
                    swizzle=[(b>>(i*3))&7 for i in range(4)]
                    if 6 in swizzle:raise RawShaderError('reserved vertex texture destination swizzle')
                    if len(texture_fetches)==16:raise RawShaderError('vertex texture fetch budget exceeded')
                    source=(a>>5)&63;destination=(a>>12)&63
                    fetch_id=len(texture_fetches)
                    texture_fetches.append(dict(instruction_address=address,slot=(a>>20)&31,
                        destination=destination,destination_swizzle=swizzle,
                        coordinates=[registers[source][(a>>s)&3] for s in (26,28)],
                        explicit_base_lod=True,sampling_executed=False))
                    for lane,select in enumerate(swizzle):
                        if select<4:registers[destination][lane]=node('texture_sample',fetch_id,select)
                        elif select<6:registers[destination][lane]=node('literal',select-4)
                    continue
                decoders={6:decode_unorm8_fetch,25:_sint16_vertex_fetch}
                if packed_geometry:decoders.update({fmt:decode_packed_geometry_fetch for fmt in (7,31,32)})
                decoder=decoders.get((b>>16)&63,_float_vertex_fetch)
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
            guard=None
            if predicate_profile:
                predicated=bool(b&(1<<28))
                if not predicated and b&(1<<27):raise RawShaderError('unused vertex predicate condition unsupported')
                if predicated:
                    if predicate is None:raise RawShaderError('unknown initial vertex predicate')
                    if scalar_opcode==28:raise RawShaderError('predicated vertex predicate mutation unsupported')
                    guard=node('predicate_condition',predicate,bool(b&(1<<27)))
            if opcode not in vector_ops or scalar_opcode not in scalar_ops:
                raise RawShaderError('unsupported vertex vector/scalar operation in the selected profile')
            if a&((1<<6)|(1<<7)|((0 if loading_math else 3)<<24)) or b&(0x18000000 if address_profile else 0xE0000000 if predicate_profile else 0xF8000000 if loading_math else 0xFF000000):
                raise RawShaderError('relative/absolute/clamp/negate/predicate vertex ALU unsupported')
            # Constant addressing flags count CONSTANT operands, not operand
            # positions (Xenia/ReX AluInstruction::src_const_is_addressed).
            # All operands below retain the OLD a0 during scalar/vector coissue.
            def addressed(operand):
                earlier_constant=any(not c&(1<<(31-j)) for j in range(operand))
                return bool(b&(1<<(30 if earlier_constant else 31)))
            def constant(operand,source,lane):
                if not addressed(operand):return node('constant',source,lane)
                if not address_profile or not b&(1<<29):
                    raise RawShaderError('loop-relative vertex constants unsupported')
                if address_register is None:raise RawShaderError('unknown initial vertex address register')
                return node('relative_constant',source,lane,address_register)
            validation_nodes=[] if predicate_profile else None
            alu_words=(a,b&~(3<<27) if predicate_profile else b,c)
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
            scalar_value=previous_scalar;next_address=None
            if scalar_opcode in (23,24):
                source,swizzle,temporary=c&255,b&255,bool(c&(1<<29))
                if temporary and source&64:raise RawShaderError('relative scalar address source unsupported')
                def scalar_operand(lane):
                    value=registers[source&63][lane] if temporary else constant(2,source,lane)
                    if temporary and source&128:value=node('absolute',value)
                    return node('negate',value) if b&(1<<24) else value
                left=scalar_operand(((swizzle>>6)+3)&3);right=scalar_operand(swizzle&3)
                # MAX_AS updates a0 even with zero scalar write mask. Its
                # scalar/previous result remains MAX, not the integer address.
                next_address=node('address_round',left,scalar_opcode==23)
                scalar_value=node('maximum',left,right)
            elif loading_math and scalar_opcode not in (*range(42,48),50):
                if not c&(1<<29) and addressed(2):
                    raise RawShaderError('addressed non-MAX_AS scalar constant unsupported')
                scalar_value=loading_scalar_node(alu_words,registers,node,previous_scalar,
                    predicate_profile=predicate_profile,scalar_gt_zero=scalar_gt_zero,
                    geometry_profile=packed_geometry)
            elif scalar_opcode!=50:
                scalar_value=scalar_constant_node(decode_scalar_constant(alu_words,coissued=loading_math,
                    allow_clamp=loading_math,allow_export=loading_math),registers,node)
            if scalar_result and scalar_value is None:
                raise RawShaderError('previous scalar result dependency unsupported')
            # Previous-scalar keeps the raw operation result. Saturation belongs
            # only to destination stores (Xenia ProcessAluInstruction/StoreResult).
            scalar_stored=node('saturate',scalar_value) if a&(1<<25) and scalar_value is not None else scalar_value
            scalar_destination=(a>>8)&63
            if not export and scalar_destination==destination and vector_result&scalar_result:
                raise RawShaderError('overlapping scalar/vector temporary writes unsupported')
            # Read all operands from pre-instruction registers before any writes.
            operands=[]
            if vector_result:
                for operand in range(3 if opcode in (11,14,17) else 2):
                    shift=16-operand*8;source=(c>>shift)&255;swizzle=(b>>shift)&255
                    temporary=bool(c&(1<<(31-operand)))
                    if temporary and (source&64 if loading_math else source>=64):raise RawShaderError('relative/absolute temporary source unsupported')
                    values=[]
                    for lane in range(4):
                        component=((swizzle>>(lane*2))+lane)&3
                        value=registers[source&63][component] if temporary else constant(operand,source,component)
                        values.append(node('absolute',value) if temporary and source&128 else value)
                    operands.append([node('negate',value) for value in values] if loading_math and b&(1<<(26-operand)) else values)
            dot=None
            if vector_result and opcode in (15,16,17):
                # Xenos DOT2ADD uses x/y products, then src2.x; all three
                # operands are swizzled/read before coissued stores. Same
                # unfused left-fold policy as the existing pixel implementation.
                for lane in range(4 if opcode==15 else 3 if opcode==16 else 2):
                    product=node('mul',operands[0][lane],operands[1][lane])
                    dot=product if dot is None else node('add',dot,product)
                if opcode==17:dot=node('add',dot,operands[2][0])
            candidate=target.copy()
            for lane in range(4):
                if vector_result&(1<<lane):
                    x,y=operands[0][lane],operands[1][lane]
                    if opcode in (15,16,17):result=dot
                    elif opcode==0:result=node('add',x,y)
                    elif opcode==5:result=node('greater',x,y)
                    elif opcode==6:result=node('greater_equal',x,y)
                    elif opcode==14:result=node('select_gt0',x,y,operands[2][lane])
                    elif opcode==2:
                        if loading_math:result=node('maximum',x,y)
                        else:
                            if x!=y:raise RawShaderError('MAX must copy identical operands')
                            result=x
                    else:
                        result=node('mul',x,y)
                        if opcode==11:result=node('add',result,operands[2][lane])
                    candidate[lane]=node('saturate',result) if a&(1<<24) else result
                if zero&(1<<lane):candidate[lane]=node('literal',0)
                if one&(1<<lane):candidate[lane]=node('literal',1)
                if export and scalar_result&(1<<lane):candidate[lane]=scalar_stored
            def selected(new,old):
                return new if guard is None or new==old else node('predicate_select',guard,new,old)
            target[:]=[selected(new,old) for new,old in zip(candidate,target)]
            if not export:
                for lane in range(4):
                    if scalar_result&(1<<lane):
                        registers[scalar_destination][lane]=selected(scalar_stored,registers[scalar_destination][lane])
            previous_scalar=selected(scalar_value,previous_scalar)
            if next_address is not None:
                address_register=next_address;address_writes.append(dict(address=address,node=next_address))
            if scalar_opcode==28:
                # Xenia-derived ucode/interpreter: nonzero -> scalar0/p0=true;
                # signed zero -> scalar1/p0=false. Read pre-coissue operands.
                predicate=node('predicate_condition',scalar_value,False)
            if predicate_profile:
                validation_groups.append(dict(address=address,guard=guard,nodes=list(dict.fromkeys(validation_nodes))))
            validation_nodes=None
    if not outputs:raise RawShaderError('no vertex exports')
    return {'sha256':hashlib.sha256(data).hexdigest(),'analysis':'experimental-vertex-arithmetic-dataflow',
        'nodes':nodes,'exports':{str(k):v for k,v in sorted(outputs.items())},'fetches':fetches,
        'allocations':allocations,'executed_instruction_addresses':executed,
        'constant_registers':sorted({n[1] for n in nodes if n[0]=='constant'}),
        'allocation_extent_verified':False,'vertex_index_input_verified':False,
        'hardware_float_edge_cases_validated':False,'native_vertex_pipeline':False,'gpu_executed':False,
        'loading_math':loading_math,'transcendental_hardware_approximation_verified':False,
        'packed_geometry':packed_geometry,'half_extended_range_supported':False,
        'predicate_profile':predicate_profile,'predicate':predicate,'previous_scalar':previous_scalar,
        'address_profile':address_profile,'address_register':address_register,'address_writes':address_writes,
        'dynamic_constants':any(n[0]=='relative_constant' for n in nodes),
        **({'base_texture_profile':True,'texture_fetches':texture_fetches,
            'texture_results_are_explicit_inputs':True,'sampling_executed':False} if base_texture_profile else {}),
        'address_out_of_range_policy':'reject; no wrap or clamped constant index',
        'validation_groups':validation_groups,'predicate_hardware_verified':False}


def arithmetic(op,left,right,*,denormal_multiply=False):
    """Finite policy with opt-in SM3 input-denormal zero multiplication only.

    Pinned ReX c94f5ebd dxbc_translator_alu.cpp kMul/kMad explicitly replaces
    zero/denormal multiplication with +0. This does NOT admit nonfinite inputs,
    subnormal ADD inputs, or nonzero subnormal results from normal operands.
    """
    if type(denormal_multiply) is not bool:raise RawShaderError('explicit denormal multiply policy required')
    if denormal_multiply and op=='mul':
        if not all(math.isfinite(v) for v in (left,right)):
            raise RawShaderError('denormal multiply profile requires finite operands')
        if any(abs(v)<2.0**-126 for v in (left,right)):return 0.0
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


def evaluate_vertex_dataflow(data,buffers,constants,index, *, dot4=False, scalar_constants=False, loading_math=False,
                             position_only=False, packed_geometry=False, geometry_ieee=False,predicate_profile=False,include_state=False,
                             address_profile=False,denormal_multiply=False,tonemap=False,scalar_gt_zero=False,
                             base_texture_profile=False,texture_samples=None,base_texture_ieee=False):
    if type(base_texture_ieee) is not bool or (base_texture_ieee and not base_texture_profile):
        raise RawShaderError('vertex zero reciprocal requires the explicit base-texture profile')
    if texture_samples is not None and (not base_texture_profile or not isinstance(texture_samples,dict)):
        raise RawShaderError('explicit vertex texture sample dictionary requires its own profile')
    if type(denormal_multiply) is not bool or (denormal_multiply and
            (not address_profile or not packed_geometry or not loading_math or geometry_ieee or predicate_profile)):
        raise RawShaderError('denormal multiplication requires the separate addressed finite geometry profile')
    if geometry_ieee and not (packed_geometry and loading_math):
        raise RawShaderError('geometry IEEE reference requires the explicit packed loading profile')
    if geometry_ieee:
        from geometry_ieee_math import evaluate_geometry_ieee
    if position_only and not loading_math:
        raise RawShaderError('position-only reference requires explicit loading profile')
    if type(index) is not int or not 0<=index<1<<24:raise RawShaderError('invalid explicit vertex index')
    proof=analyze_vertex_dataflow(data,dot4=dot4,scalar_constants=scalar_constants,loading_math=loading_math,
        packed_geometry=packed_geometry,predicate_profile=predicate_profile,address_profile=address_profile,
        tonemap=tonemap,scalar_gt_zero=scalar_gt_zero,base_texture_profile=base_texture_profile);nodes=proof['nodes'];values={};fetched=[]
    for fetch in proof['fetches']:
        if nodes[fetch['index_expression']]!=('initial',0,0):
            raise RawShaderError('only explicit unchanged r0.x auto-index supported')
        source=buffers.get(fetch['fetch_slot'])
        if not isinstance(source,bytes) or not 0<len(source)<=(256*1024 if dot4 else 4096) or len(source)%4:
            raise RawShaderError('bounded owned vertex buffer required')
        start=(index*fetch['stride_words']+fetch['offset_words'])*4
        count=1 if fetch['format'] in (6,7,25,31) else 2 if fetch['format']==32 else fetch['components']
        if start+count*4>len(source):raise RawShaderError('vertex input exceeds captured extent')
        if fetch['format']==6:
            word=int.from_bytes(source[start:start+4],'big')
            result=[f32(((word>>(i*8))&255)*f32(1/255)) for i in range(4)]
        elif fetch['format']==25:
            word=int.from_bytes(source[start:start+4],'big')
            result=[float(((word>>(i*16))&65535)^32768)-32768.0 for i in range(2)]
        elif fetch['format'] in (7,31,32):
            result=[]
            for offset in range(count):
                result.extend(unpack_geometry_word(int.from_bytes(source[start+offset*4:start+offset*4+4],'big'),
                    31 if fetch['format']==32 else fetch['format']))
        else:result=struct.unpack_from('>'+str(count)+'f',source,start)
        if not all(math.isfinite(v) for v in result):raise RawShaderError('non-finite vertex input')
        fetched.append(result)
    def resolve(i):
        if i is None:raise RawShaderError('executed undefined predicated vertex value')
        if i in values:return values[i]
        n=nodes[i];op=n[0]
        if op=='literal':result=float(n[1])
        elif op=='predicate_select':result=resolve(n[2] if resolve(n[1]) else n[3])
        elif op=='predicate_condition':
            source=resolve(n[1])
            if source not in (0.,1.):raise RawShaderError('invalid vertex predicate state')
            result=float(bool(source)==n[2])
        elif op=='predicate_setne':
            source=resolve(n[1])
            if not math.isfinite(source) or (source!=0 and abs(source)<2**-126):
                raise RawShaderError('vertex predicate source outside finite normal-or-zero domain')
            result=float(source==0.)
        elif op=='fetch':result=fetched[n[1]][n[2]]
        elif op=='texture_sample':
            vector=None if texture_samples is None else texture_samples.get(n[1])
            if not isinstance(vector,(list,tuple)) or len(vector)!=4 or vector[n[2]] is None:
                raise RawShaderError('missing explicit vertex texture result component')
            result=vector[n[2]]
            if not math.isfinite(result):raise RawShaderError('nonfinite vertex texture result')
            try:rounded=f32(result)
            except (OverflowError,struct.error) as error:raise RawShaderError('invalid vertex texture float32') from error
            if result!=rounded:raise RawShaderError('vertex texture inputs must be explicit float32')
        elif op=='address_round':
            value=resolve(n[1])
            if not math.isfinite(value) or (value!=0 and abs(value)<2**-126):
                raise RawShaderError('vertex address source outside finite normal-or-zero domain')
            rounded=f32(value+0.5) if n[2] else value
            result=float(max(-256,min(255,math.floor(rounded))))
        elif op in ('constant','relative_constant'):
            register=n[1]+int(resolve(n[3])) if op=='relative_constant' else n[1]
            if not 0<=register<256:raise RawShaderError('vertex constant address outside captured bank')
            vector=constants.get(register)
            if not isinstance(vector,bytes) or len(vector)!=16:raise RawShaderError('missing exact BE constant vector')
            result=struct.unpack_from('>f',vector,n[2]*4)[0]
            if not math.isfinite(result):raise RawShaderError('non-finite constant input')
        elif op in ('mul','add'):
            result=(evaluate_geometry_ieee(op,[resolve(n[1]),resolve(n[2])]) if geometry_ieee
                    else arithmetic(op,resolve(n[1]),resolve(n[2]),denormal_multiply=denormal_multiply))
        elif op=='negate':result=-resolve(n[1])
        elif base_texture_ieee and op=='maximum' and n[1]==n[2] and math.isinf(resolve(n[1])):
            # MAX(src,src) is the original export copy, including infinity from
            # RECIP_IEEE. Do not broaden other operations or admit a NaN.
            result=resolve(n[1])
        elif op in LOADING_OPERATIONS:
            result=(evaluate_geometry_ieee if geometry_ieee else evaluate_loading_operation)(op,[resolve(j) for j in n[1:]])
        elif op=='saturate':result=min(1.0,max(0.0,resolve(n[1])))
        elif op=='reciprocal':
            operand=resolve(n[1])
            if base_texture_ieee and operand==0:
                # Original scalar19 is RECIP_IEEE, not clamp-to-zero/max.
                # Only +/-0 is extended; downstream nonfinite arithmetic
                # still refuses. Exporting infinity is not raster admission.
                result=math.copysign(math.inf,operand);values[i]=result;return result
            if geometry_ieee and math.isinf(operand):
                result=math.copysign(0.0,operand);values[i]=result;return result
            if not math.isfinite(operand) or abs(operand)<2**-126:
                raise RawShaderError('reciprocal requires finite nonzero normal input')
            try:result=f32(1.0/operand)
            except OverflowError as error:raise RawShaderError('reciprocal overflow') from error
            if not math.isfinite(result) or abs(result)<2**-126:
                raise RawShaderError('reciprocal nonfinite/subnormal result')
        elif op=='initial':
            if n[1:]!=(0,0):raise RawShaderError('uninitialized temporary dependency')
            result=float(index)
        else:raise RawShaderError('invalid dataflow node')
        values[i]=result;return result
    # An explicit position-compute reference evaluates only the dependency cone
    # emitted by that compute module. No full-shader numerical acceptance is
    # inferred from it: non-position math may still be unsupported/nonfinite.
    # Full instruction/side-effect analysis above ALWAYS remains in force.
    if position_only:
        lanes=proof['exports'].get('62')
        if not lanes or any(i is None for i in lanes):raise RawShaderError('complete position export required')
        position=[resolve(i) for i in lanes]
        if geometry_ieee and any(not math.isfinite(v) or (v!=0 and abs(v)<2**-126) for v in position):
            raise RawShaderError('geometry IEEE exported position is outside finite normal/zero policy')
        return {'62':position}
    for fetch in proof.get('texture_fetches',[]):
        for coordinate in fetch['coordinates']:resolve(coordinate)
    for i,n in enumerate(nodes):
        if n[0]=='texture_sample':resolve(i) # Executed reads, even if overwritten.
    # Default: check even temporary math later overwritten, not just exports.
    if predicate_profile:
        for group in proof['validation_groups']:
            if group['guard'] is None or resolve(group['guard']):
                for i in group['nodes']:resolve(i)
    else:
        for i,n in enumerate(nodes):
            if n[0] in ('mul','add','reciprocal','address_round','relative_constant') or n[0] in LOADING_OPERATIONS:resolve(i)
    exported={slot:[None if i is None else resolve(i) for i in lanes] for slot,lanes in proof['exports'].items()}
    if geometry_ieee and any(v is not None and (not math.isfinite(v) or (v!=0 and abs(v)<2**-126))
                             for lanes in exported.values() for v in lanes):
        raise RawShaderError('geometry IEEE exported value is outside finite normal/zero policy')
    if include_state:return dict(exports=exported,predicate=None if proof['predicate'] is None else resolve(proof['predicate']),
        address_register=None if proof['address_register'] is None else resolve(proof['address_register']),
        address_history=[dict(address=w['address'],value=resolve(w['node'])) for w in proof['address_writes']],
        resolved_relative_constants=sorted({nodes[i][1]+int(resolve(nodes[i][3])) for i in values if nodes[i][0]=='relative_constant'}),
        previous_scalar=None if proof['previous_scalar'] is None else resolve(proof['previous_scalar']))
    return exported


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
