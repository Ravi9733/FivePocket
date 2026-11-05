// Attempt v73 generic renderer: vertex attributes read from resolved memory (resolved_vertex_fetch.h). For every
// output vertex the ring holds three texel coordinates a, b, c (x | y << 16; 0xFFFFFFFF: no texel, zero): the value
// is T[a] when b == c, else T[a] + T[b] - T[c] (the fourth corner of a CPU-expanded rectangle). T is the resolved
// image as the memory holds it: flags bit 0 exchanges red and blue (the resolve's copy_dest_swap), bit 1 the 16-bit
// halves of each dword (the resolve's and the fetch's endian modes compose to 16-in-32). Compiled with the local DXC:
//   tools/spirv_inc.py src/generic_vertex_gather.hlsl src/generic_vertex_gather.inc
[[vk::combinedImageSampler]][[vk::binding(0,0)]] Texture2D<float4> source;
[[vk::combinedImageSampler]][[vk::binding(0,0)]] SamplerState sourceSampler;
[[vk::binding(1,0)]] RWByteAddressBuffer ring;
struct Parameters { uint count; uint coordinates; uint destination; uint flags; };
[[vk::push_constant]] Parameters parameters;
float4 texel(uint c) {
    if (c == 0xFFFFFFFFu) return float4(0, 0, 0, 0);
    float4 v = source.Load(int3(c & 0xFFFFu, c >> 16, 0));
    if (parameters.flags & 1u) v = v.bgra;
    if (parameters.flags & 2u) v = v.yxwz;
    return v;
}
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= parameters.count) return;
    const uint at = parameters.coordinates + 12u * id.x;
    const uint a = ring.Load(at), b = ring.Load(at + 4u), c = ring.Load(at + 8u);
    float4 v = texel(a);
    if (b != c) v = v + texel(b) - texel(c);
    ring.Store4(parameters.destination + 16u * id.x, asuint(v));
}
