// Package depth-float (diag-df-float): generic_depth_fused_v376.hlsl (a depth resolve and its depth word in one dispatch) with
// the R32F value of a k_24_8_FLOAT target (floatDepth != 0) rounded to 20e4 (xenos_depth_df.h quantizeXenosFloat20e4: the value
// the 360's fetch of the resolved k_24_8_FLOAT returns); k_24_8 targets (floatDepth 0) keep the unrounded depth as v376 does.
// Bindings, push constants (32 bytes) and the word computation are v376's (packXenosDepthWord encodes the same 20e4 code the R32F
// value decodes). Compiled with the local DXC (local-analysis/host-tools/dxc-build, DXC 1.9):
//   dxc -spirv -T cs_6_0 -E main -fspv-target-env=vulkan1.1 -O3 generic_depth_df_fused.hlsl
// then written as generic_depth_df_fused.inc by tools/gta5_depth/spirv_inc.py.
[[vk::combinedImageSampler]][[vk::binding(0,0)]] Texture2D<float> depthTexture;
[[vk::combinedImageSampler]][[vk::binding(0,0)]] SamplerState depthSampler;
[[vk::binding(1,0)]][[vk::image_format("rgba8")]] RWTexture2D<float4> destination;
[[vk::combinedImageSampler]][[vk::binding(2,0)]] Texture2D<uint> stencilTexture;
[[vk::combinedImageSampler]][[vk::binding(2,0)]] SamplerState stencilSampler;
[[vk::binding(3,0)]][[vk::image_format("r32f")]] RWTexture2D<float4> resolved; // (float4: the store glslc makes of the v19 shader's vec4)
struct Region { int2 source; int2 target; int2 size; uint floatDepth; uint unused; };
[[vk::push_constant]] Region region;
#include "xenos_depth_word.h"
#include "xenos_depth_df.h"
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const int2 p = int2(id.xy);
    if (p.x >= region.size.x || p.y >= region.size.y) return;
    const float depth = depthTexture.Load(int3(p + region.source, 0));
    const uint stencil = stencilTexture.Load(int3(p + region.source, 0));
    const float value = region.floatDepth != 0u ? quantizeXenosFloat20e4(depth) : depth;
    resolved[p + region.target] = float4(value, value, value, value);
    const uint word = packXenosDepthWord(depth, stencil, region.floatDepth);
    destination[p + region.target] = float4(word & 0xFFu, (word >> 8) & 0xFFu, (word >> 16) & 0xFFu, word >> 24) / 255.0;
}
