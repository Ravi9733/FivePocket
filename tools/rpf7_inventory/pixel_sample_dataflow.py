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
from loading_shader_math import loading_scalar_node, evaluate_loading_operation, LOADING_OPERATIONS
from cube_shader_math import cube_result_nodes, evaluate_cube_operation, CUBE_OPERATIONS


def texture_fetch_metadata(words,registers,address,*,cube_profile=False,explicit_lod_profile=False,register_lod=None,geometry_profile=False,composite_profile=False,texel_offset_profile=False,tonemap_profile=False,filter_weights_profile=False):
    a,b,c=words
    operation='texture_sample';point=False;weights=False
    if filter_weights_profile:
        if cube_profile or explicit_lod_profile or geometry_profile or composite_profile or tonemap_profile:
            raise RawShaderError('filter weights require separate normalized 2D metadata')
        # Xenia ucode TextureFetchInstruction: opcode19 returns interpolation
        # weights, NOT texture colours. Keep that distinction in the proof.
        # Only the observed LINEAR weights / POINT colour instruction family,
        # inherited mip mode, computed LOD and signed XY offsets are admitted.
        weights=a&31==19;point=a&31==1
        expected=0x1F1F5000 if weights else 0x1F1F0000
        if not (weights or point) or b&~4095!=expected:
            raise RawShaderError('unsupported filter-weight opcode or instruction filters')
        operation='texture_weights' if weights else 'texture_sample'
        a=(a&~31)|1;b|=0xF000;texel_offset_profile=True
    variable_a=(63<<5)|(63<<12)|(31<<20)|(63<<26)
    # Xenos word0 bit25 is TX_COORD_DENORM: the instruction addresses the
    # texture in texels rather than normalized [0,1]. Word1 bits 12-15 are the
    # mag/min filter fields, where 3 means "use the fetch constant" and 0 means
    # an explicit POINT filter. A full-screen composite reads its inputs one
    # texel per pixel, so it sets both. Admit EXACTLY those five bits and
    # nothing else; every other addressing/filter field stays as strict as it
    # was. The denormalized coordinate is converted by dividing by the texture
    # size in the generated shader, so the fetch constant's own clamp/repeat
    # policy still applies and no out-of-range policy is invented here.
    composite=False
    if composite_profile and (a>>25)&1 and c==0x4000 and b&~4095==0x1F1F0000:
        composite=True;a&=~(1<<25);b|=0xF000
    # Xenos word0 bit19 selects valid-only versus fetch-all invocations, not
    # addressing/filtering/LOD. The explicit straight-line geometry profile
    # emits unconditional implicit-LOD sampling, including native helper lanes.
    # Keep the original bit as evidence; this is not hardware quad conformance.
    if geometry_profile:variable_a|=1<<19
    # The tonemap pass issues ONE of its seven fetches with bit19 clear. The
    # generated shader samples unconditionally with implicit LOD in every
    # invocation, so valid-only and fetch-all produce the same GLSL either way;
    # the bit is reported below rather than dropped. Same reasoning, and the
    # same non-claim about hardware quad conformance, as the geometry profile.
    if tonemap_profile:variable_a|=1<<19
    cube=cube_profile and c==0xC000 and b&~4095==0x0F1FF000
    explicit=explicit_lod_profile and c==0x4000 and b&~4095==0x2F1FF000
    if explicit and register_lod is None:raise RawShaderError('register LOD fetch without a preceding setTexLOD')
    # Word2 bits 16-20 and 21-25 are OFFSET_X and OFFSET_Y, a constant texel
    # offset added to the sampled coordinate, in signed HALF-texel units.
    # Xenia ucode.h TextureFetchInstruction::offset_x/offset_y, pinned
    # 95a5c3ee250f80c3b9d139658649d9ffb6db3eec; see docs/licenses/XENIA.txt.
    # Admit ONLY those ten bits, and
    # only under the explicit profile; the offset is reported here and applied
    # by the generated shader, never silently dropped. Signed 5-bit values.
    offset_x=offset_y=0
    if texel_offset_profile and not cube and not explicit:
        offset_x=(c>>16)&31;offset_y=(c>>21)&31
        offset_x-=32 if offset_x&16 else 0;offset_y-=32 if offset_y&16 else 0
        offset_x*=0.5;offset_y*=0.5
        c&=~0x03FF0000
    if a&~variable_a != (1 if geometry_profile or tonemap_profile else 1|(1<<19)) or (not cube and not explicit and (b&~4095!=0x1F1FF000 or c!=0x4000)):
        raise RawShaderError('only unmodified valid-only normalized 2D inherited-filter fetch metadata supported')
    swizzle=[(b>>(i*3))&7 for i in range(4)]
    if 6 in swizzle:raise RawShaderError('reserved texture destination swizzle')
    source=(a>>5)&63
    return {'instruction_address':address,'slot':(a>>20)&31,'destination':(a>>12)&63,
        **({'operation':operation,'explicit_point_filter':point,
            'explicit_linear_filter':weights,'weight_lod_verified':False} if filter_weights_profile else {}),
        **({'denormalized_coordinates':True,'explicit_point_filter':True} if composite else {}),
        **({'texel_offset':(offset_x,offset_y)} if (offset_x or offset_y) else {}),
        'destination_swizzle':swizzle,'coordinates':[registers[source][(a>>shift)&3] for shift in ((26,28,30) if cube else (26,28))],
        'computed_lod_requested':not (cube or explicit),'sampling_executed':False,
        'fetch_valid_only':bool(a&(1<<19)),
        **({'lod_expression':register_lod,'register_lod_requested':True} if explicit else {}),
        **({'dimension':'cube','explicit_base_lod':True} if cube else {})}


