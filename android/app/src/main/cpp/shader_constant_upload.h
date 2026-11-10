#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace recomp::gpu {
enum class FloatConstantStage : std::uint8_t { Vertex = 0, Pixel = 1 };
struct ShaderFloatConstants {
    std::array<std::uint32_t, 1024> vertex{};
    std::array<std::uint32_t, 896> pixel{};
};
// Guest shader constants are big-endian IEEE-754 bit patterns. Preserve their
// exact bits (including NaNs and signed zero); do not reinterpret as host floats.
bool decodeShaderFloatConstants(FloatConstantStage stage, std::uint32_t firstRegister,
                                std::uint32_t registerCount, const std::uint8_t* source,
                                std::size_t sourceBytes, ShaderFloatConstants& destination) noexcept;
} // namespace recomp::gpu
