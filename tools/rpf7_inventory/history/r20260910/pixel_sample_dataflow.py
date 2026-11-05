"""Experimental pixel arithmetic with EXPLICIT sample results, not a sampler.

Unconditional normalized 2D fetch metadata, scalar MUL_CONST/MUL_PREV/MAX-copy,
vector ADD/MUL/MAX-copy/DP3/DP4/CND_EQ. Explicit finite normal/zero, left-fold
unfused dot policy; both conditional choices must validate. No implied
LOD, rasterization, texture reads, GPU-time constants or allocation extent proof.
"""
import hashlib
import math
import struct
from raw_shader_analysis import RawShaderError
from scalar_mul_const import decode_scalar_mul_const
from scalar_constant import decode_scalar_constant, scalar_constant_node
from vertex_arithmetic_dataflow import schedule,arithmetic


def texture_fetch_metadata(words,registers,address):
    a,b,c=words
    variable_a=(63<<5)|(63<<12)|(31<<20)|(63<<26)
    if a&~variable_a != (1|(1<<19)) or b&~4095!=0x1F1FF000 or c!=0x4000:
        raise RawShaderError('only unmodified valid-only normalized 2D inherited-filter fetch metadata supported')
    swizzle=[(b>>(i*3))&7 for i in range(4)]
    if 6 in swizzle:raise RawShaderError('reserved texture destination swizzle')
    source=(a>>5)&63
    return {'instruction_address':address,'slot':(a>>20)&31,'destination':(a>>12)&63,
        'destination_swizzle':swizzle,'coordinates':[registers[source][(a>>shift)&3] for shift in (26,28)],
        'computed_lod_requested':True,'sampling_executed':False}