def texture_gradient_metadata(words,registers,address):
    """Canonical unpredicated getGradients; no texture resource is read.

    Pinned ucode/DXBC reference maps XZ=ddx(src.xy), YW=ddy(src.xy).
    Coarse selection is an explicit native policy, not Xbox hardware proof.
    Admit only the captured modifier family; do not ignore arbitrary fields
    merely because the reference backend does not consult a texture constant.
    """
    a,b,c=words
    variable=(63<<5)|(63<<12)|(15<<26)
    if a&~variable != (18|(1<<19)) or b&~4095!=0x1F1FF000 or c:
        raise RawShaderError('unsupported getGradients metadata or addressing')
    swizzle=[(b>>(i*3))&7 for i in range(4)]
    if 6 in swizzle:raise RawShaderError('reserved gradient destination swizzle')
    source=(a>>5)&63
    return dict(instruction_address=address,destination=(a>>12)&63,
        destination_swizzle=swizzle,coordinates=[registers[source][(a>>s)&3] for s in (26,28)],
        policy='coarse-top-row-left-column',texture_resource_required=False,
        hardware_quad_verified=False)


def pixel_supported_opcodes(*, ui_operations=False, extended_math=False, loading_math=False, cube_profile=False, predicate_profile=False, geometry_profile=False, geometry_predicate_profile=False, geometry_fixed16_effects_profile=False, composite_profile=False, rectangle_mrt_profile=False, tonemap_profile=False, post_stencil_profile=False, geometry_lod_profile=False, filter_weights_profile=False, bright_pass_profile=False):
    """Shared encoding inventory; membership alone is not dataflow admission."""
    vector = (0,1,2,7,11,12,15,16,17) if extended_math else (0,1,2,7,11,12,15,16) if ui_operations else (0,1,2,12,15,16)
    scalar = (2,3,5,19,42,43,44,45,46,47,50) if extended_math else (2,3,5,42,43,50) if ui_operations else (3,5,42,43,50)
    if loading_math:
        scalar = (*scalar,0,1,12,14,22,25,40)
        vector = (*vector,5,6,14)
        if cube_profile:
            vector = (*vector,18)
        if predicate_profile:
            scalar = (*scalar,28)
        if geometry_profile:
            scalar = (*scalar,8,10,11,16)
            if geometry_predicate_profile:
                scalar = (*scalar,28)
                vector = (*vector,25)
                if geometry_fixed16_effects_profile:scalar=(*scalar,30)
        # FRACs only. The composite profile needs the geometry scalar set's
        # fraction operation but not its packed-geometry vectors or its indexed
        # topology, so it takes that one opcode and nothing else.
        if composite_profile:scalar=(*scalar,11)
        # MINv/MINs are the exact mirrors of the MAXv/MAXs already admitted, and
        # SETEv the mirror of SETNEv. No transcendental or addressing op is added.
        if rectangle_mrt_profile:scalar=(*scalar,6);vector=(*vector,3,4)
        # The tonemap pass reduces the luminance pyramid and reshapes exposure:
        # FRACs and LOG join the SIN/COS pair because the captured shader uses
        # all four in the same expression. No vector opcode is added.
        if tonemap_profile:scalar=(*scalar,11,16,48,49)
        # Separate post-stencil colour arithmetic: FRACv and LOG_CLAMPs.
        # No native renderer/topology/state admission follows from this list.
        if post_stencil_profile:vector=(*vector,8);scalar=(*scalar,16)
        # Separate indexed sampling family: reuse already implemented MIN/FRAC
        # arithmetic, without importing rectangle or post-stencil admission.
        if geometry_lod_profile:vector=(*vector,3,8)
        # FRCv8 / FLOORv10 and FLOORs13 from Xenia ucode.h. Unary, no texture
        # reads or hardware numeric-equivalence claim follows from ALU support.
        if filter_weights_profile:vector=(*vector,8,10);scalar=(*scalar,13)
        # SETGTs8 only (Xenia ucode.h): scalar = src>0 ? 1 : 0, as already
        # admitted by the vertex scalar_gt_zero profile. No vector opcode is added.
        if bright_pass_profile:scalar=(*scalar,8)
    return vector, scalar


def pixel_instruction_budget(*, predicate_profile=False):
    # Recovered retail refMipBlur_5 executes 283 straight-line instructions.
    # Only the explicit predicate profile gets this bounded extension; default
    # profiles, byte extent, expression, fetch and control limits stay strict.
    return 320 if predicate_profile else 256


