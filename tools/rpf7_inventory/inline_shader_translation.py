"""Translate owned straight-line UI shader code to a variable-size draw ABI.

No game inputs or pre-rendered pixels are embedded. ABI: 16-byte BE header,
256 BE vertex float4s, 256 BE pixel float4s, then the actual vertex bytes.
Binding0 is that SSBO, bindings1/2 the actual swizzled textures. Native state and
resource ownership are checked by the draw consumer, not inferred here.
"""
from vertex_arithmetic_dataflow import analyze_vertex_dataflow
from pixel_sample_dataflow import analyze_pixel_sample_dataflow
from raw_shader_analysis import RawShaderError
from affine_position import affine_position_xy
from cube_shader_math import CUBE_OPERATIONS, CUBE_GLSL
from pwl_texture_math import PWL_TEXTURE_GLSL
from cube_texture_sampling import CUBE_SAMPLE_GLSL
from geometry_ieee_math import GEOMETRY_EXP_GLSL
from geometry_output_merger import (GEOMETRY_OUTPUT_GLSL, GEOMETRY_VERTEX_DEPTH_GLSL,
    geometry_mrt_output_glsl, GEOMETRY_FIXED16_DEPTH_GLSL, GEOMETRY_FIXED16_VERTEX_GLSL)
from geometry_pixel_parameters import GEOMETRY_PARAMETER_XY_GLSL
from triangle_strip_topology import screen_strip_index_glsl
from screen_strip_shader_policy import SCREEN_STRIP_DEPTH_GLSL,SCREEN_STRIP_RECIPROCAL_GLSL,SCREEN_STRIP_BLEND_GLSL


COMMON = '''#version 450
layout(set=0,binding=0,std430) readonly buffer Inputs { uint words[]; } source;
uint readBE(uint at) {
    uint v=source.words[at];
    return (v<<24)|((v&0xff00u)<<8)|((v>>8)&0xff00u)|(v>>24);
}
'''


