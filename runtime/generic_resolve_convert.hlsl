// Attempt v48 generic renderer: a colour resolve that converts instead of copying (Xenos resolve
// semantics the plain image copy cannot express). Compiled with the local DXC:
//   dxc -spirv -T cs_6_0 -E main -fspv-target-env=vulkan1.1 -O3 -Fh generic_resolve_convert.h generic_resolve_convert.hlsl
// then turned into generic_resolve_convert.inc (the SPIR-V words) by tools in the attempt (see
// generic_renderer.cpp). Modes: 0 value copy; 1 the EDRAM holds k_2_10_10_10_FLOAT (7e3, kept as half
// floats here) and the resolve reads it as fixed k_2_10_10_10: the destination gets the 7e3 bit
// fields as unorm (GTA V's HDR resolves; postfx decodes exponent/mantissa with floor/frac/exp2);
// 2 the reverse (unorm 2_10_10_10 bits read as 7e3); v75: 3 = mode 1 with the code unquantized
// (generic_hdr7e3.hlsli: encode7e3Continuous; the game's floor/frac decode reads half precision back). Every mode multiplies by `scale`: the resolve's
// exponent bias times the destination fetch's exponent adjust (sampling does not apply it to
// resolved images here).
[[vk::combinedImageSampler]][[vk::binding(0,0)]] Texture2D<float4> source;
[[vk::combinedImageSampler]][[vk::binding(0,0)]] SamplerState sourceSampler;
[[vk::binding(1,0)]][[vk::image_format("rgba16f")]] RWTexture2D<float4> destination;
struct Region { int2 source; int2 target; int2 size; uint mode; float scale; };
[[vk::push_constant]] Region region;

#include "generic_hdr7e3.hlsli" // v75: encode7e3 (mode 1) and encode7e3Continuous (mode 3)
float decode7e3(uint c) {
    const uint e = c >> 7, m = c & 127u;
    return e == 0 ? float(m) / 512.0 : (1.0 + float(m) / 128.0) * exp2(float(e) - 3.0);
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const int2 p = int2(id.xy);
    if (p.x >= region.size.x || p.y >= region.size.y) return;
    float4 v = source.Load(int3(p + region.source, 0));
    if (region.mode == 1) {
        v = float4(encode7e3(v.r) / 1023.0, encode7e3(v.g) / 1023.0, encode7e3(v.b) / 1023.0, round(saturate(v.a) * 3.0) / 3.0);
    } else if (region.mode == 3) { // v75: mode 1 without quantizing the code (half-float precision survives)
        v = float4(encode7e3Continuous(v.r) / 1023.0, encode7e3Continuous(v.g) / 1023.0, encode7e3Continuous(v.b) / 1023.0,
                   round(saturate(v.a) * 3.0) / 3.0);
    } else if (region.mode == 2) {
        const uint3 c = (uint3)round(saturate(v.rgb) * 1023.0);
        v = float4(decode7e3(c.r), decode7e3(c.g), decode7e3(c.b), round(saturate(v.a) * 3.0) / 3.0);
    }
    destination[p + region.target] = v * region.scale;
}
