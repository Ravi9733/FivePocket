#version 450
#ifdef MULTISAMPLE
layout(binding=0) uniform sampler2DMS sourceImage;
#else
layout(binding=0) uniform sampler2D sourceImage;
#endif
layout(std430,binding=1) buffer Completion { uint words[]; } completion;
layout(push_constant) uniform Selection { uint sampleIndex; uint gammaFormat; } selection;
layout(location=0) out vec4 color;

float decodeUnorm8(uint value) {
    if(value==0u)return 0.0;
    if(value==255u)return 1.0;
    uint k=uint(findMSB(value));
    uint bits=((k+118u)<<23u)+(value<<(23u-k))+(value<<(15u-k))+(value<<(7u-k))+1u;
    return uintBitsToFloat(bits);
}

float decodeGamma(uint byteValue) {
    uint value=byteValue;
    if(byteValue>=192u){value=byteValue*8u-1024u;value+=value/128u;}
    else if(byteValue>=96u){value=byteValue*4u-256u;value+=value/256u;}
    else if(byteValue>=64u){value=byteValue*2u-64u;value+=value/512u;}
    // Adreno's ordinary floating divide loses one ULP for some bytes (e.g.17),
    // causing a full-byte loss on TRUNC re-encode. Construct the correctly
    // rounded binary32 representation of the repeating 10-bit fraction using
    // integers. Matches the independent CPU division for all1024 numerators.
    if(value==0u)return 0.0;
    if(value==1023u)return 1.0;
    uint k=uint(findMSB(value));
    uint significand=(value<<(23u-k))|(value<<(13u-k));
    if(k<=3u)significand|=value<<(3u-k);else significand|=value>>(k-3u);
    if(k>=4u && ((value>>(k-4u))&1u)!=0u)++significand;
    return uintBitsToFloat(((k+116u)<<23u)+significand);
}
float encodeGamma(float value) {
    float scale=value<64.0/1023.0?1023.0:value<128.0/1023.0?1023.0/2.0:value<512.0/1023.0?1023.0/4.0:1023.0/8.0;
    float offset=value<64.0/1023.0?0.0:value<128.0/1023.0?32.0:value<512.0/1023.0?64.0:128.0;
    precise float scaled=value*scale;
    return (trunc(scaled)+offset)/255.0;
}
void main() {
    ivec2 at=ivec2(gl_FragCoord.xy);
#ifdef OMIT_TEST_PIXEL
    if(at==ivec2(0))discard; // Deliberate negative-fixture module only.
#endif
#ifdef MULTISAMPLE
    vec4 source=texelFetch(sourceImage,at,int(selection.sampleIndex));
#else
    vec4 source=texelFetch(sourceImage,at,0);
#endif
#ifdef INJECT_TEST_NONFINITE
    source.r=uintBitsToFloat(0x7FC00000u);
#endif
    if(any(isnan(source)) || any(isinf(source))) {
        atomicOr(completion.words[8],1u);source=vec4(0);
    }
#ifdef DECODE_COLOR8
    uvec4 bytes=uvec4(roundEven(source*255.0));
    color=vec4(decodeUnorm8(bytes.r),decodeUnorm8(bytes.g),decodeUnorm8(bytes.b),decodeUnorm8(bytes.a));
    if(selection.gammaFormat!=0u)color.rgb=vec3(decodeGamma(bytes.r),decodeGamma(bytes.g),decodeGamma(bytes.b));
#else
    source=clamp(source,vec4(0),vec4(1));
    precise vec4 scaled=source*255.0;
    color=roundEven(scaled)/255.0;
    if(selection.gammaFormat!=0u)color.rgb=vec3(encodeGamma(source.r),encodeGamma(source.g),encodeGamma(source.b));
#endif
    atomicAdd(completion.words[4u+selection.sampleIndex],1u);
}
