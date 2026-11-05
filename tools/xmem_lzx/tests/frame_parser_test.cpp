#include "xmem_lzx.h"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <string_view>
#include <vector>

namespace
{
void Require(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void RequireError(const std::function<void()>& operation, std::string_view message)
{
    try
    {
        operation();
    }
    catch (const xmem_lzx::Error&)
    {
        return;
    }
    std::cerr << "expected rejection: " << message << '\n';
    std::exit(1);
}
}

int main()
{
    {
        const std::vector<uint8_t> input{ 0xFF, 0x00, 0x03, 0x00, 0x02, 0xAA, 0xBB };
        const auto parsed = xmem_lzx::ParseFraming(input, 3);
        Require(parsed.frames.size() == 1, "extended frame count");
        Require(parsed.frames[0].headerOffset == 0, "extended header offset");
        Require(parsed.frames[0].payloadOffset == 5, "extended payload offset");
        Require(parsed.frames[0].compressedSize == 2, "extended input size");
        Require(parsed.frames[0].uncompressedSize == 3, "extended output size");
        Require(parsed.framedSize == input.size(), "extended framed size");
    }

    {
        const std::vector<uint8_t> input{ 0x00, 0x01, 0xAA };
        const auto parsed =
            xmem_lzx::ParseFraming(input, xmem_lzx::kDefaultFrameOutputSize);
        Require(parsed.frames.size() == 1, "default frame count");
        Require(
            parsed.frames[0].uncompressedSize == xmem_lzx::kDefaultFrameOutputSize,
            "default output size");
    }

    {
        const std::vector<uint8_t> input{
            0x00, 0x01, 0xAA,
            0xFF, 0x00, 0x03, 0x00, 0x02, 0xBB, 0xCC,
            0x00, 0x00, 0x71, 0x72, 0x73
        };
        const auto parsed = xmem_lzx::ParseFraming(
            input,
            xmem_lzx::kDefaultFrameOutputSize + 3);
        Require(parsed.frames.size() == 2, "multi-frame count");
        Require(parsed.decodedSize == xmem_lzx::kDefaultFrameOutputSize + 3,
                "multi-frame output total");
        Require(parsed.residualTailSize == 3, "bounded residual tail");
    }

    {
        std::vector<uint8_t> input;
        for (size_t i = 0; i < 16; ++i)
        {
            input.insert(input.end(), { 0x00, 0x01, static_cast<uint8_t>(i) });
        }
        input.insert(input.end(), {
            0xFF, 0x30, 0x37, 0x00, 0x01, 0xAA,
            0x00, 0x00, 0x01, 0x02, 0x03
        });
        const auto parsed = xmem_lzx::ParseFraming(input, 536631);
        Require(parsed.frames.size() == 17, "representative stream frame count");
        Require(parsed.frames.back().uncompressedSize == 12343,
                "representative final output size");
        Require(parsed.decodedSize == 536631, "representative decoded size");
        Require(parsed.residualTailSize == 3, "representative residual tail");
    }

    RequireError(
        [] { xmem_lzx::ParseFraming(std::vector<uint8_t>{ 0xFF }, 1); },
        "truncated extended header");
    RequireError(
        [] { xmem_lzx::ParseFraming(std::vector<uint8_t>{ 0x00, 0x02, 0xAA },
                                    xmem_lzx::kDefaultFrameOutputSize); },
        "truncated payload");
    RequireError(
        [] { xmem_lzx::ParseFraming(std::vector<uint8_t>{ 0x00, 0x00 }, 1); },
        "early zero terminator");
    RequireError(
        [] { xmem_lzx::ParseFraming(
            std::vector<uint8_t>{ 0xFF, 0x00, 0x02, 0x00, 0x01, 0xAA }, 1); },
        "declared output exceeds expectation");
    RequireError(
        [] { xmem_lzx::ParseFraming(
            std::vector<uint8_t>{ 0xFF, 0x00, 0x01, 0x00, 0x01, 0xAA, 0x12 }, 1); },
        "trailing byte without terminator");
    RequireError(
        [] { xmem_lzx::ParseFraming(
            std::vector<uint8_t>{
                0xFF, 0x00, 0x01, 0x00, 0x01, 0xAA, 0x00, 0x01
            }, 1); },
        "nonzero terminator");

    {
        xmem_lzx::Limits limits;
        limits.maxInputSize = 5;
        RequireError(
            [&] { xmem_lzx::ParseFraming(
                std::vector<uint8_t>{ 0xFF, 0x00, 0x01, 0x00, 0x01, 0xAA },
                1,
                limits); },
            "input limit");
    }
    {
        xmem_lzx::Limits limits;
        limits.maxOutputSize = 2;
        RequireError(
            [&] { xmem_lzx::ParseFraming(
                std::vector<uint8_t>{ 0xFF, 0x00, 0x03, 0x00, 0x01, 0xAA },
                3,
                limits); },
            "output limit");
    }
    {
        xmem_lzx::Limits limits;
        limits.maxFrameCount = 1;
        RequireError(
            [&] { xmem_lzx::ParseFraming(
                std::vector<uint8_t>{
                    0xFF, 0x00, 0x01, 0x00, 0x01, 0xAA,
                    0xFF, 0x00, 0x01, 0x00, 0x01, 0xBB
                },
                2,
                limits); },
            "frame count limit");
    }
    {
        xmem_lzx::Limits limits;
        limits.maxResidualTailSize = 2;
        RequireError(
            [&] { xmem_lzx::ParseFraming(
                std::vector<uint8_t>{
                    0xFF, 0x00, 0x01, 0x00, 0x01, 0xAA,
                    0x00, 0x00, 0x01, 0x02, 0x03
                },
                1,
                limits); },
            "residual tail limit");
    }

    return 0;
}