def analyze_pixel_sample_dataflow(data, *, ui_operations=False, extended_math=False, loading_math=False,cube_profile=False,explicit_lod_profile=False,predicate_profile=False,geometry_profile=False,geometry_predicate_profile=False,geometry_gradient_depth_profile=False,geometry_fixed16_effects_profile=False,rectangle_depth_profile=False,composite_profile=False,rectangle_mrt_profile=False,tonemap_profile=False,post_stencil_profile=False,geometry_lod_profile=False,filter_weights_profile=False,bright_pass_profile=False):
    # Separate straight-line rectangle bright-pass arithmetic: the loading set
    # plus SETGTs. Mutually exclusive with every other rectangle/geometry profile.
    if type(bright_pass_profile) is not bool or (bright_pass_profile and
            (not loading_math or cube_profile or explicit_lod_profile or predicate_profile or
             geometry_profile or geometry_predicate_profile or geometry_gradient_depth_profile or
             geometry_fixed16_effects_profile or rectangle_depth_profile or composite_profile or
             rectangle_mrt_profile or tonemap_profile or post_stencil_profile or geometry_lod_profile or
             filter_weights_profile)):
        raise RawShaderError('bright pass requires its separate straight-line loading profile')
    if type(filter_weights_profile) is not bool or (filter_weights_profile and
            (not loading_math or cube_profile or explicit_lod_profile or predicate_profile or
             geometry_profile or geometry_predicate_profile or geometry_gradient_depth_profile or
             geometry_fixed16_effects_profile or rectangle_depth_profile or composite_profile or
             rectangle_mrt_profile or tonemap_profile or post_stencil_profile or geometry_lod_profile)):
        raise RawShaderError('filter weights require separate straight-line loading analysis')
    if type(geometry_lod_profile) is not bool or (geometry_lod_profile and
            (not geometry_profile or not loading_math or cube_profile or explicit_lod_profile or
             predicate_profile or geometry_predicate_profile or geometry_gradient_depth_profile or
             geometry_fixed16_effects_profile or rectangle_depth_profile or composite_profile or
             rectangle_mrt_profile or tonemap_profile or post_stencil_profile)):
        raise RawShaderError('indexed LOD requires its separate unpredicated pixel geometry profile')
    if type(post_stencil_profile) is not bool or (post_stencil_profile and
            (not loading_math or cube_profile or explicit_lod_profile or predicate_profile or
             geometry_profile or geometry_predicate_profile or geometry_gradient_depth_profile or
             geometry_fixed16_effects_profile or rectangle_depth_profile or composite_profile or
             rectangle_mrt_profile or tonemap_profile)):
        raise RawShaderError('post-stencil arithmetic requires its separate straight-line loading profile')
    # The tonemap pass is the offset sample band's rectangle shape with richer
    # scalar arithmetic. It rides on that band's texel-offset profile and is
    # mutually exclusive with every indexed geometry profile, exactly as the
    # MRT profile below is.
    if type(tonemap_profile) is not bool or (tonemap_profile and
            (not loading_math or not rectangle_mrt_profile or geometry_profile or
             geometry_gradient_depth_profile or geometry_fixed16_effects_profile or
             geometry_predicate_profile or predicate_profile or cube_profile or
             explicit_lod_profile or composite_profile or rectangle_depth_profile)):
        raise RawShaderError('tonemap requires the separate rectangle texel-offset loading profile')
    # A nonindexed rectangle pass writing TWO colour targets with constant texel
    # offsets. It uses the straight-line loading arithmetic and none of the
    # indexed geometry machinery, so it is mutually exclusive with all of it.
    if type(rectangle_mrt_profile) is not bool or (rectangle_mrt_profile and
            (not loading_math or geometry_profile or geometry_gradient_depth_profile or
             geometry_fixed16_effects_profile or geometry_predicate_profile or predicate_profile or
             cube_profile or explicit_lod_profile or composite_profile or rectangle_depth_profile)):
        raise RawShaderError('rectangle MRT requires separate straight-line loading arithmetic')
    if type(rectangle_depth_profile) is not bool or (rectangle_depth_profile and
            (not loading_math or geometry_profile or geometry_gradient_depth_profile or geometry_fixed16_effects_profile or
             geometry_predicate_profile or predicate_profile or cube_profile or explicit_lod_profile)):
        raise RawShaderError('rectangle depth requires separate straight-line loading arithmetic')
    if type(geometry_fixed16_effects_profile) is not bool or (geometry_fixed16_effects_profile and
            (not geometry_gradient_depth_profile or not geometry_predicate_profile)):
        raise RawShaderError('fixed16 pixel effects require explicit gradient/depth and predicate profiles')
    if type(geometry_gradient_depth_profile) is not bool or (geometry_gradient_depth_profile and
            (not geometry_profile or (geometry_predicate_profile and not geometry_fixed16_effects_profile))):
        raise RawShaderError('gradient/depth exports require separate unpredicated geometry profile')
    if type(composite_profile) is not bool or (composite_profile and
            (not loading_math or cube_profile or geometry_profile)):
        raise RawShaderError('composite sampling requires the separate straight-line 2D loading profile')
    if geometry_predicate_profile and not geometry_profile:
        raise RawShaderError('geometry pixel predication requires explicit geometry profile')
    if geometry_profile and (not loading_math or cube_profile or predicate_profile or explicit_lod_profile):
        raise RawShaderError('geometry arithmetic requires the separate straight-line 2D loading profile')
    if predicate_profile and (not explicit_lod_profile or cube_profile):
        raise RawShaderError('predication requires the separate explicit-LOD pixel profile')
    if explicit_lod_profile and not loading_math:
        raise RawShaderError('explicit register LOD requires the loading profile')
    if cube_profile and not loading_math:
        raise RawShaderError('cube profile requires explicit loading arithmetic')
    if loading_math and not extended_math:
        raise RawShaderError('loading math requires explicit extended arithmetic profile')
    if extended_math and not ui_operations:
        raise RawShaderError('extended math requires explicit UI arithmetic profile')
    instruction_predicate=predicate_profile or geometry_predicate_profile
    words,controls=schedule(data);nodes=[];intern={}
    vector_ops,scalar_ops=pixel_supported_opcodes(composite_profile=composite_profile,rectangle_mrt_profile=rectangle_mrt_profile,tonemap_profile=tonemap_profile,post_stencil_profile=post_stencil_profile,ui_operations=ui_operations,
        extended_math=extended_math,loading_math=loading_math,cube_profile=cube_profile,predicate_profile=predicate_profile,
        geometry_profile=geometry_profile,geometry_predicate_profile=geometry_predicate_profile,
        geometry_fixed16_effects_profile=geometry_fixed16_effects_profile,geometry_lod_profile=geometry_lod_profile,
        filter_weights_profile=filter_weights_profile,bright_pass_profile=bright_pass_profile)
    validation_nodes=None;validation_groups=[];predicate=None
    def node(op,*args):
        key=(op,*args)
        if key not in intern:
            if len(nodes)==4096:raise RawShaderError('pixel expression budget exceeded')
            intern[key]=len(nodes);nodes.append(key)
        result=intern[key]
        if validation_nodes is not None and op not in ('initial','constant','literal','predicate_condition','predicate_select'):
            validation_nodes.append(result)
        return result
    registers=[[node('initial',r,c) for c in range(4)] for r in range(64)]
    outputs={};fetches=[];gradients=[];allocations=[];executed=[];previous_scalar=None;register_lod=None;lod_sets=[];kills=[]
    for control in controls:
        if control[0]=='alloc':
            if control[1]!=2 or allocations:raise RawShaderError('only one color allocation supported')
            allocations.append({'kind':2,'encoded_size':control[2],'unserialized':control[3]})
        if control[0]!='exec':continue
        for address in range(control[1],control[1]+control[2]):
            if len(executed)==pixel_instruction_budget(predicate_profile=predicate_profile):
                raise RawShaderError('pixel instruction budget exceeded')
            executed.append(address);a,b,c=words[address*3:address*3+3]
            if control[3]&(1<<((address-control[1])*2)):
                if a&31==18:
                    if not geometry_gradient_depth_profile:
                        raise RawShaderError('getGradients requires the explicit gradient/depth profile')
                    if len(gradients)==16:raise RawShaderError('gradient instruction budget exceeded')
                    gradient=texture_gradient_metadata((a,b,c),registers,address)
                    index=len(gradients);gradients.append(gradient)
                    target=registers[gradient['destination']].copy()
                    for lane,select in enumerate(gradient['destination_swizzle']):
                        if select<4:target[lane]=node('gradient',index,select)
                        elif select<6:target[lane]=node('literal',select-4)
                    registers[gradient['destination']]=target
                    continue
                if a&31==24:
                    # Canonical unpredicated setTexLOD, one replicated source
                    # component. It updates sampler state, NOT a texture slot or
                    # temporary register. Snapshot the pre-fetch expression.
                    component=(a>>26)&3
                    # setTexLOD has no destination (Xenia shader_translator.cc).
                    # The new captured indexed family encodes unused dst6.
                    unused_destination=(63<<12) if geometry_lod_profile else 0
                    if (not (explicit_lod_profile or geometry_lod_profile) or
                            a&~((63<<5)|(63<<26)|unused_destination)!=(24|(1<<19)) or
                            a>>26!=component*21 or b!=0x0F1FFFFF or c):
                        raise RawShaderError('unsupported setTexLOD profile or modifiers')
                    register_lod=node('regular_copy',registers[(a>>5)&63][component])
                    lod_sets.append({'instruction_address':address,'expression':register_lod})
                    continue
                if len(fetches)==32:raise RawShaderError('pixel fetch budget exceeded')
                fetch=texture_fetch_metadata((a,b,c),registers,address,cube_profile=cube_profile,
                    explicit_lod_profile=explicit_lod_profile or geometry_lod_profile,register_lod=register_lod,
                    geometry_profile=geometry_profile or rectangle_depth_profile,
                    composite_profile=composite_profile,tonemap_profile=tonemap_profile,
                    texel_offset_profile=rectangle_mrt_profile or geometry_lod_profile,
                    filter_weights_profile=filter_weights_profile);index=len(fetches);fetches.append(fetch)
                candidate=registers[fetch['destination']].copy()
                for lane,select in enumerate(fetch['destination_swizzle']):
                    if select<4:candidate[lane]=node('sample',index,select)
                    elif select<6:candidate[lane]=node('literal',select-4)
                registers[fetch['destination']]=candidate
                continue
            op=(c>>24)&31;scalar_op=a>>26;export=bool(a&(1<<15));vd=a&63;sd=(a>>8)&63
            predicated=bool(b&(1<<28));guard=None
            if instruction_predicate:
                if not predicated and b&(1<<27):
                    raise RawShaderError('unused predicate condition outside bounded profile')
                if predicated:
                    if predicate is None:raise RawShaderError('unknown initial pixel predicate')
                    if scalar_op in (28,30):raise RawShaderError('predicated predicate mutation outside bounded profile')
                    guard=node('predicate_condition',predicate,bool(b&(1<<27)))
            if op not in vector_ops or scalar_op not in scalar_ops:
                raise RawShaderError('unsupported pixel vector/scalar operation')
            if a&((1<<6)|(1<<7)|((0 if loading_math else 2 if ui_operations else 3)<<24)) or (b&(0xE0000000 if instruction_predicate else 0xF8000000 if ui_operations else 0xFF000000)) or (not export and a&(1<<14)):
                raise RawShaderError('modified pixel ALU unsupported')
            validation_nodes=[] if instruction_predicate else None
            # Only instruction predicate bits are consumed here. Addressing,
            # arithmetic modifiers and scalar operand validation stay strict.
            alu_words=(a,b&~(3<<27) if instruction_predicate else b,c)
            vm,sm=(a>>16)&15,(a>>20)&15
            if export:
                depth=(geometry_gradient_depth_profile or rectangle_depth_profile) and vd==61
                if (vd>=4 and not depth) or not allocations:
                    raise RawShaderError('unallocated/non-color export unsupported')
                if depth and ((vm|sm)!=1 or a&(1<<14)):
                    raise RawShaderError('only explicit depth X export with unchanged other lanes supported')
                vector_mask,scalar_mask=vm&~sm,sm&~vm
                zero=15&~(vm|sm) if a&(1<<14) else 0;one=vm&sm
                target=outputs.get(vd,[None]*4).copy()
            else:
                if vd==sd and vm&sm:raise RawShaderError('overlapping scalar/vector temporary writes unsupported')
                vector_mask,scalar_mask,zero,one=vm,sm,0,0;target=registers[vd].copy()
            # Snapshot BOTH scalar and vector operands before publishing any register.
            scalar=previous_scalar
            if loading_math and scalar_op not in (*range(42,48),50):
                scalar=loading_scalar_node(alu_words,registers,node,previous_scalar,predicate_profile=instruction_predicate,
                    geometry_profile=geometry_profile or post_stencil_profile,predicate_ge_profile=geometry_fixed16_effects_profile,
                    composite_profile=composite_profile,tonemap_profile=tonemap_profile,
                    filter_weights_profile=filter_weights_profile,scalar_gt_zero=bright_pass_profile)
            elif extended_math and scalar_op in range(42,48):
                scalar=scalar_constant_node(decode_scalar_constant(alu_words,coissued=True,
                    allow_clamp=loading_math,allow_export=loading_math),registers,node)
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
            scalar_stored=node('saturate',scalar) if a&(1<<25) and scalar is not None else scalar
            operands=[]
            if vector_mask or op==25:
                # FRACv is unary (Xenia95a5c3ee ucode.h kFrc). Never invent
                # reads of an unused second operand or masked-off source lanes.
                for index in range(1 if op in (8,10) else 3 if op in (11,12,14,17) else 2):
                    shift=16-index*8;source=(c>>shift)&255;swizzle=(b>>shift)&255
                    temporary=bool(c&(1<<(31-index)))
                    if temporary and (source&64 if loading_math else source>=64):raise RawShaderError('relative/absolute pixel temporary unsupported')
                    # Operand modifiers execute only on consumed components.
                    # Eager abs/negate nodes on masked-off lanes would invent
                    # reads of unavailable interpolators / PsParamGen ZW.
                    consumed=(15 if op in (15,18,25) else 7 if op==16 else
                        (3 if index<2 else 1) if op==17 else vector_mask)
                    values=[]
                    for lane in range(4):
                        if not consumed&(1<<lane):values.append(None);continue
                        component=((swizzle>>(lane*2))+lane)&3
                        value=registers[source&63][component] if temporary else node('constant',source,component)
                        if temporary and source&128:value=node('absolute',value)
                        if b&(1<<(26-index)):value=node('negate',value)
                        values.append(value)
                    operands.append(values)
            # KILLGT has a pixel side effect even with NO destination writes.
            # Compare all four pre-coissue components after swizzle/modifiers.
            kill=node('kill_gt',*operands[0],*operands[1]) if op==25 else None
            if kill is not None:kills.append(dict(address=address,guard=guard,condition=kill))
            cube=cube_result_nodes(operands,node) if vector_mask and op==18 else None
            dot=None
            if vector_mask and op in (15,16,17):
                for lane in range(4 if op==15 else 3 if op==16 else 2):
                    product=node('mul',operands[0][lane],operands[1][lane])
                    dot=product if dot is None else node('add',dot,product)
                if op==17:dot=node('add',dot,operands[2][0])
            for lane in range(4):
                if vector_mask&(1<<lane):
                    left=operands[0][lane];right=operands[1][lane] if len(operands)>1 else None
                    if op==2 and left!=right and not loading_math:raise RawShaderError('pixel MAX must copy identical operands')
                    if op==25:result=kill
                    elif op==18:result=cube[lane]
                    elif op==2:result=node('maximum',left,right) if loading_math else left
                    elif op in (15,16,17):result=dot
                    elif op==12:result=node('select_eq0',left,right,operands[2][lane])
                    elif op==14:result=node('select_gt0',left,right,operands[2][lane])
                    elif op==5:result=node('greater',left,right)
                    elif op==6:result=node('greater_equal',left,right)
                    elif op==11:result=node('add',node('mul',left,right),operands[2][lane])
                    elif op==7:result=node('not_equal',left,right)
                    elif op==3:result=node('minimum',left,right)
                    elif op==4:result=node('equal',left,right)
                    elif op==8:result=node('fraction',left)
                    elif op==10:result=node('floor',left)
                    else:result=node('add' if op==0 else 'mul',left,right)
                    if a&(1<<24):result=node('saturate',result)
                    target[lane]=result
                if zero&(1<<lane):target[lane]=node('literal',0)
                if one&(1<<lane):target[lane]=node('literal',1)
                if export and scalar_mask&(1<<lane):target[lane]=scalar_stored
            def selected(new,old):
                return new if guard is None or new==old else node('predicate_select',guard,new,old)
            if export:outputs[vd]=[selected(new,old) for new,old in zip(target,outputs.get(vd,[None]*4))]
            else:
                scalar_target=target if sd==vd else registers[sd].copy()
                for lane in range(4):
                    if scalar_mask&(1<<lane):scalar_target[lane]=scalar_stored
                if sd==vd:
                    registers[vd]=[selected(new,old) for new,old in zip(scalar_target,registers[vd])]
                else:
                    registers[vd]=[selected(new,old) for new,old in zip(target,registers[vd])]
                    registers[sd]=[selected(new,old) for new,old in zip(scalar_target,registers[sd])]
            previous_scalar=selected(scalar,previous_scalar)
            if scalar_op in (28,30):
                # Both supported predicate setters encode p0 as the inverse
                # of their scalar result, even with no destination writes.
                predicate=node('predicate_condition',scalar,False)
            if instruction_predicate:
                validation_groups.append(dict(address=address,guard=guard,nodes=list(dict.fromkeys(validation_nodes)),
                    **({'kill':kill} if kill is not None else {})))
            validation_nodes=None
    if not outputs:raise RawShaderError('no pixel color exports')
    return {'sha256':hashlib.sha256(data).hexdigest(),'analysis':'experimental-pixel-explicit-sample-dataflow',
        'nodes':nodes,'exports':{str(k):v for k,v in sorted(outputs.items())},'fetches':fetches,
        'allocations':allocations,'executed_instruction_addresses':executed,
        'constant_registers':sorted({n[1] for n in nodes if n[0]=='constant'}),
        'previous_scalar':previous_scalar,'allocation_extent_verified':False,'sampling_executed':False,
        'interpolation_verified':False,'hardware_float_edge_cases_validated':False,
        'native_fragment_pipeline':False,'gpu_executed':False,
        'loading_math':loading_math,'cube_profile':cube_profile,'explicit_lod_profile':explicit_lod_profile,
        'predicate_profile':predicate_profile,'predicate':predicate,'validation_groups':validation_groups,
        'composite_profile':composite_profile,
        **({'post_stencil_profile':True} if post_stencil_profile else {}),
        **({'geometry_lod_profile':True} if geometry_lod_profile else {}),
        **({'filter_weights_profile':True,'weight_results_are_explicit_inputs':True} if filter_weights_profile else {}),
        **({'bright_pass_profile':True} if bright_pass_profile else {}),
        'geometry_profile':geometry_profile,'geometry_predicate_profile':geometry_predicate_profile,
        'geometry_gradient_depth_profile':geometry_gradient_depth_profile,'gradients':gradients,
        'geometry_fixed16_effects_profile':geometry_fixed16_effects_profile,
        'rectangle_depth_profile':rectangle_depth_profile,
        'kills':kills,'deferred_discard':bool(kills),'log2_nonpositive_supported':False,
        'lod_sets':lod_sets,'transcendental_hardware_approximation_verified':False}


