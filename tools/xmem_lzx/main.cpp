#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include "xmem_lzx.h"

#include <array>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace
{
struct Arguments
{
    std::string input;
    std::string output;
    size_t expectedSize{};
    xmem_lzx::Limits limits;
};

[[noreturn]] void UsageError(const std::string& message)
{
    throw xmem_lzx::Error(
        message +
        "\nusage: xmem-lzx --expected-size N [--max-input N] [--max-output N] "
        "[--max-frames N] [--max-tail N] [--window-bits N] INPUT OUTPUT\n"
        "use - for stdin or stdout");
}

size_t ParseSize(std::string_view text, const char* option)
{
    size_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
    {
        UsageError(std::string("invalid value for ") + option);
    }
    return value;
}

Arguments ParseArguments(int argc, char** argv)
{
    Arguments arguments;
    bool hasExpectedSize = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view value(argv[i]);
        auto optionValue = [&](const char* option) -> std::string_view
        {
            if (++i >= argc)
            {
                UsageError(std::string("missing value for ") + option);
            }
            return argv[i];
        };

        if (value == "--expected-size")
        {
            arguments.expectedSize = ParseSize(optionValue("--expected-size"), "--expected-size");
            hasExpectedSize = true;
        }
        else if (value == "--max-input")
        {
            arguments.limits.maxInputSize = ParseSize(optionValue("--max-input"), "--max-input");
        }
        else if (value == "--max-output")
        {
            arguments.limits.maxOutputSize = ParseSize(optionValue("--max-output"), "--max-output");
        }
        else if (value == "--max-frames")
        {
            arguments.limits.maxFrameCount = ParseSize(optionValue("--max-frames"), "--max-frames");
        }
        else if (value == "--max-tail")
        {
            arguments.limits.maxResidualTailSize = ParseSize(optionValue("--max-tail"), "--max-tail");
        }
        else if (value == "--window-bits")
        {
            const size_t parsed = ParseSize(optionValue("--window-bits"), "--window-bits");
            if (parsed > static_cast<size_t>(std::numeric_limits<int>::max()))
            {
                UsageError("--window-bits is too large");
            }
            arguments.limits.windowBits = static_cast<int>(parsed);
        }
        else if (value == "--help" || value == "-h")
        {
            UsageError("bounded Xbox XMem/LZX decoder");
        }
        else if (!value.empty() && value.front() == '-' && value != "-")
        {
            UsageError("unknown option");
        }
        else if (arguments.input.empty())
        {
            arguments.input = value;
        }
        else if (arguments.output.empty())
        {
            arguments.output = value;
        }
        else
        {
            UsageError("too many positional arguments");
        }
    }

    if (!hasExpectedSize || arguments.input.empty() || arguments.output.empty())
    {
        UsageError("expected size, input, and output are required");
    }
    if (arguments.input == "-" && arguments.output == "-")
    {
        // This is safe because the complete bounded input is consumed before any
        // output is written, but spelling the case out prevents future streaming
        // changes from breaking pipeline behavior.
    }
    else if (arguments.input != "-" && arguments.output != "-")
    {
        std::error_code error;
        if (std::filesystem::exists(arguments.output, error))
        {
            UsageError("output file already exists");
        }
    }
    return arguments;
}

std::vector<uint8_t> ReadBounded(std::istream& stream, size_t limit)
{
    std::vector<uint8_t> result;
    std::array<char, 64 * 1024> block{};
    while (stream)
    {
        stream.read(block.data(), block.size());
        const size_t count = static_cast<size_t>(stream.gcount());
        if (count > limit - std::min(limit, result.size()))
        {
            throw xmem_lzx::Error("input exceeds configured size");
        }
        result.insert(result.end(), block.data(), block.data() + count);
    }
    if (!stream.eof())
    {
        throw xmem_lzx::Error("failed while reading input");
    }
    return result;
}

std::vector<uint8_t> ReadInput(const Arguments& arguments)
{
    if (arguments.input == "-")
    {
        return ReadBounded(std::cin, arguments.limits.maxInputSize);
    }

    std::ifstream stream(arguments.input, std::ios::binary);
    if (!stream)
    {
        throw xmem_lzx::Error("unable to open input file");
    }
    return ReadBounded(stream, arguments.limits.maxInputSize);
}

void WriteOutput(const Arguments& arguments, std::span<const uint8_t> output)
{
    if (arguments.output == "-")
    {
        std::cout.write(reinterpret_cast<const char*>(output.data()), output.size());
        if (!std::cout)
        {
            throw xmem_lzx::Error("failed while writing stdout");
        }
        return;
    }

    std::FILE* stream = std::fopen(arguments.output.c_str(), "wbx");
    if (stream == nullptr)
    {
        throw xmem_lzx::Error("unable to exclusively create output file");
    }
    const size_t written = std::fwrite(output.data(), 1, output.size(), stream);
    const int closeResult = std::fclose(stream);
    if (written != output.size() || closeResult != 0)
    {
        std::error_code ignored;
        std::filesystem::remove(arguments.output, ignored);
        throw xmem_lzx::Error("failed while writing output file");
    }
}
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    // Windows opens the standard streams as text: a 0x1A byte would end the input and every 0x0A written would grow
    // a 0x0D. The payload and the decoded bytes are binary on every host.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    try
    {
        const Arguments arguments = ParseArguments(argc, argv);
        const std::vector<uint8_t> input = ReadInput(arguments);
        const xmem_lzx::Framing framing =
            xmem_lzx::ParseFraming(input, arguments.expectedSize, arguments.limits);
        const std::vector<uint8_t> output =
            xmem_lzx::Decompress(input, arguments.expectedSize, arguments.limits);
        WriteOutput(arguments, output);
        std::cerr << "decoded " << output.size() << " bytes from "
                  << framing.frames.size() << " bounded frames\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "xmem-lzx: " << error.what() << '\n';
        return 1;
    }
}