def analyze_pixel_sample_dataflow(data, *, ui_operations=False, extended_math=False):
    if extended_math and not ui_operations:
        raise RawShaderError('extended math requires explicit UI arithmetic profile')
    words,controls=schedule(data);nodes=[];intern={}
    def node(op,*args):
        key=(op,*args)
        if key not in intern:
            if len(nodes)==4096:raise RawShaderError('pixel expression budget exceeded')
            intern[key]=len(nodes);nodes.append(key)
        return intern[key]
    registers=[[node('initial',r,c) for c in range(4)] for r in range(64)]
    outputs={};fetches=[];allocations=[];executed=[];previous_scalar=None
    for control in controls:
        if control[0]=='alloc':
            if control[1]!=2 or allocations:raise RawShaderError('only one color allocation supported')
            allocations.append({'kind':2,'encoded_size':control[2],'unserialized':control[3]})
        if control[0]!='exec':continue
        for address in range(control[1],control[1]+control[2]):
            if len(executed)==256:raise RawShaderError('pixel instruction budget exceeded')
            executed.append(address);a,b,c=words[address*3:address*3+3]
            if control[3]&(1<<((address-control[1])*2)):
                if len(fetches)==32:raise RawShaderError('pixel fetch budget exceeded')
                fetch=texture_fetch_metadata((a,b,c),registers,address);index=len(fetches);fetches.append(fetch)
                candidate=registers[fetch['destination']].copy()
                for lane,select in enumerate(fetch['destination_swizzle']):
                    if select<4:candidate[lane]=node('sample',index,select)
                    elif select<6:candidate[lane]=node('literal',select-4)
                registers[fetch['destination']]=candidate
                continue
            op=(c>>24)&31;scalar_op=a>>26;export=bool(a&(1<<15));vd=a&63;sd=(a>>8)&63
            vector_ops=(0,1,2,7,11,12,15,16,17) if extended_math else (0,1,2,7,11,12,15,16) if ui_operations else (0,1,2,12,15,16)
            scalar_ops=(2,3,5,19,42,43,44,45,46,47,50) if extended_math else (2,3,5,42,43,50) if ui_operations else (3,5,42,43,50)
            if op not in vector_ops or scalar_op not in scalar_ops:
                raise RawShaderError('unsupported pixel vector/scalar operation')
            if a&((1<<6)|(1<<7)|((2 if ui_operations else 3)<<24)) or (b&(0xF8000000 if ui_operations else 0xFF000000)) or (not export and a&(1<<14)):
                raise RawShaderError('modified pixel ALU unsupported')
            vm,sm=(a>>16)&15,(a>>20)&15
            if export:
                if vd>=4 or not allocations:raise RawShaderError('unallocated/non-color export unsupported')
                vector_mask,scalar_mask=vm&~sm,sm&~vm
                zero=15&~(vm|sm) if a&(1<<14) else 0;one=vm&sm
                target=outputs.get(vd,[None]*4).copy()
            else:
                if vd==sd and vm&sm:raise RawShaderError('overlapping scalar/vector temporary writes unsupported')
                vector_mask,scalar_mask,zero,one=vm,sm,0,0;target=registers[vd].copy()
            # Snapshot BOTH scalar and vector operands before publishing any register.
            scalar=previous_scalar
            if extended_math and scalar_op in range(42,48):
                scalar=scalar_constant_node(decode_scalar_constant((a,b,c),coissued=True),registers,node)
            elif scalar_op in (42,43):
                scalar_plan=decode_scalar_mul_const((a,b,c))  # temporary destination only
                scalar=node('mul',node('constant',scalar_plan['constant_register'],scalar_plan['constant_component']),
                    registers[scalar_plan['temporary_register']][scalar_plan['temporary_component']])
            elif scalar_op in (2,3,5,19):
                source=c&255;swizzle=b&255;temporary=bool(c&(1<<29))
                if temporary and source>=64:raise RawShaderError('modified scalar temporary unsupported')
                def scalar_source(component):
                    value=registers[source][component] if temporary else node('constant',source,component)
                    return node('negate',value) if b&(1<<24) else value
                left=scalar_source(((swizzle>>6)+3)&3)
                if scalar_op==2:scalar=node('mul',left,scalar_source(swizzle&3))
                elif scalar_op==19:scalar=node('reciprocal',left)
                elif scalar_op==3:
                    if previous_scalar is None:raise RawShaderError('unknown scalar MUL_PREV input')
                    scalar=node('mul',left,previous_scalar)
                else:
                    right=scalar_source(swizzle&3)
                    if left!=right:raise RawShaderError('scalar MAX must copy identical operands')
                    scalar=node('regular_copy',left)
            if scalar_mask and scalar is None:raise RawShaderError('unknown previous-scalar dependency')
            operands=[]
            if vector_mask:
                for index in range(3 if op in (11,12,17) else 2):
                    shift=16-index*8;source=(c>>shift)&255;swizzle=(b>>shift)&255
                    temporary=bool(c&(1<<(31-index)))
                    if temporary and source>=64:raise RawShaderError('relative/absolute pixel temporary unsupported')
                    values=[registers[source][((swizzle>>(lane*2))+lane)&3] if temporary else
                        node('constant',source,((swizzle>>(lane*2))+lane)&3) for lane in range(4)]
                    operands.append([node('negate',value) for value in values] if b&(1<<(26-index)) else values)
            dot=None
            if vector_mask and op in (15,16,17):
                for lane in range(4 if op==15 else 3 if op==16 else 2):
                    product=node('mul',operands[0][lane],operands[1][lane])
                    dot=product if dot is None else node('add',dot,product)
                if op==17:dot=node('add',dot,operands[2][0])
            for lane in range(4):
                if vector_mask&(1<<lane):
                    left,right=operands[0][lane],operands[1][lane]
                    if op==2 and left!=right:raise RawShaderError('pixel MAX must copy identical operands')
                    if op==2:result=left
                    elif op in (15,16,17):result=dot
                    elif op==12:result=node('select_eq0',left,right,operands[2][lane])
                    elif op==11:result=node('add',node('mul',left,right),operands[2][lane])
                    elif op==7:result=node('not_equal',left,right)
                    else:result=node('add' if op==0 else 'mul',left,right)
                    if a&(1<<24):result=node('saturate',result)
                    target[lane]=result
                if zero&(1<<lane):target[lane]=node('literal',0)
                if one&(1<<lane):target[lane]=node('literal',1)
                if export and scalar_mask&(1<<lane):target[lane]=scalar
            if export:outputs[vd]=target
            else:
                scalar_target=target if sd==vd else registers[sd].copy()
                for lane in range(4):
                    if scalar_mask&(1<<lane):scalar_target[lane]=scalar
                registers[vd]=target
                registers[sd]=scalar_target
            previous_scalar=scalar
    if not outputs:raise RawShaderError('no pixel color exports')
    return {'sha256':hashlib.sha256(data).hexdigest(),'analysis':'experimental-pixel-explicit-sample-dataflow',
        'nodes':nodes,'exports':{str(k):v for k,v in sorted(outputs.items())},'fetches':fetches,
        'allocations':allocations,'executed_instruction_addresses':executed,
        'constant_registers':sorted({n[1] for n in nodes if n[0]=='constant'}),
        'previous_scalar':previous_scalar,'allocation_extent_verified':False,'sampling_executed':False,
        'interpolation_verified':False,'hardware_float_edge_cases_validated':False,
        'native_fragment_pipeline':False,'gpu_executed':False}


