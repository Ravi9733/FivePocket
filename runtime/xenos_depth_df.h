// Package depth-float (diag-df-float / diag-df-round): the k_24_8_FLOAT (D24FS8) depth value as the 360 stores it.
// 20e4 = 4-bit exponent (bias 15) and 20-bit mantissa, no sign, denormals in units of 2^-34, values in [0, 2); code c holds
//   e = c >> 20 == 0 : (c & 0xFFFFF) * 2^-34
//   e > 0            : (1 + (c & 0xFFFFF) / 2^20) * 2^(e - 15)
// Every 20e4 value is exactly a float32 (20 <= 23 mantissa bits, exponents -34..0), so a D32_SFLOAT depth buffer that only
// ever holds 20e4 values compares exactly like the 360's 20e4 buffer (both orders are the real-number order).
// quantizeXenosFloat20e4(x) = decode(encode(x)) with xenos_depth_word.h's encoder (round to nearest, ties to even; 0 for zero,
// negatives and NaN; the largest code for values >= 2): the depth a resolve to k_24_8_FLOAT / an EDRAM store gives back.
// Written in the common subset of C++ and HLSL like xenos_depth_word.h (which it includes): the resolve shaders
// (generic_depth_df_*.hlsl) and the host tests (tools/gta5_depth/tests) compile this same text.
#ifdef __cplusplus
#pragma once
#include "xenos_depth_word.h"
namespace xenos_depth_word {
inline float asfloat(uint u) {float f;std::memcpy(&f,&u,4);return f;}
#endif

// 20e4 code (low 24 bits) -> its value as a float32 (exact).
XDW_FUNCTION float decodeXenosFloat20e4(uint code) {
    const uint c = code & 0xFFFFFFu;
    const uint e = c >> 20;
    const uint m = c & 0xFFFFFu;
    // e == 0: m * 2^-34 (m < 2^20: the conversion and the power-of-two scale are exact); else exponent e - 15 -> float32 bias 127.
    return e == 0u ? float(m) * asfloat(0x2E800000u) : asfloat(((e + 112u) << 23) | (m << 3));
}
// The 20e4 value nearest to `value` (ties to even), as a float32: what a D24FS8 store of `value` reads back.
XDW_FUNCTION float quantizeXenosFloat20e4(float value) {
    return decodeXenosFloat20e4(encodeXenosFloat20e4(value));
}

#ifdef __cplusplus
} // namespace xenos_depth_word
#endif