def evaluate_pixel_sample_dataflow(data,initial,constants,samples, *, ui_operations=False, extended_math=False, loading_math=False,cube_profile=False,explicit_lod_profile=False,predicate_profile=False,geometry_profile=False,geometry_predicate_profile=False,geometry_gradient_depth_profile=False,geometry_fixed16_effects_profile=False,rectangle_depth_profile=False,quad_initial=None,quad_samples=None,quad_lane=0,include_queries=False,include_state=False,geometry_lod_profile=False):
    """Evaluate explicit sample inputs; optional queries report computed UV/LOD.

    Queries are arithmetic evidence only. They do not perform texture sampling
    or prove that any requested level exists or is initialized.
    """
    proof=analyze_pixel_sample_dataflow(data,ui_operations=ui_operations,extended_math=extended_math,loading_math=loading_math,
        cube_profile=cube_profile,explicit_lod_profile=explicit_lod_profile,predicate_profile=predicate_profile,
        geometry_profile=geometry_profile,geometry_predicate_profile=geometry_predicate_profile,
        geometry_gradient_depth_profile=geometry_gradient_depth_profile,
        geometry_fixed16_effects_profile=geometry_fixed16_effects_profile,rectangle_depth_profile=rectangle_depth_profile,
        geometry_lod_profile=geometry_lod_profile)
    return evaluate_analyzed_pixel_dataflow(proof,initial,constants,samples,include_queries=include_queries,include_state=include_state,
        quad_initial=quad_initial,quad_samples=quad_samples,quad_lane=quad_lane)


