#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace xmem_lzx
{
inline constexpr size_t kDefaultFrameOutputSize = 0x8000;

struct Limits
{
    size_t maxInputSize = 64 * 1024 * 1024;
    size_t maxOutputSize = 256 * 1024 * 1024;
    size_t maxFrameCount = 1'000'000;
    size_t maxResidualTailSize = 15;
    int windowBits = 16;
};

struct Frame
{
    size_t headerOffset{};
    size_t payloadOffset{};
    uint16_t compressedSize{};
    uint16_t uncompressedSize{};
};

struct Framing
{
    std::vector<Frame> frames;
    size_t decodedSize{};
    size_t framedSize{};
    size_t residualTailSize{};
};

class Error final : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

Framing ParseFraming(
    std::span<const uint8_t> input,
    size_t expectedOutputSize,
    const Limits& limits = {});

std::vector<uint8_t> Decompress(
    std::span<const uint8_t> input,
    size_t expectedOutputSize,
    const Limits& limits = {});
}
