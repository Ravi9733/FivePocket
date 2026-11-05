#version 450
#ifdef MULTISAMPLE
layout(binding=0) uniform sampler2DMS sourceImage;
#else
layout(binding=0) uniform sampler2D sourceImage;
#endif
layout(std430,binding=1) buffer Completion { uint words[]; } completion;
layout(push_constant) uniform Selection { uint sampleIndex; uint truncateDepth; } selection;

// Integer expression of the same explicit20e4 truncation policy as the
// independent frexp/ldexp CPU reference. No floating divide or rounding mode.
uint truncateDepth20e4(uint bits) {
    if(bits>=0x38800000u)return (bits-0x38000000u)>>3u;
    uint exponent=bits>>23u;
    if(exponent<93u)return 0u;
    return ((bits&0x7FFFFFu)|0x800000u)>>(116u-exponent);
}
float expandDepth20e4(uint value) {
    if(value==0u)return 0.0;
    uint exponent=value>>20u,mantissa=value&0xFFFFFu;
    if(exponent!=0u)return uintBitsToFloat(((exponent+112u)<<23u)|(mantissa<<3u));
    uint highest=uint(findMSB(mantissa));
    return uintBitsToFloat(((highest+93u)<<23u)|((mantissa<<(23u-highest))&0x7FFFFFu));
}
void main() {
    ivec2 at=ivec2(gl_FragCoord.xy);
#ifdef OMIT_TEST_PIXEL
    if(at==ivec2(0))discard;
#endif
#ifdef MULTISAMPLE
    float depth=texelFetch(sourceImage,at,int(selection.sampleIndex)).r;
#else
    float depth=texelFetch(sourceImage,at,0).r;
#endif
#ifdef INJECT_TEST_NONFINITE
    depth=uintBitsToFloat(0x7FC00000u);
#endif
    if(isnan(depth) || isinf(depth) || depth<0.0 || depth>1.0) {
        atomicOr(completion.words[8],1u);depth=0.0;
    }
    // Normalize signed zero before using a positive-only encoding.
    uint bits=depth==0.0?0u:floatBitsToUint(depth);
    uint encoded=truncateDepth20e4(bits);
    // Mode2 is for a single-coverage GREATER draw against uniform zero only.
    // Native depth testing precedes quantization: a positive value becoming
    // zero would have passed natively but failed the quantized comparison.
    // Reject the entire unpublished result instead of keeping incorrect color.
    if(selection.truncateDepth==2u && depth>0.0 && encoded==0u)atomicOr(completion.words[8],2u);
    gl_FragDepth=selection.truncateDepth==0u?depth:expandDepth20e4(encoded);
    atomicAdd(completion.words[4u+selection.sampleIndex],1u);
}
