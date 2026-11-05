"""Explicit native replacement output policy; not Xbox raster conformance.

Uses the same TRUNC 20e4 and PWL gamma policies as the existing sample-transfer
paths, but applies them to EACH fragment, before the native depth test/store.
ABI13 binding5 is a guarded completion SSBO owned by the attachment renderer.
"""

GEOMETRY_VERTEX_DEPTH_GLSL = '''
layout(set=0,binding=5,std430) buffer GeometryCompletion { uint words[]; } geometryCompletion;
layout(location=16) noperspective out float geometryNdcZ;
void geometryStoreSampleDepth(vec2 limits) {
    // Zero W needs a separate homogeneous clipping implementation. Never
    // silently publish a draw with an undefined depth varying.
    bool constantDepth=limits.x==limits.y;
    bool invalid=any(isnan(gl_Position)) || any(isinf(gl_Position)) || (!constantDepth && gl_Position.w==0.0);
    // A constant viewport depth needs no Z/W. Keep the original homogeneous
    // vertex (including W=0) for native clipping and color interpolation.
    precise float ndc=(invalid || constantDepth)?0.0:gl_Position.z/gl_Position.w;
    if(invalid || isnan(ndc) || isinf(ndc)) {
        atomicOr(geometryCompletion.words[4],4u);
        gl_Position=vec4(0.0,0.0,-1.0,1.0);geometryNdcZ=0.0;
    } else geometryNdcZ=ndc;
}
'''

GEOMETRY_DEPTH_ENCODING_GLSL = '''
uint geometryDepth20e4(uint bits) {
    if(bits>=0x38800000u)return (bits-0x38000000u)>>3u;
    uint exponent=bits>>23u;
    if(exponent<93u)return 0u;
    return ((bits&0x7FFFFFu)|0x800000u)>>(116u-exponent);
}
float geometryExpand20e4(uint value) {
    if(value==0u)return 0.0;
    uint exponent=value>>20u,mantissa=value&0xFFFFFu;
    if(exponent!=0u)return uintBitsToFloat(((exponent+112u)<<23u)|(mantissa<<3u));
    uint highest=uint(findMSB(mantissa));
    return uintBitsToFloat(((highest+93u)<<23u)|((mantissa<<(23u-highest))&0x7FFFFFu));
}
'''

GEOMETRY_OUTPUT_PREFIX_GLSL = '''
layout(set=0,binding=5,std430) buffer GeometryCompletion { uint words[]; } geometryCompletion;
layout(location=16) noperspective in float geometryNdcZ;
layout(push_constant) uniform GeometryDepthRange { layout(offset=16) vec2 limits; } geometryDepthRange;
'''+GEOMETRY_DEPTH_ENCODING_GLSL+'''
float geometryEncodeGamma(float value) {
    float scale=value<64.0/1023.0?1023.0:value<128.0/1023.0?1023.0/2.0:value<512.0/1023.0?1023.0/4.0:1023.0/8.0;
    float offset=value<64.0/1023.0?0.0:value<128.0/1023.0?32.0:value<512.0/1023.0?64.0:128.0;
    precise float scaled=value*scale;
    return (trunc(scaled)+offset)/255.0;
}
'''

GEOMETRY_FIXED16_VERTEX_GLSL = '''
layout(set=0,binding=5,std430) buffer GeometryCompletion { uint words[]; } geometryCompletion;
void geometryCheckFixed16Position() {
    if(any(isnan(gl_Position)) || any(isinf(gl_Position))) {
        atomicOr(geometryCompletion.words[4],4u);
        gl_Position=vec4(0.0,0.0,-1.0,1.0);
    }
}
'''

# Separate ABI17: single-sample RG signed fixed16 with explicit shader depth.
# Original clear packing uses shifted float32 rounding, which must NOT be
# algebraically replaced with roundEven(value*32767). Preserve its operation
# order; native SNORM conversion of the representable result needs GPU tests.
GEOMETRY_FIXED16_DEPTH_GLSL = '''
layout(set=0,binding=5,std430) buffer GeometryCompletion { uint words[]; } geometryCompletion;
'''+GEOMETRY_DEPTH_ENCODING_GLSL+'''
void geometryStoreFixed16ExplicitDepth(inout vec4 value, float depth) {
    if(any(isnan(value)) || any(isinf(value))) {
        atomicOr(geometryCompletion.words[4],1u);discard;
    }
    if(isnan(depth) || isinf(depth) || depth<0.0 || depth>1.0) {
        atomicOr(geometryCompletion.words[4],2u);discard;
    }
    precise vec2 scaled=value.xy*(1.0/32.0);
    vec2 bounded=clamp(scaled,vec2(-1.0),vec2(1.0));
    precise vec2 product=bounded*32767.0;
    precise vec2 shifted=product+32768.0;
    precise vec2 signedInteger=roundEven(shifted)-32768.0;
    value.xy=signedInteger/32767.0;
    gl_FragDepth=geometryExpand20e4(geometryDepth20e4(floatBitsToUint(depth==0.0?0.0:depth)));
    atomicAdd(geometryCompletion.words[5],1u);
}
'''

