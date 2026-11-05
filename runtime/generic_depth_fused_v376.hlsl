// Attempt v376 generic renderer (diag-v376-fused-depth): a depth resolve and its depth word in ONE dispatch. Per texel of
// the region it does what generic_depth_resolve.comp (the R32F depth, binding 3 here) and generic_depth_word.hlsl (the
// k_8_8_8_8 word: bindings 0-2 and the push constants as there) do, reading the depth and stencil texels once. This file is
// generic_depth_word.hlsl plus the `resolved` binding and its store, so the word is computed by the v60 shader's
// instructions (attempts/v376/tests/run_v376.sh checks the SPIR-V: structurally against both shaders, and by running all
// three on a SPIR-V interpreter). Compiled with the local DXC (local-analysis/host-tools/dxc-build, DXC 1.9):
//   dxc -spirv -T cs_6_0 -E main -fspv-target-env=vulkan1.1 -O3 generic_depth_fused_v376.hlsl
// then written as generic_depth_fused_v376.inc by local-analysis/attempts/v60/tools/spirv_inc.py.
[[vk::combinedImageSampler]][[vk::binding(0,0)]] Texture2D<float> depthTexture;
[[vk::combinedImageSampler]][[vk::binding(0,0)]] SamplerState depthSampler;
[[vk::binding(1,0)]][[vk::image_format("rgba8")]] RWTexture2D<float4> destination;
[[vk::combinedImageSampler]][[vk::binding(2,0)]] Texture2D<uint> stencilTexture;
[[vk::combinedImageSampler]][[vk::binding(2,0)]] SamplerState stencilSampler;
[[vk::binding(3,0)]][[vk::image_format("r32f")]] RWTexture2D<float4> resolved; // (float4: the store glslc makes of the v19 shader's vec4)
struct Region { int2 source; int2 target; int2 size; uint floatDepth; uint unused; };
[[vk::push_constant]] Region region;
#include "xenos_depth_word.h"
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const int2 p = int2(id.xy);
    if (p.x >= region.size.x || p.y >= region.size.y) return;
    const float depth = depthTexture.Load(int3(p + region.source, 0));
    const uint stencil = stencilTexture.Load(int3(p + region.source, 0));
    resolved[p + region.target] = float4(depth, depth, depth, depth);
    const uint word = packXenosDepthWord(depth, stencil, region.floatDepth);
    destination[p + region.target] = float4(word & 0xFFu, (word >> 8) & 0xFFu, (word >> 16) & 0xFFu, word >> 24) / 255.0;
}