def evaluate_pixel_sample_dataflow(data,initial,constants,samples, *, ui_operations=False, extended_math=False):
    proof=analyze_pixel_sample_dataflow(data,ui_operations=ui_operations,extended_math=extended_math);nodes=proof['nodes'];values={}
    def vector_component(vector,lane):
        if not isinstance(vector,(list,tuple)) or len(vector)!=4 or vector[lane] is None:
            raise RawShaderError('missing explicit pixel vector component')
        value=vector[lane]
        if not math.isfinite(value):raise RawShaderError('non-finite pixel input')
        # Require actual float32 values rather than silently using double precision.
        try:rounded=struct.unpack('>f',struct.pack('>f',value))[0]
        except (OverflowError,struct.error) as error:raise RawShaderError('invalid pixel float32 input') from error
        if rounded!=value:raise RawShaderError('pixel inputs must already be float32')
        return value
    def resolve(index):
        if index in values:return values[index]
        n=nodes[index];op=n[0]
        if op=='initial':value=vector_component(initial.get(n[1]),n[2])
        elif op=='sample':value=vector_component(samples.get(n[1]),n[2])
        elif op=='literal':value=float(n[1])
        elif op=='constant':
            packed=constants.get(n[1])
            if not isinstance(packed,bytes) or len(packed)!=16:raise RawShaderError('missing exact BE pixel constant')
            value=struct.unpack_from('>f',packed,n[2]*4)[0]
            if not math.isfinite(value):raise RawShaderError('non-finite pixel constant')
        elif op in ('mul','add'):value=arithmetic(op,resolve(n[1]),resolve(n[2]))
        elif op=='not_equal':value=float(resolve(n[1])!=resolve(n[2]))
        elif op=='negate':value=-resolve(n[1])
        elif op=='reciprocal':
            operand=resolve(n[1])
            if not math.isfinite(operand) or abs(operand)<2**-126:
                raise RawShaderError('reciprocal evaluator requires finite nonzero normal input')
            try:value=struct.unpack('>f',struct.pack('>f',1.0/operand))[0]
            except (OverflowError,struct.error) as error:
                raise RawShaderError('reciprocal result out of finite float32 range') from error
            if not math.isfinite(value) or abs(value)<2**-126:
                raise RawShaderError('reciprocal non-finite/subnormal result unsupported')
        elif op=='saturate':value=min(1.0,max(0.0,resolve(n[1])))
        elif op in ('regular_copy','select_eq0'):
            operands=[resolve(j) for j in n[1:]]
            if any(not math.isfinite(v) or (v!=0 and abs(v)<2**-126) for v in operands):
                raise RawShaderError('pixel copy/conditional requires normal-or-zero sources')
            value=operands[0] if op=='regular_copy' else operands[1] if operands[0]==0 else operands[2]
        else:raise RawShaderError('unknown pixel expression')
        values[index]=value;return value
    for i,fetch in enumerate(proof['fetches']):
        for coord in fetch['coordinates']:resolve(coord)
        for lane in range(4):vector_component(samples.get(i),lane)
    for i,n in enumerate(nodes):
        if n[0] in ('mul','add','regular_copy','select_eq0','reciprocal'):resolve(i)  # Even overwritten scalar results must validate.
    return {slot:[None if n is None else resolve(n) for n in lanes] for slot,lanes in proof['exports'].items()}


