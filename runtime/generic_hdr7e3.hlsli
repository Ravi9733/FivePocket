// Attempt v75: Xenos 7e3 (k_2_10_10_10_FLOAT colour channel) codes for the HDR resolve conversion.
// Shared, unchanged, by generic_resolve_convert.hlsl (DXC) and the v75 host test (C++: the test
// defines H7_UINT, H7_ASUINT, H7_MIN, H7_CLAMP, H7_ROUND before including this file).
// 7e3: 3-bit exponent e, 7-bit mantissa m, no sign; e == 0: m / 512; else (1 + m/128) * 2^(e-3);
// range [0, 31.875]. The code is (e << 7) | m, 0..1023.
#ifndef H7_UINT
#define H7_UINT uint
#define H7_ASUINT(x) asuint(x)
#define H7_MIN(a, b) min(a, b)
#define H7_CLAMP(x, a, b) clamp(x, a, b)
#define H7_ROUND(x) round(x)
#endif

// v48 (exact, mode 1): the nearest code (the value the EDRAM stores); round-half-away on the mantissa,
// a mantissa of 128 carries into the exponent.
H7_UINT encode7e3(float f) {
    f = H7_CLAMP(f, 0.0f, 31.875f);
    if (!(f >= 0.25f)) return H7_MIN((H7_UINT)H7_ROUND(f * 512.0f), (H7_UINT)128u); // denormal range (e = 0); 128 = 0.25
    const H7_UINT bits = H7_ASUINT(f);
    const int e = (int)((bits >> 23) & 0xFFu) - 127;                                // -2..4
    const H7_UINT m = (H7_UINT)H7_ROUND(float(bits & 0x7FFFFFu) * (128.0f / 8388608.0f));
    return H7_MIN((H7_UINT)(e + 3) * 128u + m, (H7_UINT)1023u);
}

// v75 (continuous, mode 3): the same code without rounding the mantissa, a real number in [0, 1023]
// whose integer part is the exponent field times 128 plus the whole mantissa steps; floor/frac of
// code / 128 give back e and m / 128 + (fraction), and the game's decode (postfx.fxc: e = floor(v),
// m = frac(v), e == 0 ? 2m / 8 : (1 + m) * 2^e / 8 with v = code * 8 / 1023 after the fetch's
// exponent adjust 3) returns f to half-float precision. Equal to encode7e3 wherever f is a 7e3 value.
// NaN and negative values give 0 (the exact encoder's clamp gives 0 for them on the device too).
float encode7e3Continuous(float f) {
    if (!(f > 0.0f)) return 0.0f;
    f = H7_CLAMP(f, 0.0f, 31.875f);
    if (f < 0.25f) return f * 512.0f;
    const H7_UINT bits = H7_ASUINT(f);
    const int e = (int)((bits >> 23) & 0xFFu) - 127;                                // -2..4
    const float m = float(bits & 0x7FFFFFu) * (128.0f / 8388608.0f);                 // [0, 128)
    return H7_MIN(float(e + 3) * 128.0f + m, 1023.0f);
}

// The value a code stands for (integer codes: the 7e3 value; fractional codes: the game's decode).
float decode7e3Code(float code) {
    const float v = code / 128.0f;
    const float e = floor(v), m = v - floor(v);
    return e == 0.0f ? m / 4.0f : (1.0f + m) * exp2(e - 3.0f);
}
