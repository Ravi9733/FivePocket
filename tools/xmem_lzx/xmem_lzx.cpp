#include "xmem_lzx.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

extern "C"
{
#include <lzx.h>
#include <mspack.h>
}

namespace xmem_lzx
{
namespace
{
uint16_t ReadBigEndian16(std::span<const uint8_t> input, size_t offset)
{
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(input[offset]) << 8) |
        static_cast<uint16_t>(input[offset + 1]));
}

void RequireAvailable(size_t offset, size_t amount, size_t size, const char* what)
{
    if (offset > size || amount > size - offset)
    {
        throw Error(std::string("truncated ") + what);
    }
}

void RequireAddWithin(size_t current, size_t amount, size_t limit, const char* what)
{
    if (current > limit || amount > limit - current)
    {
        throw Error(std::string(what) + " exceeds configured or expected size");
    }
}

struct MemoryFile
{
    const uint8_t* input{};
    uint8_t* output{};
    size_t size{};
    size_t offset{};
};

int MemoryRead(mspack_file* file, void* buffer, int bytes)
{
    if (bytes < 0)
    {
        return -1;
    }

    auto& memory = *reinterpret_cast<MemoryFile*>(file);
    if (memory.input == nullptr || memory.offset > memory.size)
    {
        return -1;
    }

    const size_t count = std::min(static_cast<size_t>(bytes), memory.size - memory.offset);
    if (count != 0)
    {
        std::memcpy(buffer, memory.input + memory.offset, count);
        memory.offset += count;
    }
    return static_cast<int>(count);
}

int MemoryWrite(mspack_file* file, void* buffer, int bytes)
{
    if (bytes < 0)
    {
        return -1;
    }

    auto& memory = *reinterpret_cast<MemoryFile*>(file);
    const size_t count = static_cast<size_t>(bytes);
    if (memory.output == nullptr || memory.offset > memory.size ||
        count > memory.size - memory.offset)
    {
        return -1;
    }

    if (count != 0)
    {
        std::memcpy(memory.output + memory.offset, buffer, count);
        memory.offset += count;
    }
    return bytes;
}

void* MemoryAllocate(mspack_system*, size_t bytes)
{
    return std::calloc(1, bytes);
}

void MemoryFree(void* pointer)
{
    std::free(pointer);
}

void MemoryCopy(void* source, void* destination, size_t bytes)
{
    std::memcpy(destination, source, bytes);
}

void IgnoreMessage(mspack_file*, const char*, ...)
{
}

mspack_system MakeMemorySystem()
{
    mspack_system system{};
    system.read = MemoryRead;
    system.write = MemoryWrite;
    system.message = IgnoreMessage;
    system.alloc = MemoryAllocate;
    system.free = MemoryFree;
    system.copy = MemoryCopy;
    return system;
}

void DecodeStream(
    std::span<const uint8_t> compressed,
    std::span<uint8_t> output,
    int windowBits)
{
    if (compressed.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
    {
        throw Error("compressed stream is too large for libmspack");
    }

    auto system = MakeMemorySystem();
    MemoryFile inputFile{ compressed.data(), nullptr, compressed.size(), 0 };
    MemoryFile outputFile{ nullptr, output.data(), output.size(), 0 };
    std::unique_ptr<lzxd_stream, decltype(&lzxd_free)> decoder(
        lzxd_init(
            &system,
            reinterpret_cast<mspack_file*>(&inputFile),
            reinterpret_cast<mspack_file*>(&outputFile),
            windowBits,
            0,
            static_cast<int>(kDefaultFrameOutputSize),
            static_cast<off_t>(output.size()),
            0),
        lzxd_free);
    if (!decoder)
    {
        throw Error("libmspack rejected the LZX decoder parameters");
    }

    const int result = lzxd_decompress(decoder.get(), static_cast<off_t>(output.size()));
    if (result != MSPACK_ERR_OK)
    {
        throw Error("LZX stream decompression failed with code " + std::to_string(result));
    }
    if (outputFile.offset != output.size())
    {
        throw Error("LZX stream produced an unexpected output size");
    }
}
}

