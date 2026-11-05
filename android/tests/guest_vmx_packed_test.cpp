#include "guest_vmx_packed.h"

#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstdio>

static unsigned checks;

static void check(std::uint32_t packed) {
    std::uint32_t src[4] = {packed, 0x12345678u, 0xfedcba98u, 0x87654321u};
    std::uint32_t dst[4] = {};
    recomp::vmx::unpackNormPacked32(dst, src);
    for (unsigned c = 0; c < 3; ++c) {
        const auto field = (packed >> (c * 10)) & 1023u;
        const int signedValue = field < 512 ? int(field) : int(field) - 1024;
        // Independent exact binary arithmetic, not the implementation's
        // biased-integer addition. -512 is the architecture's overflow code.
        const auto expected = signedValue == -512 ? 0x7fc00000u :
            std::bit_cast<std::uint32_t>(3.0f + float(signedValue) * 0x1p-22f);
        assert(dst[3 - c] == expected);
    }
    const auto alpha = std::bit_cast<std::uint32_t>(1.0f + float(packed >> 30) * 0x1p-23f);
    assert(dst[0] == alpha);
    assert(src[0] == packed && src[1] == 0x12345678u && src[2] == 0xfedcba98u && src[3] == 0x87654321u);
    recomp::vmx::unpackNormPacked32(src, src);
    for (unsigned c = 0; c < 4; ++c) assert(src[c] == dst[c]);
    ++checks;
}

int main() {
    // Published Xenia instr_vupkd3d128.s UINT_2101010 hardware-test vectors.
    for (auto row : {std::array<std::uint32_t, 2>{0x400001ffu, 0x404001ffu},
                     std::array<std::uint32_t, 2>{0x40000201u, 0x403ffe01u},
                     std::array<std::uint32_t, 2>{0x40000200u, 0x7fc00000u}}) {
        std::uint32_t v[4] = {row[0], 0xcdcdcdcdu, 0xcdcdcdcdu, 0xcdcdcdcdu};
        recomp::vmx::unpackNormPacked32(v, v);
        assert(v[3] == row[1] && v[2] == 0x40400000u && v[1] == 0x40400000u && v[0] == 0x3f800001u);
    }
    for (unsigned c = 0; c < 3; ++c)
        for (unsigned field = 0; field < 1024; ++field)
            for (unsigned alpha = 0; alpha < 4; ++alpha)
                check((field << (c * 10)) | (alpha << 30));
    for (unsigned x = 0; x < 1024; ++x)
        for (unsigned y = 0; y < 1024; ++y)
            check(x | (y << 10) | (((x * 37 + y * 19) & 1023) << 20) | ((x & 3) << 30));
    for (auto value : {0u, 0xffffffffu, 0x20080200u, 0x1ff7fdffu, 0x20180601u}) check(value);
    assert(checks == 1060869);
    std::printf("VMX_NORMPACKED32_BIT_EXACT_COMPONENTS_PAIR_GRID_ALIAS_PASS %u\n", checks);
}