R8_PLANE_IMAGE_BYTES=1382400


def _translate_pixel_compute(data,texture32,fragment=False,vertex_tint=False,*,r8_planes=False):
    proof=analyze_pixel_sample_dataflow(data);nodes=proof['nodes'];constants=proof['constant_registers']
    sample_count=len(proof['fetches']);record_words=8+sample_count*4
    total=(4112 if texture32 else record_words*16)+16*len(constants)
    if r8_planes:
        if texture32 or (vertex_tint and not fragment):raise RawShaderError('R8 plane probe does not mix other image/vertex profiles')
        total=R8_PLANE_IMAGE_BYTES+(16 if fragment else 128)+16*len(constants)
    if (not 1<=sample_count<=(1 if texture32 else 3) or set(proof['exports'])!={'0'} or None in proof['exports']['0'] or
            total>(R8_PLANE_IMAGE_BYTES+(96 if fragment else 208) if r8_planes else 4176 if texture32 else 4096)):
        raise RawShaderError('only one bounded sample and complete color0 supported by compute probe')
    if texture32 and [nodes[n] for n in proof['fetches'][0]['coordinates']]!=[('initial',0,0),('initial',0,1)]:
        raise RawShaderError('texture32 probe requires unchanged initial r0.xy coordinates')
    plane_coordinates=[]
    if r8_planes:
        if [f['slot'] for f in proof['fetches']]!=[0,1,2]:
            raise RawShaderError('three R8 fetches must select slots0,1,2 in order')
        for fetch in proof['fetches']:
            coordinates=[nodes[n] for n in fetch['coordinates']]
            if coordinates not in ([('initial',0,0),('initial',0,1)],[('initial',0,2),('initial',0,3)]):
                raise RawShaderError('R8 probe requires explicit unchanged r0.xy or r0.zw')
            plane_coordinates.append([n[2] for n in coordinates])
    statements=[];expressions={}
    def expression(index):
        if index in expressions:return expressions[index]
        n=nodes[index];op=n[0];name=f'n{index}'
        if op=='initial':
            if n[1]>=2:raise RawShaderError('compute probe supplies only explicit initial r0/r1')
            if r8_planes:
                value=(f'coordinate[{n[2]}]' if n[1]==0 else f'vertexTint[{n[2]}]' if vertex_tint else f'uintBitsToFloat(readBE({R8_PLANE_IMAGE_BYTES//4+n[2]}u))') if fragment else f'uintBitsToFloat(readBE({R8_PLANE_IMAGE_BYTES//4}u+index*8u+{n[1]*4+n[2]}u))'
            elif texture32:
                if n[1]==0:
                    if n[2]>=2:raise RawShaderError('texture32 probe has no initial r0.zw')
                    value=f'coordinate[{n[2]}]'
                else:value=f'vertexTint[{n[2]}]' if vertex_tint else f'uintBitsToFloat(readBE({1024+n[2]}u))'
            else:value=f'uintBitsToFloat(readBE(index*{record_words}u+{n[1]*4+n[2]}u))'
        elif op=='sample':value=(f'sampled{n[1]}[{n[2]}]' if r8_planes else
            f'sampled[{n[2]}]' if texture32 else f'uintBitsToFloat(readBE(index*{record_words}u+{8+n[1]*4+n[2]}u))')
        elif op=='constant':value=f'uintBitsToFloat(readBE({(R8_PLANE_IMAGE_BYTES//4+(4 if fragment else 32) if r8_planes else 1028 if texture32 else record_words*4)+constants.index(n[1])*4+n[2]}u))'
        elif op=='literal':value=f'{n[1]}.0'
        elif op in ('mul','add'):
            left,right=expression(n[1]),expression(n[2])
            statements.append(f'if (!regular({left}) || !regular({right})) return;')
            value=f'(({left}==0.0 || {right}==0.0)?0.0:({left}*{right}))' if op=='mul' else f'({left}+{right})'
        elif op in ('regular_copy','select_eq0'):
            args=[expression(j) for j in n[1:]]
            statements.append(f'if ({" || ".join("!regular("+arg+")" for arg in args)}) return;')
            value=args[0] if op=='regular_copy' else f'({args[0]}==0.0?{args[1]}:{args[2]})'
        else:raise RawShaderError('unsupported pixel compute expression')
        statements.append(f'precise float {name}={value};')
        statements.append(f'if ({"!regular("+name+")" if op in ("mul","add","regular_copy","select_eq0") else "isnan("+name+") || isinf("+name+")"}) return;')
        expressions[index]=name;return name
    for fetch in proof['fetches']:
        for coordinate in fetch['coordinates']:expression(coordinate)
    if r8_planes:
        for i in range(3):statements.append(f'if(any(isnan(sampled{i})) || any(isinf(sampled{i}))) return;')
    elif texture32:statements.append('if(any(isnan(sampled)) || any(isinf(sampled))) return;')
    else:statements.append(f'for(uint lane=0u;lane<{sample_count*4}u;++lane) {{ float v=uintBitsToFloat(readBE(index*{record_words}u+8u+lane)); if(isnan(v)||isinf(v)) return; }}')
    for i,n in enumerate(nodes):
        if n[0] in ('mul','add','regular_copy','select_eq0'):expression(i)
    colors=[expression(n) for n in proof['exports']['0']]
    stores=([f'color0=vec4({",".join(colors)});'] if fragment else
        [f'result.words[1u+index][{lane}]=floatBitsToUint({value});' for lane,value in enumerate(colors)])
    if r8_planes and fragment:
        # Keep bounds and texture-coordinate rejection early. Arithmetic is only
        # finite float operations/loads, with no stores until the final discard.
        # One accumulated decision avoids the observed many-discard path failure
        # in the emulator fragment backend without weakening any predicate.
        statements=[f'valid = valid && !({s[4:-9]});' if s.startswith('if (') and s.endswith(') return;')
            else s for s in statements]
        stores.insert(0,'if (!valid) discard;')
    native_sampling=''
    if r8_planes:
        for i,coordinates in enumerate(plane_coordinates):
            components=[f'coordinate[{lane}]' if fragment else f'uintBitsToFloat(readBE({R8_PLANE_IMAGE_BYTES//4}u+index*8u+{lane}u))' for lane in coordinates]
            native_sampling+=(f'    vec2 coordinate{i}=vec2({",".join(components)});\n'
                f'    if(any(isnan(coordinate{i})) || any(isinf(coordinate{i}))) return;\n'
                f'    vec4 sampled{i}=textureLod(imageSource{i},coordinate{i},0.0);\n')
    source='''#version 450
'''+(('layout(location=1) in vec4 vertexTint;\n' if vertex_tint else '')+
    f'layout(location=0) in vec{4 if r8_planes else 2} coordinate;\nlayout(location=0) out vec4 color0;\n' if fragment else
    'layout(local_size_x=1) in;\n')+'''
layout(set=0,binding=0,std430) readonly buffer Source { uint words[]; } source;
'''+('' if fragment else 'layout(set=0,binding=1,std430) buffer Result { uvec4 words[]; } result;\n')+(
    'layout(set=0,binding=2) uniform sampler2D imageSource;\n' if texture32 else
    ''.join(f'layout(set=0,binding={i+2}) uniform sampler2D imageSource{i};\n' for i in range(3)) if r8_planes else '')+'''
uint readBE(uint at) { uint v=source.words[at]; return (v<<24)|((v&0xff00u)<<8)|((v>>8)&0xff00u)|(v>>24); }
bool regular(float v) { uint bits=floatBitsToUint(v)&0x7fffffffu; return bits==0u || (bits>=0x00800000u && bits<0x7f800000u); }
void main() {
'''+('    bool valid=true;\n' if r8_planes and fragment else '')+(
    f'    if(source.words.length()<{total//4}) discard;\n' if fragment else
    f'    uint index=gl_GlobalInvocationID.x;\n    if(index>={1024 if texture32 else 4}u || result.words.length()<{1026 if texture32 else 6} || source.words.length()<{total//4}) return;\n')+(
    (native_sampling.replace('return;','discard;') if r8_planes else '    vec4 sampled=textureLod(imageSource,coordinate,0.0);\n') if fragment else (
    '    vec2 coordinate=(vec2(index%32u,index/32u)+vec2(0.5))/32.0;\n'
    '    vec4 sampled=textureLod(imageSource,coordinate,0.0);\n' if texture32 else native_sampling))+''.join(
        '    '+(s.replace('return;','discard;') if fragment else s)+'\n' for s in statements+stores)+'}\n'
    if r8_planes:
        return source,dict(proof,probe_stage='fragment' if fragment else 'compute',input_bytes=total,
            output_bytes=16416 if fragment else 96,maximum_invocations=None if fragment else 4,
            record_bytes=16 if fragment else 32,image_bytes=R8_PLANE_IMAGE_BYTES,image_count=3,
            native_formats=['R8_UNORM']*3,native_extents=[[1280,720],[640,360],[640,360]],
            native_image_swizzle=['R','R','R','1'],initial_coordinate_components=plane_coordinates,
            sample_result_override=False,native_texture_sampling=True,texture_coordinates_used_for_sampling=True,
            computed_lod_overridden=True,native_explicit_lod=0,xenos_sampler_verified=False,
            native_fragment_pipeline=False,synthetic_vertex_partner=fragment and not vertex_tint,
            vertex_tint_input=vertex_tint,
            deferred_arithmetic_guards=fragment,
            title_linkage_verified=False,guest_writeback=False,frame_presented=False)
    if fragment:
        return source,dict(proof,probe_stage='fragment',input_bytes=total,output_bytes=16416,
            sample_result_override=False,native_texture_sampling=True,texture_coordinates_used_for_sampling=True,
            synthetic_vertex_partner=not vertex_tint,vertex_tint_input=vertex_tint,
            title_linkage_verified=False,computed_lod_overridden=True,native_explicit_lod=0,
            xenos_sampler_verified=False,guest_writeback=False,frame_presented=False)
    if texture32:
        return source,dict(proof,probe_stage='compute',input_bytes=total,output_bytes=16416,maximum_invocations=1024,
            sample_result_override=False,native_texture_sampling=True,texture_coordinates_used_for_sampling=True,
            initial_r0_xy_override='32x32-texel-centers',computed_lod_overridden=True,native_explicit_lod=0,
            xenos_sampler_verified=False,guest_writeback=False,frame_presented=False)
    return source,dict(proof,probe_stage='compute',input_bytes=total,output_bytes=96,maximum_invocations=4,
        sample_count=sample_count,record_bytes=record_words*4,
        sample_result_override=True,texture_coordinates_used_for_sampling=False,guest_writeback=False,frame_presented=False)