Framing ParseFraming(
    std::span<const uint8_t> input,
    size_t expectedOutputSize,
    const Limits& limits)
{
    if (expectedOutputSize == 0)
    {
        throw Error("expected output size must be nonzero");
    }
    if (limits.maxInputSize == 0 || limits.maxOutputSize == 0 ||
        limits.maxFrameCount == 0)
    {
        throw Error("limits must be nonzero");
    }
    if (input.size() > limits.maxInputSize)
    {
        throw Error("input exceeds configured size");
    }
    if (expectedOutputSize > limits.maxOutputSize)
    {
        throw Error("expected output exceeds configured size");
    }
    if (limits.windowBits < 15 || limits.windowBits > 21)
    {
        throw Error("regular LZX window bits must be between 15 and 21");
    }

    Framing framing;
    size_t offset = 0;
    while (framing.decodedSize < expectedOutputSize)
    {
        if (framing.frames.size() >= limits.maxFrameCount)
        {
            throw Error("frame count exceeds configured limit");
        }

        RequireAvailable(offset, 2, input.size(), "XMem frame header");
        Frame frame;
        frame.headerOffset = offset;
        if (input[offset] == 0xFF)
        {
            RequireAvailable(offset, 5, input.size(), "extended XMem frame header");
            frame.uncompressedSize = ReadBigEndian16(input, offset + 1);
            frame.compressedSize = ReadBigEndian16(input, offset + 3);
            offset += 5;
        }
        else
        {
            frame.uncompressedSize = static_cast<uint16_t>(kDefaultFrameOutputSize);
            frame.compressedSize = ReadBigEndian16(input, offset);
            offset += 2;
        }

        if (frame.compressedSize == 0 || frame.uncompressedSize == 0)
        {
            throw Error("zero-sized XMem frame before expected output was reached");
        }
        RequireAddWithin(
            framing.decodedSize,
            frame.uncompressedSize,
            expectedOutputSize,
            "declared frame output");
        RequireAvailable(offset, frame.compressedSize, input.size(), "XMem frame payload");

        frame.payloadOffset = offset;
        framing.frames.push_back(frame);
        framing.decodedSize += frame.uncompressedSize;
        offset += frame.compressedSize;
    }

    framing.framedSize = offset;
    if (offset < input.size())
    {
        RequireAvailable(offset, 2, input.size(), "XMem zero terminator");
        if (ReadBigEndian16(input, offset) != 0)
        {
            throw Error("unexpected bytes after the final XMem frame");
        }
        offset += 2;
        framing.residualTailSize = input.size() - offset;
        if (framing.residualTailSize > limits.maxResidualTailSize)
        {
            throw Error("residual tail exceeds configured limit");
        }
    }

    return framing;
}

std::vector<uint8_t> Decompress(
    std::span<const uint8_t> input,
    size_t expectedOutputSize,
    const Limits& limits)
{
    const Framing framing = ParseFraming(input, expectedOutputSize, limits);
    std::vector<uint8_t> output(expectedOutputSize);
    size_t compressedSize = 0;
    for (const Frame& frame : framing.frames)
    {
        RequireAddWithin(
            compressedSize,
            frame.compressedSize,
            limits.maxInputSize,
            "deframed input");
        compressedSize += frame.compressedSize;
    }

    std::vector<uint8_t> compressed;
    compressed.reserve(compressedSize);
    for (const Frame& frame : framing.frames)
    {
        const auto payload = input.subspan(frame.payloadOffset, frame.compressedSize);
        compressed.insert(compressed.end(), payload.begin(), payload.end());
    }

    DecodeStream(compressed, output, limits.windowBits);
    return output;
}
}
