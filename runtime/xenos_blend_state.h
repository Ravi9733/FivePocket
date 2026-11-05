#pragma once
// Attempt v85: RB_BLENDCONTROL0-3 (0x2201, 0x2209, 0x220A, 0x220B) and RB_COLOR_MASK (0x2104) as the generic
// renderer maps them to one Vulkan colour attachment (pure; host-tested against hand-computed values).
//   bits 0-4 color_srcblend, 5-7 color_comb_fcn, 8-12 color_destblend,
//   bits 16-20 alpha_srcblend, 21-23 alpha_comb_fcn, 24-28 alpha_destblend.
// Factors: 0 ZERO, 1 ONE, 4 SRC_COLOR, 5 1-SRC_COLOR, 6 SRC_ALPHA, 7 1-SRC_ALPHA, 8 DST_COLOR, 9 1-DST_COLOR,
// 10 DST_ALPHA, 11 1-DST_ALPHA, 12 CONSTANT_COLOR, 13 1-CONSTANT_COLOR, 14 CONSTANT_ALPHA, 15 1-CONSTANT_ALPHA,
// 16 SRC_ALPHA_SATURATE (2, 3, 17-31 undefined). Ops: 0 ADD, 1 SUBTRACT, 2 MIN, 3 MAX, 4 REVERSE_SUBTRACT.
// A colour factor in the alpha equation means its alpha (Vulkan defines SRC_COLOR's alpha as As, as the
// Xenos does). RB_COLOR_MASK: 4 bits per target, R G B A from bit 0 (Vulkan's R/G/B/A_BIT order).
// Mapping identical to v71 for every value; the flags only report what v71 would silently map:
//   UnknownFactor  2, 3 or 17-31 (mapped to ONE, as v71)
//   UnknownOp      5-7 (mapped to ADD)
//   MinMaxFactors  MIN/MAX with a factor other than ONE: the Xenos applies the factors (Xenia), Vulkan
//                  ignores them for MIN/MAX
#include <vulkan/vulkan.h>
#include <cstdint>

namespace recomp::gpu::generic::state {

enum : unsigned {BlendUnknownFactor=1,BlendUnknownOp=2,BlendMinMaxFactors=4};
inline VkBlendFactor xenosBlendFactor(unsigned x,unsigned* flags=nullptr) {
    switch(x) {
    case 0: return VK_BLEND_FACTOR_ZERO;case 1: return VK_BLEND_FACTOR_ONE;
    case 4: return VK_BLEND_FACTOR_SRC_COLOR;case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6: return VK_BLEND_FACTOR_SRC_ALPHA;case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_COLOR;case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_DST_ALPHA;case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: if(flags)*flags|=BlendUnknownFactor;return VK_BLEND_FACTOR_ONE;
    }
}
inline VkBlendOp xenosBlendOp(unsigned x,unsigned* flags=nullptr) {
    switch(x) {
    case 0: return VK_BLEND_OP_ADD;case 1: return VK_BLEND_OP_SUBTRACT;case 2: return VK_BLEND_OP_MIN;case 3: return VK_BLEND_OP_MAX;
    case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: if(flags)*flags|=BlendUnknownOp;return VK_BLEND_OP_ADD;
    }
}
struct BlendAttachment {VkPipelineColorBlendAttachmentState state{};unsigned flags=0;};
// One target: its RB_BLENDCONTROLi word and RB_COLOR_MASK nibble. Blending is off when both equations are
// ONE*src + ZERO*dst (the Xenos has no enable bit; v71's rule).
inline BlendAttachment blendAttachment(std::uint32_t bc,unsigned writeMask) {
    BlendAttachment r;auto& a=r.state;
    a.colorWriteMask=writeMask&15;
    a.srcColorBlendFactor=xenosBlendFactor(bc&31,&r.flags);a.colorBlendOp=xenosBlendOp((bc>>5)&7,&r.flags);
    a.dstColorBlendFactor=xenosBlendFactor((bc>>8)&31,&r.flags);
    a.srcAlphaBlendFactor=xenosBlendFactor((bc>>16)&31,&r.flags);a.alphaBlendOp=xenosBlendOp((bc>>21)&7,&r.flags);
    a.dstAlphaBlendFactor=xenosBlendFactor((bc>>24)&31,&r.flags);
    a.blendEnable=!(a.srcColorBlendFactor==VK_BLEND_FACTOR_ONE && a.dstColorBlendFactor==VK_BLEND_FACTOR_ZERO && a.colorBlendOp==VK_BLEND_OP_ADD &&
                    a.srcAlphaBlendFactor==VK_BLEND_FACTOR_ONE && a.dstAlphaBlendFactor==VK_BLEND_FACTOR_ZERO && a.alphaBlendOp==VK_BLEND_OP_ADD);
    const auto minMax=[](VkBlendOp o){return o==VK_BLEND_OP_MIN || o==VK_BLEND_OP_MAX;};
    if(a.blendEnable && ((minMax(a.colorBlendOp) && (a.srcColorBlendFactor!=VK_BLEND_FACTOR_ONE || a.dstColorBlendFactor!=VK_BLEND_FACTOR_ONE)) ||
                         (minMax(a.alphaBlendOp) && (a.srcAlphaBlendFactor!=VK_BLEND_FACTOR_ONE || a.dstAlphaBlendFactor!=VK_BLEND_FACTOR_ONE))))
        r.flags|=BlendMinMaxFactors;
    return r;
}
// The 32-bit float colour formats are the only ones colorFormat() produces whose blending Vulkan does not
// require (R8G8B8A8_UNORM, A2B10G10R10_UNORM_PACK32, R16G16_SFLOAT, R16G16B16A16_SFLOAT must blend).
inline bool blendSupportOptional(VkFormat f){return f==VK_FORMAT_R32_SFLOAT || f==VK_FORMAT_R32G32_SFLOAT;}

} // namespace recomp::gpu::generic::state