def translate_pixel_sample_compute(data):
    """Four BE r0/r1/sample vec4 records then constants; samples NOT fetched."""
    return _translate_pixel_compute(data,False)


def translate_r8_planes_pixel_compute(data):
    """Tight Y/V/U R8 images, four BE r0/r1 records, sorted constants. Native
    linear/repeat/LOD0 is an explicit test choice, not recovered Xenos sampling.
    """
    return _translate_pixel_compute(data,False,r8_planes=True)


def translate_r8_planes_pixel_fragment(data,*,vertex_tint=False):
    """Native fragment probe with explicit vec4 UV input and either a tint
    header or vertex varying. Neither linkage nor raster is recovered title state.
    """
    return _translate_pixel_compute(data,False,fragment=True,vertex_tint=vertex_tint,r8_planes=True)


def translate_texture32_pixel_compute(data):
    """32x32 RGBA8 image + BE tint/constant vectors, native center sampling.

    This compute experiment overrides interpolation and computed LOD. It is not
    a fragment stage or Xenos sampler implementation and never retires a draw.
    """
    return _translate_pixel_compute(data,True)


def translate_texture32_pixel_fragment(data,*,vertex_tint=False):
    """Derived fragment stage for an explicit synthetic native raster fixture.

    The vertex partner, viewport, attachment, interpolation and LOD are test
    choices, not recovered title state. Invalid math discards the fragment.
    """
    return _translate_pixel_compute(data,True,True,vertex_tint)
