// Attempt v60 generic renderer: a depth resolve's memory word as a k_8_8_8_8 texture (xenos_depth_word.h):
// R = bits 0-7 (stencil), G/B/A = bits 8-31 (20e4 or unorm24 depth), one texel per resolved pixel, for
// fetches that read a depth resolve destination as colour. Compiled with the local DXC:
//   dxc -spirv -T cs_6_0 -E main -fspv-target-env=vulkan1.1 -O3 generic_depth_word.hlsl
// then written as generic_depth_word.inc (the SPIR-V words) by tools/spirv_inc.py.
[[vk::combinedImageSampler]][[vk::binding(0,0)]] Texture2D<float> depthTexture;
[[vk::combinedImageSampler]][[vk::binding(0,0)]] SamplerState depthSampler;
[[vk::binding(1,0)]][[vk::image_format("rgba8")]] RWTexture2D<float4> destination;
[[vk::combinedImageSampler]][[vk::binding(2,0)]] Texture2D<uint> stencilTexture;
[[vk::combinedImageSampler]][[vk::binding(2,0)]] SamplerState stencilSampler;
struct Region { int2 source; int2 target; int2 size; uint floatDepth; uint unused; };
[[vk::push_constant]] Region region;
#include "xenos_depth_word.h"
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const int2 p = int2(id.xy);
    if (p.x >= region.size.x || p.y >= region.size.y) return;
    const float depth = depthTexture.Load(int3(p + region.source, 0));
    const uint stencil = stencilTexture.Load(int3(p + region.source, 0));
    const uint word = packXenosDepthWord(depth, stencil, region.floatDepth);
    destination[p + region.target] = float4(word & 0xFFu, (word >> 8) & 0xFFu, (word >> 16) & 0xFFu, word >> 24) / 255.0;
}
