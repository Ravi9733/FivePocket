// Attempt v60: the 32-bit word a Xenos depth resolve writes to memory, depth in bits 8-31 and stencil
// in bits 0-7 (the layout xenos_depth24_texture.h reads, and the one GTA V's 19d97c7e restore and its
// b880c5f0/6b2d44d1 ambient passes read back as k_8_8_8_8: the stencil byte is component X). The depth
// is k_24_8_FLOAT's unsigned 20e4 (exponent bias 15, 20-bit mantissa, denormals in units of 2^-34,
// values in [0, 2): xenos_float24_clear.h's decodeXenosFloat24 is the inverse) or k_24_8's unorm24.
// Written in the common subset of C++ and HLSL: generic_depth_word.hlsl includes it (DXC) and the host
// test includes it (clang), so the encoder the GPU runs is the one the test checks.
#ifdef __cplusplus
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
namespace xenos_depth_word {
typedef std::uint32_t uint;
inline uint asuint(float f) {uint u;std::memcpy(&u,&f,4);return u;}
inline float floor(float f) {return std::floor(f);}
#define XDW_FUNCTION inline
#else
#define XDW_FUNCTION
#endif

// Round to nearest, ties to even; 0 for zero, negatives and NaN; the largest word (1.999998) for
// values the format cannot hold (>= 2 after rounding, +Inf).
XDW_FUNCTION uint encodeXenosFloat20e4(float value) {
    if (!(value > 0.0f)) return 0u;
    const uint bits = asuint(value);
    const uint e = (bits >> 23) & 0xFFu;                  // float32 biased exponent
    if (e >= 128u) return 0xFFFFFFu;                      // value >= 2
    const uint full = (bits & 0x7FFFFFu) | 0x800000u;     // 1.m23 (value = full * 2^(e-150))
    const bool normal = e >= 113u;                        // value >= 2^-14
    const uint shift = normal ? 3u : 116u - e;            // to 1.m20, or to units of 2^-34
    if (shift > 24u) return 0u;                           // below half the smallest denormal
    uint kept = full >> shift;
    const uint rest = full & ((1u << shift) - 1u);
    const uint half = 1u << (shift - 1u);
    if (rest > half || (rest == half && (kept & 1u) != 0u)) kept += 1u;
    const uint word = normal ? (((e - 112u) << 20) + (kept - 0x100000u)) : kept; // a carry moves up the exponent
    return word > 0xFFFFFFu ? 0xFFFFFFu : word;
}
// k_24_8 (D24S8): round(depth * (2^24 - 1)), depth clamped to [0, 1]. From 2^23 up the float product is
// already an integer (adding 0.5 there would round to even); the product itself is a float, so the
// result can be one unit off the exact rounding near the top of the range.
XDW_FUNCTION uint encodeXenosUnorm24(float value) {
    const float clamped = value > 0.0f ? (value < 1.0f ? value : 1.0f) : 0.0f;
    const float scaled = clamped * 16777215.0f;
    const uint u = scaled >= 8388608.0f ? (uint)scaled : (uint)floor(scaled + 0.5f);
    return u > 0xFFFFFFu ? 0xFFFFFFu : u;
}
XDW_FUNCTION uint packXenosDepthWord(float depth, uint stencil, uint floatDepth) {
    const uint depth24 = floatDepth != 0u ? encodeXenosFloat20e4(depth) : encodeXenosUnorm24(depth);
    return (depth24 << 8) | (stencil & 0xFFu);
}

#ifdef __cplusplus
} // namespace xenos_depth_word
#endif