def translate_inline_pair(vertex_code, pixel_code, *, indexed=False, extended_profile=False, direct_triangles=False,
                          loading_profile=False, triangle_fan=False, quad_list=False, position_compute=False, color8_source_clamp=False,
                          cube_profile=False, rectangle_list=False, cube_pwl_rgb=False,cube_filter='linear',explicit_lod_profile=False,predicate_profile=False,
                          geometry_profile=False, geometry_compute=False, geometry_color8_format=None, geometry_centroid_mask=0,
                          geometry_mrt_formats=None,vertex_predicate_profile=False,geometry_predicate_profile=False,
                          geometry_parameter_register=None,geometry_blend_controls=None,geometry_depth_color_witness=False,
                          geometry_position_compute=False,geometry_gradient_depth_profile=False,
                          geometry_fixed16_effects_profile=False,fixed16_rectangle_depth_profile=False,vertex_address_profile=False,
                          vertex_denormal_multiply_profile=False,composite_profile=False,rectangle_mrt_profile=False,
                          offset_sample_profile=False,tonemap_profile=False,vertex_only=False,screen_space_profile=False,
                          post_stencil_profile=False,screen_strip_profile=False,world_strip_profile=False,
                          indexed_rgb7e3_profile=False,scalar_fan_profile=False,geometry_lod_profile=False):
    # ABI32 is OFFLINE shader capability, not native execution. It combines
    # packed/predicated vertices with unpredicated 2D pixel sampling, signed
    # half-texel offsets, register LOD, MIN and FRAC. Native consumers still owe
    # actual texture planes, implicit-LOD/offset behavior and format12 blending.
    if type(geometry_lod_profile) is not bool or (geometry_lod_profile and
            (not indexed or not extended_profile or not loading_profile or not geometry_profile or
             not vertex_predicate_profile or direct_triangles or triangle_fan or quad_list or
             rectangle_list or position_compute or color8_source_clamp or cube_profile or cube_pwl_rgb or
             explicit_lod_profile or predicate_profile or geometry_color8_format is not None or
             geometry_mrt_formats is not None or geometry_centroid_mask or
             geometry_parameter_register is not None or geometry_blend_controls is not None or
             geometry_depth_color_witness or geometry_predicate_profile or geometry_gradient_depth_profile or
             geometry_fixed16_effects_profile or fixed16_rectangle_depth_profile or vertex_address_profile or
             vertex_denormal_multiply_profile or composite_profile or rectangle_mrt_profile or
             offset_sample_profile or tonemap_profile or vertex_only or screen_space_profile or
             post_stencil_profile or screen_strip_profile or world_strip_profile or
             indexed_rgb7e3_profile or scalar_fan_profile)):
        raise RawShaderError('indexed LOD requires its separate packed-vertex, 2D pixel geometry profile')
    # ABI31 is an offline capability, not draw/query admission. This fan
    # writes exactly interpolator0.x; manufacturing yzw would hide a linkage
    # defect. Keep all old fan contracts and generated modules unchanged.
    if type(scalar_fan_profile) is not bool or (scalar_fan_profile and
            (not triangle_fan or not extended_profile or not loading_profile or
             indexed or direct_triangles or quad_list or rectangle_list or
             color8_source_clamp or cube_profile or cube_pwl_rgb or
             explicit_lod_profile or predicate_profile or geometry_profile or
             geometry_compute or geometry_position_compute or
             geometry_color8_format is not None or geometry_mrt_formats is not None or
             geometry_centroid_mask or geometry_parameter_register is not None or
             geometry_blend_controls is not None or geometry_depth_color_witness or
             vertex_predicate_profile or geometry_predicate_profile or
             geometry_gradient_depth_profile or geometry_fixed16_effects_profile or
             fixed16_rectangle_depth_profile or vertex_address_profile or
             vertex_denormal_multiply_profile or composite_profile or rectangle_mrt_profile or
             offset_sample_profile or tonemap_profile or vertex_only or screen_space_profile or
             post_stencil_profile or screen_strip_profile or world_strip_profile or indexed_rgb7e3_profile)):
        raise RawShaderError('scalar fan requires its separate nonindexed loading profile')
    # ABI30: separate resource-free indexed RGB7e3 replacement. No color8
    # clamping, fixed16 scale or constant-depth strip assumption. This offline
    # capability does NOT authorize a live boundary; the consumer owes actual
    # indices, culling, physical ownership, depth/stencil and numeric proof.
    if type(indexed_rgb7e3_profile) is not bool or (indexed_rgb7e3_profile and
            (not indexed or not extended_profile or not loading_profile or not geometry_profile or
             not geometry_predicate_profile or geometry_compute or direct_triangles or triangle_fan or
             quad_list or rectangle_list or position_compute or color8_source_clamp or cube_profile or
             cube_pwl_rgb or explicit_lod_profile or predicate_profile or geometry_color8_format is not None or
             geometry_mrt_formats is not None or geometry_centroid_mask or vertex_predicate_profile or
             geometry_parameter_register is not None or geometry_blend_controls is not None or
             geometry_depth_color_witness or geometry_gradient_depth_profile or geometry_fixed16_effects_profile or
             fixed16_rectangle_depth_profile or vertex_address_profile or vertex_denormal_multiply_profile or
             composite_profile or rectangle_mrt_profile or offset_sample_profile or tonemap_profile or
             vertex_only or screen_space_profile or post_stencil_profile or screen_strip_profile or world_strip_profile)):
        raise RawShaderError('indexed RGB7e3 requires its separate resource-free perspective profile')
    # ABI29 is offline analysis/translation only: original perspective float3
    # strip, four complete interpolators, four textures, pixel discard. It is
    # not ABI28's W1 grid or an indexed MRT/color8 draw. A native consumer still
    # owes clip/viewport, sampler, physical7e3/D24FS8 and output-state proofs.
    if type(world_strip_profile) is not bool or (world_strip_profile and
            (not extended_profile or not loading_profile or indexed or direct_triangles or
             triangle_fan or quad_list or rectangle_list or color8_source_clamp or
             cube_profile or cube_pwl_rgb or explicit_lod_profile or predicate_profile or
             geometry_profile or geometry_compute or geometry_position_compute or
             geometry_color8_format is not None or geometry_mrt_formats is not None or
             geometry_centroid_mask or geometry_parameter_register is not None or
             geometry_blend_controls is not None or geometry_depth_color_witness or
             vertex_predicate_profile or geometry_predicate_profile or
             geometry_gradient_depth_profile or geometry_fixed16_effects_profile or
             fixed16_rectangle_depth_profile or vertex_address_profile or
             vertex_denormal_multiply_profile or composite_profile or rectangle_mrt_profile or
             offset_sample_profile or tonemap_profile or vertex_only or screen_space_profile or
             post_stencil_profile or screen_strip_profile)):
        raise RawShaderError('world strip requires its separate four-texture perspective loading profile')
    # ABI28 is OFFLINE capability only. A native consumer must separately prove
    # strip extent, provoking/interpolation rules, sampling, existing RGB/depth/
    # stencil ownership and the captured blend state. No existing native ABI
    # admits this profile. Reuse unpacked DOT2ADD math, NOT tonemap scaling.
    if type(screen_strip_profile) is not bool or (screen_strip_profile and
            (not extended_profile or not loading_profile or indexed or direct_triangles or
             triangle_fan or quad_list or rectangle_list or color8_source_clamp or
             cube_profile or cube_pwl_rgb or explicit_lod_profile or predicate_profile or
             geometry_profile or geometry_compute or geometry_position_compute or
             geometry_color8_format is not None or geometry_mrt_formats is not None or
             geometry_centroid_mask or geometry_parameter_register is not None or
             geometry_blend_controls is not None or geometry_depth_color_witness or
             vertex_predicate_profile or geometry_predicate_profile or
             geometry_gradient_depth_profile or geometry_fixed16_effects_profile or
             fixed16_rectangle_depth_profile or vertex_address_profile or
             vertex_denormal_multiply_profile or composite_profile or rectangle_mrt_profile or
             offset_sample_profile or tonemap_profile or vertex_only or screen_space_profile or
             post_stencil_profile)):
        raise RawShaderError('screen strip requires its separate unpacked one-texture loading profile')
    # ABI27 is OFFLINE shader capability only. Its native consumer is not yet
    # implemented: RGB blending, existing colour/depth ownership, stencil tests
    # and physical output publication are separate requirements. Do not reuse
    # ABI24 (which scales its output for a different fixed16 render target).
    if type(post_stencil_profile) is not bool or (post_stencil_profile and
            (not extended_profile or not loading_profile or not rectangle_list or indexed or
             direct_triangles or triangle_fan or quad_list or color8_source_clamp or cube_profile or
             explicit_lod_profile or predicate_profile or geometry_profile or geometry_compute or
             geometry_position_compute or geometry_color8_format is not None or geometry_mrt_formats is not None or
             geometry_centroid_mask or geometry_parameter_register is not None or geometry_blend_controls is not None or
             geometry_depth_color_witness or vertex_predicate_profile or geometry_predicate_profile or
             geometry_gradient_depth_profile or geometry_fixed16_effects_profile or fixed16_rectangle_depth_profile or
             vertex_address_profile or vertex_denormal_multiply_profile or composite_profile or rectangle_mrt_profile or
             offset_sample_profile or tonemap_profile or vertex_only or screen_space_profile)):
        raise RawShaderError('post-stencil requires its separate three-texture rectangle profile')
    # ABI26 is a separate position-only rectangle interface. SQ_PROGRAM_CNTL
    # enables generated pixel XY in r0; the native consumer must verify that
    # exact state and an unscaled single-sample canvas. No unwritten varying
    # is invented, and ZW of a non-point generated parameter remain unknown.
    if type(screen_space_profile) is not bool or (screen_space_profile and
            (not composite_profile or not extended_profile or not loading_profile or
             not rectangle_list or indexed or direct_triangles or triangle_fan or quad_list or
             cube_profile or explicit_lod_profile or predicate_profile or geometry_profile or
             geometry_compute or geometry_position_compute or geometry_parameter_register is not None or
             geometry_color8_format is not None or geometry_mrt_formats is not None or
             geometry_centroid_mask or geometry_depth_color_witness or geometry_blend_controls is not None or
             geometry_predicate_profile or vertex_predicate_profile or color8_source_clamp or
             geometry_gradient_depth_profile or geometry_fixed16_effects_profile or
             fixed16_rectangle_depth_profile or rectangle_mrt_profile or offset_sample_profile or
             tonemap_profile or vertex_only)):
        raise RawShaderError('screen-space XY requires its separate composite rectangle profile')
    pixel_parameter_register=0 if screen_space_profile else geometry_parameter_register
    if type(vertex_denormal_multiply_profile) is not bool or (vertex_denormal_multiply_profile and
            (not vertex_address_profile or geometry_fixed16_effects_profile)):
        raise RawShaderError('vertex denormal multiplication requires the separate addressed fixed16 profile')
    if type(vertex_address_profile) is not bool or (vertex_address_profile and
            (not geometry_gradient_depth_profile or not geometry_profile or vertex_predicate_profile or geometry_compute)):
        raise RawShaderError('vertex address register requires separate fixed16 geometry raster/position profile')
    if type(fixed16_rectangle_depth_profile) is not bool or (fixed16_rectangle_depth_profile and
            (not rectangle_list or not loading_profile or not extended_profile or indexed or geometry_profile or
             geometry_gradient_depth_profile or geometry_fixed16_effects_profile or geometry_predicate_profile or
             vertex_predicate_profile or predicate_profile or cube_profile or explicit_lod_profile or
             position_compute or geometry_compute or geometry_position_compute or color8_source_clamp or
             geometry_color8_format is not None or geometry_mrt_formats is not None or geometry_centroid_mask or
             geometry_depth_color_witness or geometry_parameter_register is not None or geometry_blend_controls is not None)):
        raise RawShaderError('fixed16 rectangle depth requires separate nonindexed raster loading profile')
    fixed16_depth=geometry_gradient_depth_profile or fixed16_rectangle_depth_profile
    # ABI5 is experimental/offline. Existing native admission accepts ABI4's
    # narrower layouts/state only; callers must not install a new program merely
    # because this translator can emit GLSL for it.
    if geometry_profile and (not indexed or not loading_profile or cube_profile or rectangle_list or
            explicit_lod_profile or predicate_profile or triangle_fan or quad_list or direct_triangles or position_compute):
        raise RawShaderError('geometry requires the separate indexed straight-line loading profile')
    if geometry_compute and not geometry_profile:
        raise RawShaderError('geometry observation requires the explicit geometry profile')
    if type(geometry_position_compute) is not bool or (geometry_position_compute and
            (not geometry_profile or geometry_compute or position_compute)):
        raise RawShaderError('indexed position compute requires the separate geometry profile')
    if vertex_predicate_profile and not geometry_profile:
        raise RawShaderError('vertex predication requires separate geometry profile')
    mrt=geometry_mrt_formats is not None
    if type(geometry_fixed16_effects_profile) is not bool or (geometry_fixed16_effects_profile and
            (not geometry_gradient_depth_profile or not geometry_predicate_profile or
             geometry_depth_color_witness or geometry_parameter_register is not None or geometry_blend_controls is not None)):
        raise RawShaderError('fixed16 pixel effects require their separate textured predicate/gradient/depth profile')
    if type(geometry_gradient_depth_profile) is not bool or (geometry_gradient_depth_profile and
            (not geometry_profile or mrt or geometry_color8_format is not None or geometry_compute or
             (geometry_predicate_profile and not geometry_fixed16_effects_profile) or geometry_centroid_mask)):
        raise RawShaderError('fixed16 gradient/depth requires separate center-interpolated geometry profile')
    if type(geometry_depth_color_witness) is not bool or (geometry_depth_color_witness and not geometry_predicate_profile):
        raise RawShaderError('depth/color witness requires explicit geometry pixel profile')
    if geometry_predicate_profile and (not geometry_profile or (not mrt and not geometry_fixed16_effects_profile and not indexed_rgb7e3_profile) or geometry_compute):
        raise RawShaderError('geometry pixel predication requires separate MRT raster profile')
    if geometry_parameter_register is not None and (not geometry_predicate_profile or
            type(geometry_parameter_register) is not int or not 0<=geometry_parameter_register<16):
        raise RawShaderError('pixel parameter input requires explicit ABI15 register')
    if geometry_blend_controls is not None and (not geometry_predicate_profile or
            type(geometry_blend_controls) is not tuple or len(geometry_blend_controls)!=4 or
            any(type(v) is not int or v not in (0x10001,0x7010001,0x7000706) for v in geometry_blend_controls)):
        raise RawShaderError('four explicit supported ABI15 blend controls required')
    if mrt and (type(geometry_mrt_formats) is not tuple or len(geometry_mrt_formats)!=4 or
            all(f is None for f in geometry_mrt_formats) or
            any(f is not None and (type(f) is not int or f not in (0,1)) for f in geometry_mrt_formats) or
            not geometry_profile or geometry_compute or geometry_color8_format is not None or color8_source_clamp):
        raise RawShaderError('MRT requires four explicit optional color8 formats and separate geometry raster profile')
    if geometry_color8_format is not None and (type(geometry_color8_format) is not int or
            geometry_color8_format not in (0,1) or not geometry_profile or geometry_compute or color8_source_clamp):
        raise RawShaderError('color8/depth20e4 requires the separate geometry raster output profile')
    if type(geometry_centroid_mask) is not int or geometry_centroid_mask<0 or geometry_centroid_mask>0xFFFF or (
            geometry_centroid_mask and (not geometry_profile or geometry_compute)):
        raise RawShaderError('centroid mask requires explicit geometry raster state')
    if direct_triangles and (not extended_profile or indexed):
        raise RawShaderError('direct triangles require the explicit nonindexed extended profile')
    if loading_profile and not extended_profile:
        raise RawShaderError('loading math requires the explicit extended profile')
    if explicit_lod_profile and (not loading_profile or cube_profile):
        raise RawShaderError('explicit register LOD requires the separate 2D loading profile')
    if predicate_profile and not explicit_lod_profile:
        raise RawShaderError('predication requires the explicit-LOD profile')
    if cube_profile and not loading_profile:
        raise RawShaderError('cube sampling requires the explicit loading profile')
    # A full-screen composite: nonindexed rectangle-list geometry whose pixel
    # stage addresses its inputs in texels. It needs the straight-line 2D
    # loading arithmetic and nothing from the indexed geometry profile.
    if type(composite_profile) is not bool or (composite_profile and
            (not loading_profile or not rectangle_list or indexed or cube_profile or geometry_profile)):
        raise RawShaderError('composite sampling requires the explicit nonindexed rectangle loading profile')
    # A nonindexed rectangle pass that writes TWO colour targets and samples
    # with constant texel offsets. It needs the straight-line 2D loading
    # arithmetic and nothing from the indexed geometry profile, whose MRT band
    # stores through color8/depth helpers this pass does not use.
    # ABI23: the deferred lighting composite. Same nonindexed rectangle list and
    # same constant texel offsets as the MRT band, and it needs the identical
    # straight-line loading arithmetic, but it writes ONE colour target rather
    # than two. It is therefore the MRT band's fetch/ALU/interpolator profile
    # with the ordinary single-export contract, and never both at once.
    if type(offset_sample_profile) is not bool or (offset_sample_profile and
            (not loading_profile or not extended_profile or not rectangle_list or indexed or
             rectangle_mrt_profile or composite_profile or cube_profile or geometry_profile or
             direct_triangles or triangle_fan or quad_list or geometry_compute or
             explicit_lod_profile or predicate_profile or vertex_predicate_profile or color8_source_clamp or
             geometry_gradient_depth_profile or geometry_fixed16_effects_profile or
             fixed16_rectangle_depth_profile or vertex_address_profile or
             geometry_color8_format is not None or geometry_mrt_formats is not None)):
        raise RawShaderError('offset sample requires the explicit nonindexed rectangle loading profile')
    # ABI24. The tonemap pass has the offset sample band's exact draw shape -- a
    # nonindexed rectangle list writing ONE colour target with constant texel
    # offsets -- and differs only in the arithmetic its stages use: DOT2ADD in
    # the vertex stage, FRAC/LOG/SIN/COS in the pixel stage, and one fetch-all
    # fetch. It is therefore that band's profile plus those opcodes, and never
    # available without it.
    if type(tonemap_profile) is not bool or (tonemap_profile and not offset_sample_profile):
        raise RawShaderError('tonemap requires the explicit offset sample rectangle profile')
    if type(rectangle_mrt_profile) is not bool or (rectangle_mrt_profile and
            (not loading_profile or not extended_profile or not rectangle_list or indexed or
             composite_profile or cube_profile or geometry_profile or direct_triangles or triangle_fan or
             quad_list or geometry_compute or geometry_position_compute or
             explicit_lod_profile or predicate_profile or vertex_predicate_profile or color8_source_clamp or
             geometry_gradient_depth_profile or geometry_fixed16_effects_profile or
             fixed16_rectangle_depth_profile or vertex_address_profile or
             geometry_color8_format is not None or geometry_mrt_formats is not None)):
        raise RawShaderError('rectangle MRT requires the explicit nonindexed rectangle loading profile')
    if cube_pwl_rgb and not cube_profile:
        raise RawShaderError('cube PWL conversion requires explicit cube profile')
    if cube_filter not in ('point','linear') or (cube_filter!='linear' and not cube_profile):
        raise RawShaderError('explicit cube point or linear filter required')
    if rectangle_list and (not loading_profile or indexed or direct_triangles or triangle_fan or quad_list):
        raise RawShaderError('rectangle expansion requires explicit nonindexed loading profile')
    if triangle_fan and (not loading_profile or indexed or direct_triangles):
        raise RawShaderError('fan expansion requires explicit nonindexed loading profile')
    if quad_list and (not loading_profile or indexed or direct_triangles or triangle_fan):
        raise RawShaderError('quad expansion requires explicit nonindexed loading profile')
    if position_compute and (not (triangle_fan or quad_list or rectangle_list or screen_strip_profile or world_strip_profile) or not loading_profile):
        raise RawShaderError('position compute requires an explicit loading fan, quad or rectangle profile')
    # ABI25. A draw with NO pixel stage at all: the guest bound none, disabled
    # colour and depth writes, and left stencil enabled, so the only output is
    # the stencil buffer. Vulkan allows a pipeline without a fragment stage for
    # exactly that, and the vertex stage here exports position and nothing else.
    # It is a direct triangle list, so it takes that profile's own indexing.
    if type(vertex_only) is not bool or (vertex_only and
            (not direct_triangles or not loading_profile or indexed or position_compute or
             geometry_compute or geometry_profile or rectangle_list or triangle_fan or quad_list or
             cube_profile or explicit_lod_profile or predicate_profile or composite_profile or
             rectangle_mrt_profile or offset_sample_profile or tonemap_profile or
             fixed16_rectangle_depth_profile or geometry_gradient_depth_profile or
             color8_source_clamp or geometry_color8_format is not None or
             geometry_mrt_formats is not None)):
        raise RawShaderError('vertex only requires the separate direct triangle loading profile')
    if vertex_only and pixel_code:
        raise RawShaderError('vertex only requires an absent pixel stage')
    if color8_source_clamp and (not triangle_fan or not loading_profile or position_compute):
        raise RawShaderError('fixed-point source clamp requires an explicit raster loading fan')
    vs=analyze_vertex_dataflow(vertex_code,dot4=True,scalar_constants=extended_profile,loading_math=loading_profile,
        packed_geometry=geometry_profile,predicate_profile=vertex_predicate_profile,address_profile=vertex_address_profile,
        tonemap=tonemap_profile or post_stencil_profile or screen_strip_profile,
        scalar_gt_zero=world_strip_profile)  # Unpacked DOT2ADD, not output scaling.
    # An absent pixel stage analyses to nothing rather than being threaded
    # through every predicate below: each one is about a stage that is not there,
    # and an empty analysis makes them all vacuous on their own terms.
    ps={'fetches':[],'exports':{},'gradients':[],'kills':[],'nodes':[],'lod_sets':[],
        'predicate':None,'allocations':[],'executed_instruction_addresses':[],
        'sha256':None,'interpolation_verified':False} if vertex_only else analyze_pixel_sample_dataflow(
        pixel_code,ui_operations=True,extended_math=extended_profile,loading_math=loading_profile,
        cube_profile=cube_profile,explicit_lod_profile=explicit_lod_profile,predicate_profile=predicate_profile,
        geometry_profile=geometry_profile or world_strip_profile,
        geometry_predicate_profile=geometry_predicate_profile or world_strip_profile,
        geometry_gradient_depth_profile=geometry_gradient_depth_profile,
        geometry_fixed16_effects_profile=geometry_fixed16_effects_profile,
        rectangle_depth_profile=fixed16_rectangle_depth_profile,
        composite_profile=composite_profile,rectangle_mrt_profile=rectangle_mrt_profile or offset_sample_profile,
        tonemap_profile=tonemap_profile,post_stencil_profile=post_stencil_profile,
        geometry_lod_profile=geometry_lod_profile)
    textured=bool(ps['fetches'])
    expected=set(vs['exports'])
    valid_layouts=({'62'},) if vertex_only or screen_space_profile else (
        ({'0','1','62'},{'0','1','2','62'},{'0','1','2','3','62'}) if textured else ({'0','62'},))
    if textured and extended_profile and not screen_space_profile:valid_layouts=({'0','62'},*valid_layouts)
    # The textured UI layouts carry a two-lane first interpolator. The captured
    # rectangle MRT stage writes all four lanes of BOTH interpolators and its
    # pixel stage reads them, so that narrowing must not be applied to it.
    # The offset sample band's narrowing was pinned per profile because the
    # captured ABI23 and ABI24 pairs write all four lanes of every interpolator.
    # A later pair in the same family writes only two of interpolator 0, so this
    # asks the VERTEX STAGE what it actually provided rather than asking the
    # profile. The answer flows into the interface declaration, the export list
    # and the emitter's read guard alike, and the emitter already refuses a pixel
    # read past it ('unprovided stage input'), so a lane that is read but never
    # written still fails -- it just fails for the true reason.
    def written_prefix(key):
        lanes=vs['exports'][key];count=0
        while count<4 and lanes[count] is not None:count+=1
        # A gap would make vecN carry a lane the vertex stage never assigned.
        if any(n is not None for n in lanes[count:]):
            raise RawShaderError('interpolator lanes must be written as a contiguous prefix')
        return count
    components=lambda key:(written_prefix(key) if scalar_fan_profile or offset_sample_profile or post_stencil_profile or screen_strip_profile or world_strip_profile else
        2 if textured and not cube_profile and not rectangle_mrt_profile and (key=='0' or (len(expected)==5 and key=='1')) else 4)
    # Geometry shaders often export xyz only. Preserve each written lane as a
    # separate scalar interface component; NEVER manufacture an unwritten w.
    # Pixel expression traversal below rejects an actual read of a missing lane.
    masks={s:sum(1<<lane for lane,n in enumerate(vs['exports'][s]) if n is not None)
        for s in expected-{'62'}}
    if scalar_fan_profile and (expected!={'0','62'} or masks!={'0':1} or
            ps['fetches'] or ps['kills'] or ps['gradients'] or '61' in ps['exports']):
        raise RawShaderError('scalar fan requires position, only interpolator0.x and resource-free colour')
    valid_geometry=('62' in expected and None not in vs['exports']['62'] and
        all(s in {str(i) for i in range(16)} and mask for s,mask in masks.items()))
    color_exports={k:v for k,v in ps['exports'].items() if k!='61'}
    pixel_outputs=sorted(color_exports,key=int)
    if fixed16_rectangle_depth_profile and (not ps['fetches'] or ps['gradients'] or ps['kills'] or
            ps['exports'].get('61',[None])[0] is None or expected!={'0','62'} or
            len({f['slot'] for f in ps['fetches']})!=1):
        raise RawShaderError('fixed16 rectangle requires sampled color and explicit depth X without gradients or discard')
    if geometry_gradient_depth_profile and ((ps['fetches'] and not geometry_fixed16_effects_profile) or not ps['gradients'] or
            ps['exports'].get('61',[None])[0] is None):
        raise RawShaderError('fixed16 profile requires resource-free gradients and explicit depth X')
    valid_pixel=True if vertex_only else (bool(pixel_outputs) and all(s in ('0','1','2','3') and
        geometry_mrt_formats[int(s)] is not None and None not in ps['exports'][s] for s in pixel_outputs)) if mrt else (
        set(color_exports)=={'0','1'} and all(None not in color_exports[s] for s in ('0','1'))
        ) if rectangle_mrt_profile else (
        set(color_exports)=={'0'} and None not in color_exports['0'])
    # Position is never narrowed: a rectangle needs all four lanes of it.
    if offset_sample_profile and '62' in expected and components('62')!=4:
        raise RawShaderError('offset sample position export requires all four lanes')
    if ((not valid_geometry if geometry_profile else expected not in valid_layouts or
        any(n is None for key in expected for n in vs['exports'][key][:components(key)])) or
        not valid_pixel or len(ps['fetches'])>(32 if extended_profile else 2)):
        raise RawShaderError('complete supported interpolators, position and color output required')
    if geometry_blend_controls is not None and any(geometry_blend_controls[int(s)]!=0x10001 and
            geometry_mrt_formats[int(s)]!=0 for s in pixel_outputs):
        raise RawShaderError('MRT blending requires linear UNORM output')
    slots=list(dict.fromkeys(f['slot'] for f in ps['fetches']))
    if geometry_fixed16_effects_profile and len(slots)>4:
        raise RawShaderError('fixed16 pixel ABI reserves binding5 for completion; at most four textures supported')
    if mrt and len(slots)>4:
        raise RawShaderError('MRT ABI reserves binding5 for completion; at most four textures supported')
    dimensions={slot:{f.get('dimension','2d') for f in ps['fetches'] if f['slot']==slot} for slot in slots}
    if any(len(d)!=1 for d in dimensions.values()):
        raise RawShaderError('one texture dimension per binding required')
    if (len(slots)>(32 if geometry_profile else 4 if world_strip_profile else 3 if post_stencil_profile else 2) or (not extended_profile and ((len(expected)==5)!=(len(ps['fetches'])==2) or
        len(slots)!=len(ps['fetches'])))):
        raise RawShaderError('two independent texture slots require the four-interpolator layout')
    fetches=vs['fetches']
    if (not fetches or len({f['stride_words'] for f in fetches})!=1 or
        any(f['fetch_slot']!=95 or vs['nodes'][f['index_expression']]!=('initial',0,0) for f in fetches)):
        raise RawShaderError('one inline auto-indexed vertex stream required')
    stride=fetches[0]['stride_words']
    if indexed_rgb7e3_profile and (expected!={'0','62'} or masks!={'0':7} or ps['fetches'] or
            ps['gradients'] or ps['lod_sets'] or set(ps['exports'])!={'0'} or
            stride!=6 or len(fetches)!=1 or fetches[0]['format']!=57 or fetches[0]['offset_words']):
        raise RawShaderError('indexed RGB7e3 requires stride24/float3, one xyz interpolator and resource-free colour0')
    if indexed_rgb7e3_profile:ps['indexed_rgb7e3_profile']=True
    if world_strip_profile and (expected!={'0','1','2','3','62'} or
            any(components(k)!=4 for k in expected) or len(ps['fetches'])!=5 or set(slots)!={1,2,3,6} or
            set(ps['exports'])!={'0'} or ps['gradients'] or ps['lod_sets'] or
            any(f.get('texel_offset') or f.get('denormalized_coordinates') or f.get('dimension','2d')!='2d' for f in ps['fetches']) or
            stride!=9 or len(fetches)!=1 or fetches[0]['format']!=57 or fetches[0]['offset_words']):
        raise RawShaderError('world strip requires stride36/float3, four complete interpolators and five reads of slots1/2/3/6')
    if screen_strip_profile and (expected!={'0','1','62'} or
            any(components(k)!=4 for k in expected) or len(ps['fetches'])!=1 or slots!=[3] or
            set(ps['exports'])!={'0'} or ps['gradients'] or ps['kills'] or ps['lod_sets'] or
            any(not f['fetch_valid_only'] or f.get('texel_offset') or f.get('denormalized_coordinates') for f in ps['fetches']) or
            stride!=2 or len(fetches)!=1 or fetches[0]['format']!=37 or fetches[0]['offset_words']):
        raise RawShaderError('screen strip requires stride8/float2, two complete interpolators and one slot3 sample')
    if screen_strip_profile:ps['screen_strip_profile']=True
    if world_strip_profile:ps['world_strip_profile']=True
    if post_stencil_profile and (expected!={'0','1','62'} or len(ps['fetches'])!=3 or
            set(slots)!={7,8,9} or ps['gradients'] or ps['kills'] or
            any(not f['fetch_valid_only'] or f.get('texel_offset') or f.get('denormalized_coordinates') for f in ps['fetches']) or
            stride!=9 or len(fetches)!=1 or fetches[0]['format']!=57 or fetches[0]['offset_words']):
        raise RawShaderError('post-stencil requires the captured stride36/three valid-only sample interface')
    for f in fetches:
        formats=(6,7,25,31,32,36,37,38,57) if geometry_profile else (6,25,36,37,38,57) if extended_profile else (6,25,37,38)
        words=1 if f['format'] in (6,7,25,31) else 2 if f['format']==32 else f['components']
        if f['format'] not in formats or f['offset_words']+words>stride:
            raise RawShaderError('unsupported inline vertex format/extent')
    fetch=ps['fetches'][0] if textured else None
    if screen_space_profile and (len(ps['fetches'])!=1 or ps['gradients'] or ps['kills'] or
            not fetch.get('denormalized_coordinates') or not fetch.get('explicit_point_filter')):
        raise RawShaderError('screen-space XY requires one texel-addressed point fetch without gradients or discard')
    sample_coordinates=[]
    for sample in ps['fetches']:
        if extended_profile:
            sample_coordinates.append(sample['coordinates'])
            continue
        coordinates=[ps['nodes'][i] for i in sample['coordinates']]
        if (len(coordinates)!=2 or any(n[0]!='initial' for n in coordinates) or
            coordinates[0][1]!=coordinates[1][1] or [n[2] for n in coordinates]!=[0,1] or
            str(coordinates[0][1]) not in expected or coordinates[0][1]>=4):
            raise RawShaderError('unchanged provided interpolated UV required')
        sample_coordinates.append(coordinates[0][1])

    def body(proof, vertex, rectangle_function=False):
        nodes=proof['nodes'];statements=[];done={};sampled=set();sampling=set();assumptions={}
        stage_predicate=vertex_predicate_profile if vertex else predicate_profile or geometry_predicate_profile or world_strip_profile
        if not vertex and proof.get('kills'):statements.append('bool pixelKilled=false;')
        if not vertex and pixel_parameter_register is not None:statements.append(GEOMETRY_PARAMETER_XY_GLSL)
        def address_fault(condition):
            if geometry_position_compute:
                # A position diagnostic has no draw-completion buffer. Preserve
                # explicit nonfinite output so its consumer cannot pass clipping.
                action='for(uint lane=0u;lane<4u;++lane) result.words[4u+index*4u+lane]=0x7fc00000u;'
            else:
                action='atomicOr(geometryCompletion.words[4],8u); gl_Position=vec4(0.0,0.0,-1.0,1.0);'
            statements.append(f'if ({condition}) {{ {action} return; }}')
        if vertex:
            for i,f in enumerate(fetches):
                at=f"2052u+index*{stride}u+{f['offset_words']}u"
                if f['format'] in (6,7,25,31,32):statements.append(f'uint packed{i}=readBE({at});')
                if f['format']==32:statements.append(f'uint packed{i}High=readBE({at}+1u);')
                for lane in range(f['components']):
                    value=(f'float((packed{i}>>{lane*8}u)&255u)*(1.0/255.0)' if f['format']==6 else
                           f'max(-1.0,float(bitfieldExtract(int(packed{i}),{lane*10 if lane<3 else 30},{10 if lane<3 else 2}))*(1.0/{511 if lane<3 else 1}.0))' if f['format']==7 else
                           f'unpackHalf2x16(packed{i}{"High" if lane>=2 else ""})[{lane%2}]' if f['format'] in (31,32) else
                           f'float(bitfieldExtract(int(packed{i}),{lane*16},16))' if f['format']==25 else
                           f'uintBitsToFloat(readBE({at}+{lane}u))')
                    statements.append(f'precise float fetch{i}_{lane}={value};')
        elif textured and not extended_profile:
            for i,coordinate in enumerate(sample_coordinates):
                statements.append(f'vec4 sampled{i}=texture(atlas{i},interpolator{coordinate}.xy);')
        def coordinate_vector(i,binding,coordinates):
            # TX_COORD_DENORM addresses in texels. Normalize by the actual bound
            # size instead of using an unnormalized-coordinate sampler: Vulkan
            # forbids those with REPEAT addressing or multiple mip levels, both
            # of which real bindings here use. Dividing preserves the fetch
            # constant's own clamp/repeat policy exactly.
            vector=f'vec2({",".join(coordinates)})'
            if ps['fetches'][i].get('denormalized_coordinates'):
                if screen_space_profile:
                    # Xenos rounds coordinates at 1/256-texel precision. This
                    # narrow point-fetch profile starts exactly on integer
                    # texels; division may otherwise select the previous texel.
                    # Xenia's fetch translator uses 1.5/1024 texels before
                    # normalization (less than half its rounding interval).
                    # This is not a half-texel offset, and does not alter the
                    # already qualified, separately admitted profiles.
                    vector=f'({vector}+vec2(0.00146484375))'
                return f'({vector}/vec2(textureSize(atlas{binding},0)))'
            # The metadata converts signed half-texel fields into TEXELS, so it is
            # divided by the bound size to reach the normalized coordinate the
            # sampler takes. It is applied here rather than dropped; the fetch
            # constant's own clamp/repeat policy still decides what lies
            # outside, exactly as for the denormalized case above.
            offset=ps['fetches'][i].get('texel_offset')
            if offset:
                return f'({vector}+vec2({offset[0]},{offset[1]})/vec2(textureSize(atlas{binding},0)))'
            return vector

        def emit_sample(i):
            if i in sampled:return
            if i in sampling:raise RawShaderError('cyclic texture dataflow')
            sampling.add(i)
            coordinates=[expression(n) for n in sample_coordinates[i]]
            binding=slots.index(ps['fetches'][i]['slot'])
            if ps['fetches'][i].get('dimension')=='cube':
                # Xenos face-local ST is in [1,2], with explicit face ID.
                # An array avoids imposing seamless host cube filtering. This
                # Conversion is post-filter; actual signs/filter/clamp must be
                # validated by the consumer against this explicit profile.
                statements.append(f'vec4 sampled{i}=sampleCubeBase(atlas{binding},vec3(({coordinates[0]})-1.0,({coordinates[1]})-1.0,{coordinates[2]}),'+('true' if cube_filter=='linear' else 'false')+');')
                if cube_pwl_rgb:
                    for channel in 'rgb':statements.append(f'sampled{i}.{channel}=xenosPwlSample(sampled{i}.{channel});')
            elif 'lod_expression' in ps['fetches'][i]:
                lod=expression(ps['fetches'][i]['lod_expression'])
                statements.append(f'vec4 sampled{i}=textureLod(atlas{binding},{coordinate_vector(i,binding,coordinates)},{lod});')
            else:
                statements.append(f'vec4 sampled{i}=texture(atlas{binding},{coordinate_vector(i,binding,coordinates)});')
            sampling.remove(i);sampled.add(i)
        def branch(guard,taken,emit):
            # Values emitted inside a branch are not definitely assigned
            # outside it. Keep expression/sample caches lexically scoped.
            saved_done=done.copy();saved_sampled=sampled.copy();saved_assumptions=assumptions.copy()
            condition=nodes[guard]
            if condition[0]!='predicate_condition':raise RawShaderError('unknown predicate guard encoding')
            assumptions[condition[1]]=condition[2] if taken else not condition[2]
            try:emit()
            finally:
                done.clear();done.update(saved_done)
                sampled.clear();sampled.update(saved_sampled)
                assumptions.clear();assumptions.update(saved_assumptions)
        def expression(i):
            if i is None:raise RawShaderError('unprovided predicated value on reachable path')
            if len(statements)>16384:raise RawShaderError('predicated shader statement budget exceeded')
            # Simplify under the CURRENT branch before consulting the outer
            # cache. Complementary writes must not read an undefined old lane.
            n=nodes[i];op=n[0]
            if op=='predicate_condition' and n[1] in assumptions:
                return '1.0' if assumptions[n[1]]==n[2] else '0.0'
            if op=='predicate_select':
                condition=nodes[n[1]]
                if condition[1] in assumptions:
                    return expression(n[2] if assumptions[condition[1]]==condition[2] else n[3])
            if i in done:return done[i]
            if op=='literal':value=f'{n[1]}.0'
            elif op=='predicate_select':
                if not stage_predicate:raise RawShaderError('predicate expression outside explicit stage profile')
                guard=expression(n[1]);value=f'n{i}'
                statements.append(f'precise float {value};')
                statements.append(f'if ({guard}!=0.0) {{')
                branch(n[1],True,lambda:statements.append(f'{value}={expression(n[2])};'))
                statements.append('} else {')
                branch(n[1],False,lambda:statements.append(f'{value}={expression(n[3])};'))
                statements.append('}')
            elif op in ('predicate_condition','predicate_setne','predicate_setge'):
                if not stage_predicate:raise RawShaderError('predicate operation outside explicit stage profile')
                source=expression(n[1]);value=f'n{i}'
                if op=='predicate_setge':
                    statements.append(f'precise float {value}=({source}>=0.0?0.0:1.0);')
                else:
                    test=f'{source}!=0.0' if op=='predicate_condition' and n[2] else f'{source}==0.0'
                    statements.append(f'precise float {value}=({test}?1.0:0.0);')
            elif op=='initial':
                if vertex and n[1:]==(0,0):value='float(index)'
                elif not vertex and n[1]==pixel_parameter_register:
                    if n[2]>=2:raise RawShaderError('non-point generated pixel ZW remain unknown')
                    value=f'generatedPixelXY[{n[2]}]'
                elif not vertex and geometry_profile:
                    if not masks.get(str(n[1]),0)&(1<<n[2]):
                        raise RawShaderError(f'pixel reads an unwritten geometry interpolator lane r{n[1]}.{n[2]} (node{i})')
                    value=f'interpolator{n[1]}_{n[2]}'
                elif not vertex and str(n[1]) in expected and n[1]<4 and n[2]<components(str(n[1])):
                    value=(f'interpolator{n[1]}' if scalar_fan_profile and components(str(n[1]))==1
                           else f'interpolator{n[1]}[{n[2]}]')
                else:raise RawShaderError('unprovided stage input')
            elif op=='fetch':value=f'fetch{n[1]}_{n[2]}'
            elif op=='sample':
                if vertex or n[1]>=len(sample_coordinates):raise RawShaderError('unexpected texture sample')
                if extended_profile:emit_sample(n[1])
                value=f'sampled{n[1]}[{n[2]}]'
            elif op=='gradient':
                if vertex or not geometry_gradient_depth_profile:
                    raise RawShaderError('gradient expression outside explicit pixel quad profile')
                source=expression(proof['gradients'][n[1]]['coordinates'][n[2]//2])
                operation='dFdxCoarse' if n[2]%2==0 else 'dFdyCoarse'
                value=f'n{i}';statements.append(f'precise float {value}={operation}({source});')
            elif op=='address_round':
                if not vertex or not vertex_address_profile:raise RawShaderError('address mutation outside explicit vertex profile')
                source=expression(n[1]);value=f'n{i}'
                address_fault(f'isnan({source}) || isinf({source}) || ({source}!=0.0 && abs({source})<uintBitsToFloat(0x00800000u))')
                statements.append(f'precise float addressInput{i}={source}'+('+0.5;' if n[2] else ';'))
                statements.append(f'precise float {value}=clamp(floor(addressInput{i}),-256.0,255.0);')
            elif op=='relative_constant':
                if not vertex or not vertex_address_profile:raise RawShaderError('dynamic constant outside explicit vertex profile')
                address=expression(n[3]);value=f'n{i}'
                statements.append(f'int constantIndex{i}={n[1]}+int({address});')
                address_fault(f'constantIndex{i}<0 || constantIndex{i}>=256')
                statements.append(f'precise float {value}=uintBitsToFloat(readBE(4u+uint(constantIndex{i})*4u+{n[2]}u));')
                address_fault(f'isnan({value}) || isinf({value})')
            elif op=='constant':value=f'uintBitsToFloat(readBE({(4 if vertex else 1028)+n[1]*4+n[2]}u))'
            elif op in CUBE_OPERATIONS:
                if vertex or not cube_profile:raise RawShaderError('CUBE requires explicit pixel cube profile')
                args=[expression(j) for j in n[1:]]
                value=f'n{i}'
                statements.append(f'precise float {value}=xenosCube(vec3({",".join(args)}))[{CUBE_OPERATIONS.index(op)}];')
            elif op=='kill_gt':
                if vertex or not (geometry_predicate_profile or world_strip_profile):raise RawShaderError('pixel kill outside geometry predicate profile')
                args=[expression(j) for j in n[1:]]
                value=f'n{i}'
                test=' || '.join(f'{args[j]}>{args[j+4]}' for j in range(4))
                statements.append(f'precise float {value}=({test}?1.0:0.0);')
            elif op in ('mul','add','regular_copy','select_eq0','not_equal','equal','saturate','negate','reciprocal',
                         'sqrt','rsqrt','exp2','log2','greater','greater_equal','nonzero','truncate','fraction','select_gt0','maximum','minimum','absolute','sine','cosine'):
                args=[expression(j) for j in n[1:]]
                if op=='mul':
                    if (world_strip_profile or indexed_rgb7e3_profile) and not vertex:
                        # Observed EXP_IEEE underflow feeds MUL. SM3 denormal
                        # input -> +0 regardless of native EXP flush behavior.
                        rhs=f'(((floatBitsToUint({args[0]})&0x7f800000u)==0u || (floatBitsToUint({args[1]})&0x7f800000u)==0u)?0.0:({args[0]}*{args[1]}))'
                    elif vertex and vertex_denormal_multiply_profile:
                        # SM3 zero/denormal multiplicand -> +0 (ReX c94f5ebd
                        # dxbc_translator_alu.cpp kMul/kMad). Inspect exponent
                        # bits, not driver-dependent denormal float compares.
                        # Nonfinite operands remain outside this finite profile.
                        address_fault(f'isnan({args[0]}) || isinf({args[0]}) || isnan({args[1]}) || isinf({args[1]})')
                        rhs=f'(((floatBitsToUint({args[0]})&0x7f800000u)==0u || (floatBitsToUint({args[1]})&0x7f800000u)==0u)?0.0:({args[0]}*{args[1]}))'
                    else:rhs=f'(({args[0]}==0.0 || {args[1]}==0.0)?0.0:({args[0]}*{args[1]}))'
                elif op=='add':rhs=f'({args[0]}+{args[1]})'
                elif op=='regular_copy':rhs=args[0]
                elif op=='select_eq0':rhs=f'({args[0]}==0.0?{args[1]}:{args[2]})'
                elif op=='not_equal':rhs=f'({args[0]}!={args[1]}?1.0:0.0)'
                elif op=='equal':rhs=f'({args[0]}=={args[1]}?1.0:0.0)'
                elif op=='negate':rhs=f'(-{args[0]})'
                elif op=='reciprocal':
                    if not extended_profile:raise RawShaderError('reciprocal requires extended profile')
                    rhs=f'screenStripReciprocal({args[0]})' if screen_strip_profile and not vertex else f'(1.0/{args[0]})'
                elif op in ('sqrt','rsqrt','exp2','log2','greater','greater_equal','nonzero','truncate','fraction','select_gt0','maximum','minimum','absolute','sine','cosine'):
                    if not loading_profile:raise RawShaderError('loading operation requires explicit loading profile')
                    if op=='log2' and not (geometry_profile or tonemap_profile or post_stencil_profile or world_strip_profile):raise RawShaderError('logarithm requires explicit arithmetic profile')
                    # Radians in, as Xenos SIN/COS take them. GLSL sin/cos are
                    # the host's own; this reproduces the operation, not the
                    # Xbox's approximation of it.
                    if op in ('sine','cosine') and not tonemap_profile:raise RawShaderError('periodic transcendental requires explicit tonemap profile')
                    if op in ('sqrt','rsqrt','exp2','log2','absolute','truncate','fraction','sine','cosine'):
                        function={'sqrt':'sqrt','rsqrt':'inversesqrt','exp2':'exp2','log2':'log2','absolute':'abs','truncate':'trunc','fraction':'fract','sine':'sin','cosine':'cos'}[op]
                        if op=='exp2' and (geometry_profile or world_strip_profile):function='geometryExp2'
                        rhs=f'{function}({args[0]})'
                    elif op=='greater':rhs=f'({args[0]}>{args[1]}?1.0:0.0)'
                    elif op=='greater_equal':rhs=f'({args[0]}>={args[1]}?1.0:0.0)'
                    elif op=='nonzero':rhs=f'({args[0]}!=0.0?1.0:0.0)'
                    elif op=='select_gt0':rhs=f'({args[0]}>0.0?{args[1]}:{args[2]})'
                    elif op=='minimum':rhs=f'({args[0]}<{args[1]}?{args[0]}:{args[1]})'
                    else:rhs=f'({args[0]}>={args[1]}?{args[0]}:{args[1]})'
                else:rhs=f'clamp({args[0]},0.0,1.0)'
                value=f'n{i}';statements.append(f'precise float {value}={rhs};')
            else:raise RawShaderError('unsupported translated expression')
            done[i]=value;return value
        outputs=([('62','position' if position_compute or geometry_compute or geometry_position_compute or rectangle_function else 'gl_Position',4)]+
                 ([] if position_compute or geometry_profile else [(s,'interpolator'+s,components(s)) for s in sorted(expected-{'62'})])) if vertex else (
                     [(s,'color'+s,4) for s in pixel_outputs] if mrt or rectangle_mrt_profile else [('0','color',4)])
        if not vertex and extended_profile:
            # Every original fetch is emitted once in instruction order, even
            # when its result is overwritten. Coordinates use the snapshot at
            # that fetch, not the shader's final temporary register contents.
            for i in range(len(sample_coordinates)):emit_sample(i)
        if not vertex and geometry_gradient_depth_profile:
            # Unconditional original gradient operations before any kill/error
            # discard. Nodes refer to the pre-fetch expression snapshot, not
            # the final destination register. Preserve overwritten gradients.
            for i,n in enumerate(nodes):
                if n[0]=='gradient':expression(i)
        if vertex and vertex_address_profile:
            # Validate executed address writes even if a later instruction
            # overwrites a0. Coissue reads are already bound to immutable nodes.
            for write in proof['address_writes']:expression(write['node'])
            for i,n in enumerate(nodes):
                if n[0]=='relative_constant':expression(i)
        if stage_predicate:
            # Execute even overwritten scalar arithmetic, but only under its
            # instruction guard. Joins remain lazy rather than flattening both
            # branches into unconditional rsqrt/division/undefined-source reads.
            for group in proof['validation_groups']:
                def emit_group():
                    for i in group['nodes']:expression(i)
                    if 'kill' in group:
                        statements.append(f'pixelKilled=pixelKilled || ({expression(group["kill"])}!=0.0);')
                if group['guard'] is None:
                    emit_group()
                else:
                    guard=expression(group['guard'])
                    statements.append(f'if ({guard}!=0.0) {{')
                    branch(group['guard'],True,emit_group)
                    statements.append('}')
        # Fetches above remain outside kill control flow, preserving derivatives
        # for surviving lanes. No color/depth/error-buffer store precedes this
        # actual discard. A zero destination mask never suppresses pixel kill.
        if not vertex and proof.get('kills'):statements.append('if (pixelKilled) discard;')
        for slot,target,count in outputs:
            values=[expression(n) for n in proof['exports'][slot][:count]]
            statements.append(f'{target}={values[0]};' if scalar_fan_profile and count==1 else
                              f'{target}=vec{count}({",".join(values)});')
        if vertex and geometry_profile and not geometry_position_compute:
            for slot in sorted(masks,key=int):
                for lane,n in enumerate(proof['exports'][slot]):
                    if n is not None:statements.append(f'interpolator{slot}_{lane}={expression(n)};')
        if not vertex and color8_source_clamp:
            # Fixed-point render targets clamp source color/alpha BEFORE
            # blending (pinned Xenia-derived dxbc_translator_om.cpp). The
            # separately owned color target quantizes AFTER each draw. No
            # hardware sRGB conversion or pre-blend byte quantization here.
            statements.append('color=clamp(color,vec4(0.0),vec4(1.0));')
        if not vertex and screen_strip_profile:
            statements.append('color=screenStripPrepareBlend(color);')
        if not vertex and (world_strip_profile or indexed_rgb7e3_profile):
            statements.append('gl_FragDepth=geometryExpand20e4(geometryDepth20e4(floatBitsToUint(gl_FragCoord.z)));')
        if not vertex and tonemap_profile:
            # The ABI24 pass writes a k_16_16 target: two channels of fixed
            # point over [-32,32] (reference xenos.h:307, and the tree's own
            # PPC-derived packGta5Fixed16Clear, which maps +/-32 to +/-32767).
            # Vulkan has no such format, so the attachment is R16G16_SNORM and
            # the scale that the hardware applies on encode is applied here:
            # after it, SNORM's own clamp-and-scale stores exactly the int16
            # the guest would. Rounding at an exact midpoint is the host's
            # round-to-nearest rather than the guest's round-nearest-even, so
            # a single ULP there is not claimed to be reproduced.
            statements.append('color=vec4(color.xy*(1.0/32.0),color.zw);')
        if not vertex and geometry_color8_format is not None:
            statements.append(f'geometryStoreColor8Depth20e4(color,{geometry_color8_format}u);')
        if not vertex and fixed16_depth:
            statements.append(f'geometryStoreFixed16ExplicitDepth(color,{expression(proof["exports"]["61"][0])});')
        if not vertex and mrt:
            for slot in pixel_outputs:
                if geometry_blend_controls is not None and geometry_blend_controls[int(slot)]!=0x10001:
                    statements.append(f'geometryClampMrtBlendSource(color{slot});')
                else:statements.append(f'geometryStoreMrtColor8(color{slot},{geometry_mrt_formats[int(slot)]}u);')
            statements.append('geometryStoreMrtDepth20e4();')
            if geometry_depth_color_witness:statements.append('geometryCheckDestinationWitness();')
        if vertex and not position_compute and not geometry_compute and not geometry_position_compute and not rectangle_function:
            # Host supplies the actual signed viewport and pixel-center
            # mapping into the padded native viewport. Preserve non-unit W.
            statements.extend([
                'gl_Position.xy=gl_Position.xy*viewport.scaleOffset.xy+viewport.scaleOffset.zw*gl_Position.w;'])
            if geometry_color8_format is not None or mrt:
                statements.append('geometryStoreSampleDepth(viewport.depthLimits);')
            if fixed16_depth:statements.append('geometryCheckFixed16Position();')
        return '\n'.join('    '+s for s in statements)

    def varyings(direction):
        if geometry_profile:
            # Adreno 740's vendor linker rejects some valid scalar/Component
            # interfaces when the pixel shader consumes all the live lanes.
            # Keep one vector per Xenos export location instead. Aliases expose
            # ONLY written lanes; holes stay undefined, and expression() above
            # still rejects any pixel read of one. This does not zero-fill or
            # change interpolation, arithmetic, or the compute-export profile.
            declarations=[f'layout(location={s}) '+('centroid ' if mrt and direction=='in' and geometry_centroid_mask&(1<<int(s)) else '')+
                f'{direction} vec4 packedInterpolator{s};'
                for s in sorted(masks,key=int)]
            def value(s):
                if direction!='in':return f'packedInterpolator{s}'
                if mrt:return f'packedInterpolator{s}'
                if geometry_centroid_mask&(1<<int(s)):return f'interpolateAtCentroid(packedInterpolator{s})'
                # Depth is evaluated per sample in ABI13, but guest center
                # interpolators must remain evaluated at the PIXEL center.
                if geometry_color8_format is not None:return f'interpolateAtOffset(packedInterpolator{s},vec2(0.0))'
                return f'packedInterpolator{s}'
            aliases=[f'#define interpolator{s}_{lane} {value(s)}[{lane}]'
                for s in sorted(masks,key=int) for lane in range(4) if masks[s]&(1<<lane)]
            return '\n'.join([*declarations,*aliases])+'\n'
        if scalar_fan_profile:
            return f'layout(location=0) {direction} float interpolator0;\n'
        return '\n'.join(f'layout(location={s}) {direction} vec{components(s)} interpolator{s};' for s in sorted(expected-{'62'}))+'\n'
    indexing='    uint index=uint(gl_VertexIndex);\n' if indexed or direct_triangles else '''
    const uint quad[6]=uint[6](0u,1u,2u,0u,2u,3u);
    uint index=(uint(gl_VertexIndex)/6u)*4u+quad[uint(gl_VertexIndex)%6u];
'''
    if triangle_fan:
        # Caller must issue exactly (original_vertex_count-2)*3 vertices and
        # validate the original count/owned extent. No restart or fan ring guess.
        indexing='    uint corner=uint(gl_VertexIndex)%3u;\n    uint index=corner==0u?0u:uint(gl_VertexIndex)/3u+corner;\n'
    if screen_strip_profile or world_strip_profile:
        # Expand each original strip triangle without changing VS inputs or
        # dropping degenerates. Preserve winding parity. Rotate the last guest
        # vertex to the first native vertex, for last->first provoking selection.
        # The future consumer must check the actual provoking/interpolation mode
        # and issue exactly (original_count-2)*3 vertices; no restart is supported.
        indexing=screen_strip_index_glsl()
    common=COMMON+(GEOMETRY_EXP_GLSL if geometry_profile or world_strip_profile else '')
    viewport_fields='vec4 scaleOffset;'+(' vec2 depthLimits;' if geometry_color8_format is not None or mrt else '')
    vertex=common+(GEOMETRY_FIXED16_VERTEX_GLSL if geometry_gradient_depth_profile else GEOMETRY_VERTEX_DEPTH_GLSL if geometry_color8_format is not None or mrt else '')+'layout(push_constant) uniform Viewport { '+viewport_fields+' } viewport;\n'+varyings('out')+'void main() {\n'+indexing+body(vs,True)+'\n}\n'
    if rectangle_list and not position_compute:
        # Re-run the original VS for all three original vertices. Expand the
        # outputs, NEVER the input bytes (the original VS can be nonlinear).
        # Pinned ReXGlue/Xenia selects the corner opposite the longest edge,
        # then constructs (v1-v0)+v2 with uncontracted arithmetic. Consumers
        # must validate rectangle geometry, complete extent and signed viewport.
        parameters=sorted(expected-{'62'})
        arguments=', '.join(['uint index','out vec4 position']+
            [f'out vec{components(s)} interpolator{s}' for s in parameters])
        vertex=common+(GEOMETRY_FIXED16_VERTEX_GLSL if fixed16_rectangle_depth_profile else '')+'layout(push_constant) uniform Viewport { vec4 scaleOffset; } viewport;\n'+varyings('out')
        vertex+=f'void originalVertex({arguments}) {{\n'+body(vs,True,True)+'\n}\nvoid main() {\n'
        vertex+='    vec4 positions[3];\n'+''.join(f'    vec{components(s)} parameters{s}[3];\n' for s in parameters)
        vertex+='    uint base=(uint(gl_VertexIndex)/6u)*3u;\n'
        vertex+='    for(uint i=0u;i<3u;++i) originalVertex(base+i,positions[i]'+''.join(f',parameters{s}[i]' for s in parameters)+');\n'
        vertex+='''    precise vec3 edgeSquared;
    for(int i=0;i<3;++i) {
        precise vec2 edge=positions[(i+2)%3].xy-positions[(i+1)%3].xy;
        edgeSquared[i]=edge.x*edge.x+edge.y*edge.y;
    }
    int a=(edgeSquared.x>edgeSquared.y && edgeSquared.x>edgeSquared.z)?0:(edgeSquared.y>edgeSquared.z?1:2);
    int b=(a+1)%3,c=(a+2)%3;
    const uint corners[6]=uint[6](0u,1u,2u,2u,1u,3u);
    uint corner=corners[uint(gl_VertexIndex)%6u];
    int chosen=corner==0u?a:corner==1u?b:c;
    precise vec4 syntheticPosition=(positions[b]-positions[a])+positions[c];
    gl_Position=corner==3u?syntheticPosition:positions[chosen];
'''
        for s in parameters:
            vertex+=f'    precise vec{components(s)} synthetic{s}=(parameters{s}[b]-parameters{s}[a])+parameters{s}[c];\n'
            vertex+=f'    interpolator{s}=corner==3u?synthetic{s}:parameters{s}[chosen];\n'
        vertex+='    gl_Position.xy=gl_Position.xy*viewport.scaleOffset.xy+viewport.scaleOffset.zw*gl_Position.w;\n'
        if fixed16_rectangle_depth_profile:vertex+='    geometryCheckFixed16Position();\n'
        vertex+='}\n'
    if position_compute or geometry_position_compute:
        # Raw original homogeneous position, BEFORE native viewport mapping.
        # Full VS/PS analysis still rejects unsupported memory side effects.
        # The indexed geometry profile may contain a validated pixel KILL;
        # that pixel program is not run by this position-only computation.
        # Compute alone is not a draw admission or clipping decision.
        vertex=common+'''layout(local_size_x=64) in;
layout(set=0,binding=1,std430) buffer Result { uint words[]; } result;
void main() {
    uint index=gl_GlobalInvocationID.x;
    uint count=readBE(0u);
    if(count<3u || count>65536u || index>=count ||
       uint(source.words.length())<2052u+count*STRIDEu ||
       uint(result.words.length())<8u+count*4u) return;
    vec4 position;
'''.replace('STRIDE',str(stride))+body(vs,True)+'''
    for(uint lane=0u;lane<4u;++lane)
        result.words[4u+index*4u+lane]=floatBitsToUint(position[lane]);
}
'''
    if geometry_compute:
        # Same packed-fetch and output expression emitter as the raster VS.
        # Observe every WRITTEN export lane before viewport mapping. Holes stay
        # untouched in the output buffer; this is not raster/PS verification or
        # a reason to retire any guest work. Slot16 stores position export62.
        declarations=''.join(f'    float interpolator{s}_{lane};\n'
            for s in sorted(masks,key=int) for lane in range(4) if masks[s]&(1<<lane))
        vertex=common+'''layout(local_size_x=64) in;
layout(set=0,binding=1,std430) buffer Result { uint words[]; } result;
void main() {
    uint index=gl_GlobalInvocationID.x;
    uint count=readBE(0u);
    if(count==0u || count>65536u || index>=count ||
       uint(source.words.length())<2052u+count*STRIDEu ||
       uint(result.words.length())<8u+count*68u) return;
    vec4 position;
'''.replace('STRIDE',str(stride))+declarations+body(vs,True)+'\n'
        for slot in sorted(masks,key=int):
            for lane in range(4):
                if masks[slot]&(1<<lane):
                    vertex+=f'    result.words[4u+index*68u+{int(slot)*4+lane}u]=floatBitsToUint(interpolator{slot}_{lane});\n'
        vertex+='''    for(uint lane=0u;lane<4u;++lane)
        result.words[4u+index*68u+64u+lane]=floatBitsToUint(position[lane]);
}
'''
    samplers=''.join(f'layout(set=0,binding={i+1}) uniform sampler2D'+('Array' if dimensions[slot]=={'cube'} else '')+f' atlas{i};\n' for i,slot in enumerate(slots))
    color_declarations=(''.join(f'layout(location={s}) out vec4 color{s};\n' for s in pixel_outputs) if mrt or rectangle_mrt_profile else 'layout(location=0) out vec4 color;\n')
    # No fragment stage is emitted for a draw that has none. The caller compiles
    # the vertex module alone and builds a pipeline without one, which is what
    # writes stencil and nothing else.
    pixel=None if vertex_only else common+samplers+(CUBE_GLSL+CUBE_SAMPLE_GLSL if cube_profile else '')+(PWL_TEXTURE_GLSL if cube_pwl_rgb else '')+(GEOMETRY_FIXED16_DEPTH_GLSL if fixed16_depth else geometry_mrt_output_glsl(geometry_depth_color_witness) if mrt else GEOMETRY_OUTPUT_GLSL if geometry_color8_format is not None else '')+varyings('in')+color_declarations+'void main() {\n'+body(ps,False)+'\n}\n'
    if screen_strip_profile:
        # This remains an explicit new profile, not a widening of any old ABI.
        # Original shader arithmetic is unchanged; native output-merger policy
        # is separate and requires the pinned state and W1 consumer proof.
        pixel=pixel.replace('void main() {\n',SCREEN_STRIP_RECIPROCAL_GLSL+SCREEN_STRIP_DEPTH_GLSL+SCREEN_STRIP_BLEND_GLSL+'void main() {\n    screenStripStoreDepth();\n',1)
    if world_strip_profile or indexed_rgb7e3_profile:
        # Native perspective interpolation and clipping remain required. The
        # output consumer must set the captured depth viewport (which can be
        # constant); do not reuse ABI28's reversed W1 depth assumption.
        from geometry_output_merger import GEOMETRY_DEPTH_ENCODING_GLSL
        pixel=pixel.replace('void main() {\n',GEOMETRY_DEPTH_ENCODING_GLSL+'void main() {\n',1)
    return vertex,pixel,dict(vertex=vs,pixel=ps,stride=stride*4,texture_slot=fetch['slot'] if fetch else None,
        texture_slots=slots,indexed=indexed,interpolators=len(expected)-1,abi_version=32 if geometry_lod_profile else 31 if scalar_fan_profile else 30 if indexed_rgb7e3_profile else 29 if world_strip_profile else 28 if screen_strip_profile else 27 if post_stencil_profile else 26 if screen_space_profile else 25 if vertex_only else 24 if tonemap_profile else 23 if offset_sample_profile else 22 if rectangle_mrt_profile else 20 if composite_profile else 19 if fixed16_rectangle_depth_profile else 18 if geometry_fixed16_effects_profile else 17 if geometry_gradient_depth_profile else 16 if geometry_depth_color_witness else 15 if geometry_predicate_profile else 14 if mrt else 13 if geometry_color8_format is not None else 12 if geometry_profile else (11 if len(ps['executed_instruction_addresses'])>256 else 10) if predicate_profile else 9 if explicit_lod_profile else 8 if cube_profile or rectangle_list else 7 if color8_source_clamp else 6 if loading_profile else 5 if extended_profile else 4,
        direct_triangles=direct_triangles,experimental_profile=extended_profile,
        affine_position_xy=affine_position_xy(vs) if direct_triangles else None,
        loading_profile=loading_profile,triangle_fan=triangle_fan,quad_list=quad_list,
        scalar_fan_profile=scalar_fan_profile,scalar_fan_query_semantics_verified=False,
        geometry_lod_profile=geometry_lod_profile,geometry_lod_native_sampling_verified=False,
        position_compute=position_compute,color8_source_clamp=color8_source_clamp,
        cube_profile=cube_profile,rectangle_list=rectangle_list,cube_sampling_state_verified=False,
        explicit_lod_profile=explicit_lod_profile,explicit_lod_sampling_verified=False,
        predicate_profile=predicate_profile,predicate_hardware_verified=False,
        vertex_predicate_profile=vertex_predicate_profile,
        vertex_address_profile=vertex_address_profile,
        geometry_predicate_profile=geometry_predicate_profile,geometry_discard_hardware_verified=False,
        geometry_gradient_depth_profile=geometry_gradient_depth_profile,geometry_fixed16_format=4 if fixed16_depth else None,
        geometry_fixed16_effects_profile=geometry_fixed16_effects_profile,
        fixed16_rectangle_depth_profile=fixed16_rectangle_depth_profile,
        geometry_explicit_depth=fixed16_depth,geometry_gradient_hardware_verified=False,
        geometry_parameter_register=geometry_parameter_register,geometry_parameter_coordinates_unscaled=True,
        geometry_blend_controls=geometry_blend_controls,
        geometry_depth_color_witness=geometry_depth_color_witness,geometry_destination_coverage_verified=False,
        cube_pwl_rgb=cube_pwl_rgb,
        cube_filter=cube_filter if cube_profile else None,
        rectangle_extent_validated=False,
        hardware_transcendentals_verified=False,fan_extent_validated=False,
        geometry_profile=geometry_profile,geometry_compute=geometry_compute,geometry_position_compute=geometry_position_compute,
        geometry_color8_format=geometry_color8_format,geometry_depth20e4=geometry_color8_format is not None or mrt or fixed16_depth,
        geometry_sample_depth=geometry_color8_format is not None,
        geometry_mrt_formats=geometry_mrt_formats,geometry_color_outputs=[int(s) for s in pixel_outputs],
        geometry_centroid_mask=geometry_centroid_mask,
        geometry_interpolator_masks=masks if geometry_profile else None,
        geometry_resources_verified=False,geometry_numeric_domain_verified=False,
        **({'indexed_rgb7e3_profile':True,'indexed_rgb7e3_native_state_verified':False,
            'indexed_rgb7e3_physical_publication_verified':False,
            'indexed_rgb7e3_depth_policy':'single-sample-captured-viewport-fragcoord-trunc20e4'} if indexed_rgb7e3_profile else {}),
        **({'vertex_denormal_multiply_profile':True} if vertex_denormal_multiply_profile else {}),
        **({'world_strip_profile':True,'world_strip_native_state_verified':False,
            'world_strip_resources_verified':False,'strip_extent_validated':False,
            'strip_interpolation_verified':False,'strip_provoking_last_required':True,
            'world_strip_depth_policy':'single-sample-captured-viewport-fragcoord-trunc20e4'} if world_strip_profile else {}),
        **({'post_stencil_profile':True,'post_stencil_native_state_verified':False} if post_stencil_profile else {}),
        **({'screen_strip_profile':True,'strip_extent_validated':False,
            'screen_strip_depth_policy':'single-sample-fragcoord-trunc20e4',
            'strip_interpolation_verified':False,'strip_provoking_last_required':True,
            'screen_strip_native_state_verified':False} if screen_strip_profile else {}),
        **({'screen_space_profile':True,'screen_space_parameter_register':0,
            'screen_space_native_state_verified':False} if screen_space_profile else {}))