def evaluate_analyzed_pixel_dataflow(proof,initial,constants,samples,*,include_queries=False,include_state=False,quad_initial=None,quad_samples=None,quad_lane=0,screen_strip_ieee=False,world_strip_underflow=False,geometry_lod_ieee=False):
    """Reuse a locally analyzed immutable proof across a captured sample lattice.

    Offline arithmetic only; no native admission or texture sampling is implied.
    Gradients require four explicit lane inputs ordered TL,TR,BL,BR. The coarse
    policy takes top-row ddx/left-column ddy, never a fabricated single-lane zero.
    """
    if type(screen_strip_ieee) is not bool or (screen_strip_ieee and not proof.get('screen_strip_profile',False)):
        raise RawShaderError('zero reciprocal reference requires the explicit screen strip profile')
    if type(geometry_lod_ieee) is not bool or (geometry_lod_ieee and
            (not proof.get('geometry_lod_profile',False) or screen_strip_ieee or world_strip_underflow)):
        raise RawShaderError('zero reciprocal reference requires the explicit geometry LOD profile')
    zero_reciprocal_domain=screen_strip_ieee or geometry_lod_ieee
    if geometry_lod_ieee:
        from geometry_lod_shader_policy import (geometry_lod_infinite_product,
            geometry_lod_zero_log,geometry_lod_infinite_exp)
    if zero_reciprocal_domain:
        # Xenia ucode.h scalar19 is RECIP_IEEE, unlike17/18's finite clamps.
        # Reuse the existing exact signed-zero/SM3-zero-product helpers only.
        # This adds a CPU reference domain, NOT native draw admission or a
        # global nonfinite arithmetic policy. Current geometry LOD PS uses
        # reciprocal(+0) followed by 0*INF before a finite conditional result.
        from screen_strip_shader_policy import screen_strip_zero_reciprocal,screen_strip_zero_infinity_product
    if type(world_strip_underflow) is not bool or (world_strip_underflow and
            (not (proof.get('world_strip_profile',False) or proof.get('indexed_rgb7e3_profile',False)) or screen_strip_ieee)):
        raise RawShaderError('exponential underflow reference requires an explicit bounded perspective profile')
    if world_strip_underflow:
        from world_strip_shader_policy import world_strip_exp2
    if type(quad_lane) is not int or not 0<=quad_lane<4:
        raise RawShaderError('explicit quad lane must be 0..3')
    if quad_initial is not None and (not isinstance(quad_initial,(list,tuple)) or len(quad_initial)!=4 or
            any(not isinstance(v,dict) for v in quad_initial)):
        raise RawShaderError('four explicit quad input dictionaries required')
    if quad_samples is not None and (quad_initial is None or not isinstance(quad_samples,(list,tuple)) or
            len(quad_samples)!=4 or any(not isinstance(v,dict) for v in quad_samples)):
        raise RawShaderError('four explicit quad sample dictionaries required')
    if quad_initial is not None and proof['fetches'] and quad_samples is None:
        raise RawShaderError('quad texture samples must be explicit for all four lanes')
    nodes=proof['nodes'];values={};predicate_profile=proof['predicate_profile'] or proof.get('geometry_predicate_profile',False)
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
    def resolve(index,lane=quad_lane):
        if index is None:raise RawShaderError('executed undefined predicated value')
        key=(lane,index)
        if key in values:return values[key]
        def child(i):return resolve(i,lane)
        n=nodes[index];op=n[0]
        if op=='initial':value=vector_component((initial if quad_initial is None else quad_initial[lane]).get(n[1]),n[2])
        elif op=='sample':value=vector_component((samples if quad_samples is None else quad_samples[lane]).get(n[1]),n[2])
        elif op=='gradient':
            if quad_initial is None:raise RawShaderError('getGradients requires explicit quad inputs')
            gradient=proof['gradients'][n[1]];source=gradient['coordinates'][n[2]//2]
            value=arithmetic('add',resolve(source,1 if n[2]%2==0 else 2),-resolve(source,0))
        elif op=='literal':value=float(n[1])
        elif op=='predicate_select':value=child(n[2] if child(n[1]) else n[3])
        elif op=='predicate_condition':
            source=child(n[1])
            if source not in (0.,1.):raise RawShaderError('invalid predicate state')
            value=float(bool(source)==n[2])
        elif op in ('predicate_setne','predicate_setge'):
            source=child(n[1])
            if not math.isfinite(source) or (source!=0 and abs(source)<2**-126):
                raise RawShaderError('predicate source outside finite normal-or-zero domain')
            value=float(source==0.) if op=='predicate_setne' else float(not source>=0.)
        elif op=='constant':
            packed=constants.get(n[1])
            if not isinstance(packed,bytes) or len(packed)!=16:raise RawShaderError('missing exact BE pixel constant')
            value=struct.unpack_from('>f',packed,n[2]*4)[0]
            if not math.isfinite(value):raise RawShaderError('non-finite pixel constant')
        elif op in ('mul','add'):
            left,right=child(n[1]),child(n[2])
            if world_strip_underflow and proof.get('indexed_rgb7e3_profile',False) and op=='add':
                from indexed_rgb7e3_shader_policy import indexed_rgb7e3_add
                value=indexed_rgb7e3_add(left,right)
            elif geometry_lod_ieee and op=='mul' and (math.isinf(left) or math.isinf(right)):
                value=geometry_lod_infinite_product(left,right)
            elif zero_reciprocal_domain and op=='mul' and ((left==0 and math.isinf(right)) or (right==0 and math.isinf(left))):
                value=screen_strip_zero_infinity_product(left,right)
            else:value=arithmetic(op,left,right,denormal_multiply=world_strip_underflow)
        elif op=='not_equal':value=float(child(n[1])!=child(n[2]))
        elif op=='kill_gt':
            operands=[child(j) for j in n[1:]]
            comparisons=[evaluate_loading_operation('greater',[operands[j],operands[j+4]]) for j in range(4)]
            value=float(any(comparisons))
        elif op=='negate':value=-child(n[1])
        elif op=='exp2' and world_strip_underflow:value=world_strip_exp2(child(n[1]))
        elif geometry_lod_ieee and op=='log2' and child(n[1])==0:
            value=geometry_lod_zero_log(child(n[1]))
        elif geometry_lod_ieee and op=='exp2' and math.isinf(child(n[1])):
            value=geometry_lod_infinite_exp(child(n[1]))
        elif op in LOADING_OPERATIONS:value=evaluate_loading_operation(op,[child(j) for j in n[1:]])
        elif op in CUBE_OPERATIONS:value=evaluate_cube_operation(op,[child(j) for j in n[1:]])
        elif op=='reciprocal':
            operand=child(n[1])
            if zero_reciprocal_domain and operand==0:
                value=screen_strip_zero_reciprocal(operand);values[key]=value;return value
            if not math.isfinite(operand) or abs(operand)<2**-126:
                raise RawShaderError('reciprocal evaluator requires finite nonzero normal input')
            try:value=struct.unpack('>f',struct.pack('>f',1.0/operand))[0]
            except (OverflowError,struct.error) as error:
                raise RawShaderError('reciprocal result out of finite float32 range') from error
            if not math.isfinite(value) or abs(value)<2**-126:
                raise RawShaderError('reciprocal non-finite/subnormal result unsupported')
        elif op=='saturate':value=min(1.0,max(0.0,child(n[1])))
        elif op in ('regular_copy','select_eq0'):
            operands=[child(j) for j in n[1:]]
            if any(not math.isfinite(v) or (v!=0 and abs(v)<2**-126) for v in operands):
                raise RawShaderError('pixel copy/conditional requires normal-or-zero sources')
            value=operands[0] if op=='regular_copy' else operands[1] if operands[0]==0 else operands[2]
        else:raise RawShaderError('unknown pixel expression')
        values[key]=value;return value
    for i,fetch in enumerate(proof['fetches']):
        for coord in fetch['coordinates']:resolve(coord)
        if 'lod_expression' in fetch:resolve(fetch['lod_expression'])
        for component in range(4):vector_component((samples if quad_samples is None else quad_samples[quad_lane]).get(i),component)
    if predicate_profile:
        for group in proof['validation_groups']:
            if group['guard'] is None or resolve(group['guard']):
                for i in group['nodes']:resolve(i)
    else:
        for i,n in enumerate(nodes):
            if n[0] in ('gradient','mul','add','regular_copy','select_eq0','reciprocal') or n[0] in LOADING_OPERATIONS or n[0] in CUBE_OPERATIONS:resolve(i)  # Even overwritten scalar results must validate.
    # Deferred discard preserves helper-lane arithmetic/sampling. This bounded
    # reference requires all executed arithmetic to validate even for a killed
    # pixel; it does not imply immediate execution termination or NaN support.
    discarded=any((k['guard'] is None or resolve(k['guard'])) and resolve(k['condition']) for k in proof.get('kills',[]))
    exports={} if discarded else {slot:[None if n is None else resolve(n) for n in lanes] for slot,lanes in proof['exports'].items()}
    if screen_strip_ieee and any(v is not None and not math.isfinite(v) for lanes in exports.values() for v in lanes):
        raise RawShaderError('screen strip final output must remain finite')
    if geometry_lod_ieee and any(v is not None and not math.isfinite(v) for lanes in exports.values() for v in lanes):
        raise RawShaderError('geometry LOD final output must remain finite')
    if world_strip_underflow and any(v is not None and (not math.isfinite(v) or (v!=0 and abs(v)<2**-126))
            for lanes in exports.values() for v in lanes):
        raise RawShaderError('world strip final output requires finite normal-or-zero values')
    if include_queries or include_state:
        result={'exports':exports}
        if include_queries:result['queries']=[dict(slot=f['slot'],coordinates=[resolve(n) for n in f['coordinates']],
            **({'lod':resolve(f['lod_expression'])} if 'lod_expression' in f else {})) for f in proof['fetches']]
        if include_state:result.update(discarded=discarded,predicate=None if proof['predicate'] is None else resolve(proof['predicate']),
            previous_scalar=None if proof['previous_scalar'] is None else resolve(proof['previous_scalar']))
        return result
    return exports


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
