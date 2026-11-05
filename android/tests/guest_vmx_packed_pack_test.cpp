#include "guest_vmx_packed.h"

#include <array>
#include <bit>
#include <cassert>
#include <cfenv>
#include <cstdint>
#include <cstdio>
#include <cstring>

#pragma STDC FENV_ACCESS ON

static unsigned checks;

static std::uint32_t reference(const std::uint32_t (&src)[4]) {
    std::uint32_t result = 0;
    for (unsigned c = 0; c < 4; ++c) {
        const float low = c == 3 ? 3.0f : 3.0f - 511.0f * 0x1p-22f;
        const float high = c == 3 ? 3.0f + 3.0f * 0x1p-22f : 3.0f + 511.0f * 0x1p-22f;
        float value = std::bit_cast<float>(src[3 - c]);
        value = value >= low ? value : low;
        value = value <= high ? value : high;
        // An independent arithmetic reference, not a mantissa mask/bit clamp.
        const auto integer = static_cast<std::int32_t>((value - 3.0f) * 0x1p22f);
        result |= (static_cast<std::uint32_t>(integer) & (c == 3 ? 3u : 1023u)) << (c * 10);
    }
    return result;
}

template<unsigned Mask, unsigned Shift>
static void checkPlacement(const std::uint32_t (&src)[4], std::uint32_t packed) {
    // Explicit PPC guest X,Y,Z,W write masks from InstrEmit_vpkd3d128's
    // permutation controls, independently reversed to host W,Z,Y,X below.
    constexpr unsigned written[3][4] = {{8, 4, 2, 1}, {12, 6, 3, 1}, {12, 6, 3, 8}};
    struct Guarded { std::uint32_t before; std::uint32_t words[4]; std::uint32_t after; };
    Guarded out{0x12345678u, {0xabcdef01u, 0xa1b2c3d4u, 0x66778899u, 0xcafef00du}, 0x87654321u};
    const auto original = out;
    std::uint32_t copy[4]; std::memcpy(copy, src, sizeof(copy));
    std::feclearexcept(FE_ALL_EXCEPT);
    std::feraiseexcept(FE_DIVBYZERO);
    const int flags = std::fetestexcept(FE_ALL_EXCEPT), rounding = std::fegetround();
    recomp::vmx::packNormPacked32<Mask, Shift>(out.words, copy);
    assert(std::fetestexcept(FE_ALL_EXCEPT) == flags && std::fegetround() == rounding);
    assert(std::memcmp(src, copy, sizeof(copy)) == 0);
    recomp::vmx::packNormPacked32<Mask, Shift>(copy, copy);
    assert(std::fetestexcept(FE_ALL_EXCEPT) == flags && std::fegetround() == rounding);
    for (unsigned lane = 0; lane < 4; ++lane) {
        const bool changed = (written[Mask - 1][Shift] & (1u << (3 - lane))) != 0;
        assert(out.words[lane] == (changed ? packed : original.words[lane]));
        assert(copy[lane] == (changed ? packed : src[lane]));
    }
    assert(out.before == original.before && out.after == original.after);
    ++checks;
}

static void check(const std::uint32_t (&src)[4]) {
    const auto expected = reference(src);
    checkPlacement<1, 0>(src, expected); checkPlacement<1, 1>(src, expected);
    checkPlacement<1, 2>(src, expected); checkPlacement<1, 3>(src, expected);
    checkPlacement<2, 0>(src, expected); checkPlacement<2, 1>(src, expected);
    checkPlacement<2, 2>(src, expected); checkPlacement<2, 3>(src, expected);
    checkPlacement<3, 0>(src, expected); checkPlacement<3, 1>(src, expected);
    checkPlacement<3, 2>(src, expected); checkPlacement<3, 3>(src, expected);
}

int main() {
    // Published Xenia instr_vpkd3d128.s UINT_2101010 vectors (guest XYZW).
    constexpr std::uint32_t published[3][5] = {
        {0xb8ff8000u, 0xb8ff8000u, 0xc04001ffu, 0x4e9a5a5au, 0xe0180601u},
        {0x42c80000u, 0xc2c80000u, 0x40400000u, 0x3f800000u, 0x000805ffu},
        {0x3f000000u, 0xbf000000u, 0x3f800000u, 0, 0x20180601u}};
    for (auto& row : published) {
        std::uint32_t src[4] = {row[3], row[2], row[1], row[0]};
        assert(reference(src) == row[4]);
        check(src);
    }
    for (int rounding : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        assert(std::fesetround(rounding) == 0);
        // Every biased XYZ encoding, including both adjacent overflow values.
        for (int value = -513; value <= 513; ++value)
            for (unsigned alpha = 0; alpha < 4; ++alpha) {
                std::uint32_t src[4] = {0x40400000u + alpha,
                    std::bit_cast<std::uint32_t>(3.0f - float(value) * 0x1p-22f),
                    std::bit_cast<std::uint32_t>(3.0f + float(value / 2) * 0x1p-22f),
                    std::bit_cast<std::uint32_t>(3.0f + float(value) * 0x1p-22f)};
                check(src);
            }
        // Exponent/sign classes, bounds, signed zeros, denormals, infinity,
        // quiet/signaling NaNs and representative mantissa payloads in every lane.
        for (unsigned exponent = 0; exponent < 256; ++exponent)
            for (unsigned sign = 0; sign < 2; ++sign)
                for (unsigned fraction : {0u, 1u, 0x201u, 0x3fffffu, 0x400000u, 0x7fffffu})
                    for (unsigned lane = 0; lane < 4; ++lane) {
                        std::uint32_t src[4] = {0x40400002u, 0x403fff01u, 0x40400007u, 0x40400081u};
                        src[lane] = (sign << 31) | (exponent << 23) | fraction;
                        check(src);
                    }
    }
    assert(std::fesetround(FE_TONEAREST) == 0);
    assert(checks == 787044);
    std::printf("VMX_NORMPACKED32_PACK_SATURATION_LANES_ALIAS_FP_STATE_PASS %u\n", checks);
}
