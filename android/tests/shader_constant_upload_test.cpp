#include "shader_constant_upload.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

using namespace recomp::gpu;
namespace {
constexpr std::array<std::uint32_t, 12> Patterns{0, 0x80000000, 0x7F800000, 0xFF800000,
    0x7FC1ABCD, 0x7F812345, 1, 0x007FFFFF, 0x00800000, 0x3F800000, 0x01234567, 0xFEDCBA98};
}
int main() {
    ShaderFloatConstants constants;
    constants.vertex.fill(0xDEADBEEF); constants.pixel.fill(0xAABBCCDD);
    std::vector<std::uint8_t> source(4096 + 1);
    for (std::size_t word = 0; word < 1024; ++word) {
        const auto value = Patterns[word % Patterns.size()];
        for (unsigned byte = 0; byte < 4; ++byte) source[1 + word * 4 + byte] = std::uint8_t(value >> ((3 - byte) * 8));
    }
    assert(decodeShaderFloatConstants(FloatConstantStage::Vertex, 0, 256, source.data() + 1, 4096, constants));
    for (std::size_t word = 0; word < 1024; ++word) assert(constants.vertex[word] == Patterns[word % Patterns.size()]);
    for (auto word : constants.pixel) assert(word == 0xAABBCCDD);
    assert(decodeShaderFloatConstants(FloatConstantStage::Pixel, 223, 1, source.data() + 1, 16, constants));
    for (std::size_t word = 0; word < 892; ++word) assert(constants.pixel[word] == 0xAABBCCDD);
    for (std::size_t word = 0; word < 4; ++word) assert(constants.pixel[892 + word] == Patterns[word]);
    const auto before = constants;
    for (auto stage : {FloatConstantStage::Vertex, FloatConstantStage::Pixel}) {
        const auto capacity = stage == FloatConstantStage::Vertex ? 256u : 224u;
        assert(decodeShaderFloatConstants(stage, capacity, 0, nullptr, 0, constants));
        for (const auto first : {capacity, capacity + 1, 0xFFFFFFFFu})
            assert(!decodeShaderFloatConstants(stage, first, 1, source.data(), 16, constants));
        assert(!decodeShaderFloatConstants(stage, 0, 0xFFFFFFFFu, source.data(), 0, constants));
        for (std::size_t size : {0, 1, 15, 17, 32})
            assert(!decodeShaderFloatConstants(stage, 0, 1, source.data(), size, constants));
        assert(!decodeShaderFloatConstants(stage, 0, 1, nullptr, 16, constants));
    }
    assert(!decodeShaderFloatConstants(static_cast<FloatConstantStage>(2), 0, 0, nullptr, 0, constants));
    assert(constants.vertex == before.vertex && constants.pixel == before.pixel);
    // Overlap is interpreted from the complete original source, not incrementally
    // overwritten words. Compare byte order independently of host endianness.
    const auto* alias = reinterpret_cast<const std::uint8_t*>(constants.vertex.data());
    std::array<std::uint32_t, 8> expected{};
    for (std::size_t i = 0; i < 8; ++i)
        for (unsigned byte = 0; byte < 4; ++byte) expected[i] = (expected[i] << 8) | alias[i * 4 + byte];
    assert(decodeShaderFloatConstants(FloatConstantStage::Vertex, 1, 2, alias, 32, constants));
    for (std::size_t i = 0; i < 8; ++i) assert(constants.vertex[4 + i] == expected[i]);
}