GEOMETRY_OUTPUT_GLSL = GEOMETRY_OUTPUT_PREFIX_GLSL+'''void geometryStoreColor8Depth20e4(inout vec4 value, uint format) {
    // FragCoord.z may still be pixel-center depth under sample shading.
    // Explicit sample interpolation of screen-linear NDC depth preserves the
    // distinct samples. Color interpolators retain their guest center/centroid.
    if(gl_SampleID<0 || any(isnan(value)) || any(isinf(value))) {
        atomicOr(geometryCompletion.words[4],1u);discard;
    }
    float ndc=interpolateAtSample(geometryNdcZ,gl_SampleID);
    if(isnan(ndc) || isinf(ndc)) { atomicOr(geometryCompletion.words[4],2u);discard; }
    precise float scale=geometryDepthRange.limits.y-geometryDepthRange.limits.x;
    precise float z=clamp(ndc,0.0,1.0)*scale+geometryDepthRange.limits.x;
    if(isnan(z) || isinf(z) || z<0.0 || z>1.0) {
        atomicOr(geometryCompletion.words[4],2u);discard;
    }
    gl_FragDepth=geometryExpand20e4(geometryDepth20e4(floatBitsToUint(z==0.0?0.0:z)));
    value=clamp(value,vec4(0.0),vec4(1.0));
    precise vec4 scaled=value*255.0;
    vec4 encoded=roundEven(scaled)/255.0;
    if(format==1u)encoded.rgb=vec3(geometryEncodeGamma(value.r),geometryEncodeGamma(value.g),geometryEncodeGamma(value.b));
    value=encoded;
    atomicAdd(geometryCompletion.words[5],1u);
}
'''

# Separate ABI14 single-sample MRT policy. Do not call the ABI13 sample-depth
# function once for every attachment: that would repeat depth/receipt effects
# and require sample-rate shading even for the actual single-sample targets.
GEOMETRY_MRT_OUTPUT_GLSL = GEOMETRY_OUTPUT_PREFIX_GLSL+'''
void geometryClampMrtBlendSource(inout vec4 value) {
    if(any(isnan(value)) || any(isinf(value))) {
        atomicOr(geometryCompletion.words[4],1u);discard;
    }
    // Linear UNORM: clamp source BEFORE blend, quantize native destination
    // AFTER blend. Rounding source to UNORM8 first changes the blend equation.
    value=clamp(value,vec4(0.0),vec4(1.0));
}
void geometryStoreMrtColor8(inout vec4 value, uint format) {
    if(any(isnan(value)) || any(isinf(value))) {
        atomicOr(geometryCompletion.words[4],1u);discard;
    }
    value=clamp(value,vec4(0.0),vec4(1.0));
    precise vec4 scaled=value*255.0;
    vec4 encoded=roundEven(scaled)/255.0;
    if(format==1u)encoded.rgb=vec3(geometryEncodeGamma(value.r),geometryEncodeGamma(value.g),geometryEncodeGamma(value.b));
    value=encoded;
}
void geometryStoreMrtDepth20e4() {
    // Exactly one sample: the ordinary screen-linear varying is evaluated at
    // that sample. No gl_SampleID / interpolateAtSample capability is needed.
    float ndc=geometryNdcZ;
    if(isnan(ndc) || isinf(ndc)) { atomicOr(geometryCompletion.words[4],2u);discard; }
    precise float scale=geometryDepthRange.limits.y-geometryDepthRange.limits.x;
    precise float z=clamp(ndc,0.0,1.0)*scale+geometryDepthRange.limits.x;
    if(isnan(z) || isinf(z) || z<0.0 || z>1.0) {
        atomicOr(geometryCompletion.words[4],2u);discard;
    }
    gl_FragDepth=geometryExpand20e4(geometryDepth20e4(floatBitsToUint(z==0.0?0.0:z)));
    atomicAdd(geometryCompletion.words[5],1u);
}
'''

# ABI16 extends the existing push block (one block per stage). Snapshot depth
# words begin after the 16-word guarded completion header. The native consumer
# must prove depth!=0 implies valid color, and restrict late tests to GE/no
# stencil/no depth writes. This is a validity check, not a guest color clear.
GEOMETRY_DEPTH_COLOR_WITNESS_GLSL = '''
void geometryCheckDestinationWitness() {
    if(geometryDepthRange.witnessRequired==0u)return;
    ivec2 pixel=ivec2(floor(gl_FragCoord.xy))-ivec2(geometryDepthRange.witnessOrigin);
    uvec2 extent=geometryDepthRange.witnessExtent;
    if(any(lessThan(pixel,ivec2(0))) || any(greaterThanEqual(uvec2(pixel),extent))) {
        atomicOr(geometryCompletion.words[4],8u);discard;
    }
    uint at=16u+uint(pixel.y)*extent.x+uint(pixel.x);
    float previous=uintBitsToFloat(geometryCompletion.words[at]);
    // A zero may have been drawn, but this witness does not prove it. Fail
    // closed; never replace unknown color with invented zeros.
    if(isnan(previous) || isinf(previous) || previous<=0.0 || previous>1.0) {
        atomicOr(geometryCompletion.words[4],8u);discard;
    }
}
'''

def geometry_mrt_output_glsl(depth_color_witness=False):
    if not depth_color_witness:
        return GEOMETRY_MRT_OUTPUT_GLSL
    return GEOMETRY_MRT_OUTPUT_GLSL.replace(
        'layout(offset=16) vec2 limits;',
        'layout(offset=16) vec2 limits; layout(offset=24) uvec2 witnessOrigin; '
        'layout(offset=32) uvec2 witnessExtent; '
        'layout(offset=40) uint witnessRequired;')+GEOMETRY_DEPTH_COLOR_WITNESS_GLSL
