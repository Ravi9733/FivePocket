#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace recomp::gpu::test {
using Words = std::array<std::uint32_t,3>;
using Bytes = std::vector<std::uint8_t>;
inline std::uint64_t alloc(std::uint32_t kind = 2, std::uint32_t count = 1) {
    return (std::uint64_t{12} << 44) | (std::uint64_t{kind} << 41) | (count-1);
}
inline std::uint64_t exec(std::uint32_t address = 1, std::uint32_t count = 1, std::uint32_t opcode = 2) {
    return (std::uint64_t{opcode} << 44) | (std::uint64_t{1} << 41) | address | (count << 12);
}
inline Words alu(std::uint32_t dest = 0, std::uint32_t vector = 15, std::uint32_t scalar = 0,
        bool exported = true, bool zero = false, std::uint32_t source = 0, std::uint32_t swizzle = 0) {
    return {{(50u << 26) | dest | (vector << 16) | (scalar << 20) | (std::uint32_t{exported} << 15) |
        (std::uint32_t{zero} << 14), (swizzle << 16) | (swizzle << 8),
        (2u << 24) | (7u << 29) | (source << 16) | (source << 8)}};
}
inline Bytes program(std::vector<std::uint64_t> controls, const std::vector<Words>& instructions) {
    if (controls.size() % 2) controls.push_back(0);
    Bytes bytes;
    const auto append = [&](std::uint32_t w) {
        for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(w >> shift);
    };
    for (std::size_t i = 0; i < controls.size(); i += 2) {
        append(controls[i]); append((controls[i] >> 32) | ((controls[i+1] & 65535) << 16)); append(controls[i+1] >> 16);
    }
    for (const auto& words : instructions) for (auto w : words) append(w);
    return bytes;
}
}
