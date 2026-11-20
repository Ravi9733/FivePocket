#include "shader_constant_upload.h"
#include <cstring>
#include <vector>

namespace recomp::gpu {
bool decodeShaderFloatConstants(FloatConstantStage stage, std::uint32_t firstRegister,
                                std::uint32_t registerCount, const std::uint8_t* source,
                                std::size_t sourceBytes, ShaderFloatConstants& destination) noexcept {
    std::size_t capacity = 0;
    std::uint32_t* target = nullptr;
    switch (stage) {
        case FloatConstantStage::Vertex:
            capacity = 256;
            target = destination.vertex.data();
            break;
        case FloatConstantStage::Pixel:
            capacity = 224;
            target = destination.pixel.data();
            break;
        default:
            return false;
    }
    if (firstRegister > capacity || registerCount > capacity - firstRegister) return false;
    if (registerCount == 0) return true;
    const std::size_t words = static_cast<std::size_t>(registerCount) * 4;
    if (!source || words > static_cast<std::size_t>(-1) / 4) return false;
    const std::size_t required = words * 4;
    if (sourceBytes < required) return false;
    // Stage into a temporary first: source is allowed to alias destination.
    try {
        std::vector<std::uint32_t> decoded(words);
        for (std::size_t i = 0; i < words; ++i) {
            const auto* p = source + i * 4;
            decoded[i] = (std::uint32_t(p[0]) << 24) |
                         (std::uint32_t(p[1]) << 16) |
                         (std::uint32_t(p[2]) << 8) |
                         std::uint32_t(p[3]);
        }
        std::memcpy(target + static_cast<std::size_t>(firstRegister) * 4,
                    decoded.data(), required);
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace recomp::gpu
