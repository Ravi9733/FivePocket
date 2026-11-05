// Package depth-float (diag-df-float): a depth resolve of a k_24_8_FLOAT target into its R32F image, the value the 360's
// texture fetch of the resolved k_24_8_FLOAT returns: the stored depth rounded to 20e4 (xenos_depth_df.h quantizeXenosFloat20e4,
// round to nearest even; the target is D32_SFLOAT_S8_UINT while diag-df-float is on). generic_depth_resolve.comp (v19) with the
// quantization: same bindings (0 the depth aspect, 1 the R32F storage image), push constants (24 bytes) and grid. Compiled with
// the local DXC (local-analysis/host-tools/dxc-build, DXC 1.9):
//   dxc -spirv -T cs_6_0 -E main -fspv-target-env=vulkan1.1 -O3 generic_depth_df_resolve.hlsl
// then written as generic_depth_df_resolve.inc by tools/gta5_depth/spirv_inc.py.
[[vk::combinedImageSampler]][[vk::binding(0,0)]] Texture2D<float> depthTexture;
[[vk::combinedImageSampler]][[vk::binding(0,0)]] SamplerState depthSampler;
[[vk::binding(1,0)]][[vk::image_format("r32f")]] RWTexture2D<float4> destination;
struct Region { int2 source; int2 target; int2 size; };
[[vk::push_constant]] Region region;
#include "xenos_depth_word.h"
#include "xenos_depth_df.h"
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const int2 p = int2(id.xy);
    if (p.x >= region.size.x || p.y >= region.size.y) return;
    const float q = quantizeXenosFloat20e4(depthTexture.Load(int3(p + region.source, 0)));
    destination[p + region.target] = float4(q, q, q, q);
}
