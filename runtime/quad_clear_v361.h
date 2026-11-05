#pragma once
// Attempt v361 (exact): helpers for drawing a colour attachment's clear rectangles as one instanced quad draw
// (clear_quad_v361.vert/.frag, clear_quad_v361_spirv.h) instead of vkCmdClearAttachments.
//
// Value rule (exactValue): vkCmdClearAttachments converts the float clear value to the attachment format, and a fragment shader
// output of the same float is converted by the colour output path; the two conversions are only certain to give the same bits
// when no rounding, clamping, flushing or NaN canonicalisation is involved:
//  - UNORM components (R8G8B8A8_UNORM, A2B10G10R10_UNORM_PACK32): only +0.0 and 1.0 (the spec's float -> normalized fixed-point
//    rule requires exactly 0 and 2^b - 1 for them in every implementation; any other value may round either way);
//  - 16-bit float components (R16G16B16A16_SFLOAT, R16G16_SFLOAT): +0.0 or a value that is a normal half exactly (biased f32
//    exponent 113..142, the low 13 mantissa bits zero); half denormals (may be flushed), -0, Inf and NaN are not taken;
//  - 32-bit float components (R32_SFLOAT, R32G32_SFLOAT): +0.0 or a normal float (stored as is; f32 denormals may be flushed);
//  - every other format: never (the renderer's alias-clear targets use only the formats above).
// Only the components the format stores are tested (the others are dropped by both paths).
//
// Coverage rule (ndc): rectangle edges are integer pixel coordinates p (0 <= p <= 4096) encoded as p / 2048 - 1 on a 4096 x 4096
// viewport at the origin (the renderer's v157 screen-space viewport): p * 2^-11 and the subtraction are exact in float, and the
// viewport transform 2048 * ndc + 2048 gives p back exactly. Every pixel centre lies 0.5 from every rectangle edge, so the covered
// pixels are exactly those of the rectangle; the strip's two triangles share their diagonal's endpoints, so the rasterization
// rules cover a centre on the diagonal exactly once (no blending: the count would not matter either).
#include <cstdint>
#include <cstring>
#include <utility>
#include <vulkan/vulkan.h>
namespace recomp::gpu::v361 {
constexpr float Extent=4096.0f;           // the viewport (width and height) the quads are drawn on
constexpr std::uint32_t InstanceBytes=32;  // per rectangle: vec4 rect (NDC x0, y0, x1, y1), vec4 value
struct FormatClass {unsigned components=0,kind=0;}; // kind 0 none, 1 UNORM, 2 half, 3 float
inline FormatClass formatClass(VkFormat f) {
    switch(f) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return {4,1};
    case VK_FORMAT_R16G16B16A16_SFLOAT: return {4,2};
    case VK_FORMAT_R16G16_SFLOAT: return {2,2};
    case VK_FORMAT_R32_SFLOAT: return {1,3};
    case VK_FORMAT_R32G32_SFLOAT: return {2,3};
    default: return {};
    }
}
inline bool exactComponent(unsigned kind,float x) {
    std::uint32_t b;std::memcpy(&b,&x,4);
    if(b==0)return true;                                  // +0.0: zero bits in every format, by either path
#if defined(RECOMP_V361_MUTANT_HALF_ROUNDS) // test mutant: halves that need rounding taken as exact (exact_test must fail)
    if(kind==2){const unsigned e=(b>>23)&0xFF;return e>=113 && e<=142;}
#endif
    if(kind==1)return b==0x3F800000u;                     // UNORM: 1.0 only
    const unsigned e=(b>>23)&0xFFu;const std::uint32_t m=b&0x7FFFFFu;
    if(e==0 || e==0xFF)return false;                      // -0.0, f32 denormals, Inf, NaN
    if(kind==3)return true;                               // a normal f32 into a 32-bit float component
    if(kind==2)return e>=113 && e<=142 && !(m&0x1FFFu);   // a normal half exactly: 2^-14 <= |x| < 2^16, 10 mantissa bits
    return false;
}
inline bool exactValue(VkFormat f,const float v[4]) {
    const auto c=formatClass(f);
    if(!c.kind)return false;
    for(unsigned k=0;k<c.components;++k)if(!exactComponent(c.kind,v[k]))return false;
    return true;
}
inline float ndc(std::int32_t p){return float(p)*(1.0f/2048.0f)-1.0f;}
// One instance: the rectangle's NDC corners and the clear value (bit copy).
inline void writeInstance(void* out,const VkClearRect& r,const float v[4]) {
    float q[8];
    q[0]=ndc(r.rect.offset.x);q[1]=ndc(r.rect.offset.y);
    q[2]=ndc(r.rect.offset.x+std::int32_t(r.rect.extent.width));q[3]=ndc(r.rect.offset.y+std::int32_t(r.rect.extent.height));
    std::memcpy(q+4,v,16);
#if defined(RECOMP_V361_MUTANT_VALUE) // test mutant: red and green swapped (the harness scenes must differ)
    std::swap(q[4],q[5]);
#endif
#if defined(RECOMP_V361_MUTANT_EDGE) // test mutant: every right edge one pixel short (the harness scenes must differ)
    if(r.rect.extent.width>1)q[2]=ndc(r.rect.offset.x+std::int32_t(r.rect.extent.width)-1);
#endif
    std::memcpy(out,q,sizeof q);
}
}
