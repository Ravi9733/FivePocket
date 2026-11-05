#include <file.h>
#include <image.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

std::string jsonString(std::string_view value) {
    std::ostringstream escaped;
    escaped << '"';
    for (const unsigned char character : value) {
        switch (character) {
        case '"': escaped << "\\\""; break;
        case '\\': escaped << "\\\\"; break;
        case '\b': escaped << "\\b"; break;
        case '\f': escaped << "\\f"; break;
        case '\n': escaped << "\\n"; break;
        case '\r': escaped << "\\r"; break;
        case '\t': escaped << "\\t"; break;
        default:
            if (character < 0x20U) {
                escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<unsigned int>(character) << std::dec;
            } else {
                escaped << character;
            }
        }
    }
    escaped << '"';
    return escaped.str();
}

std::string hexAddress(std::size_t value) {
    std::ostringstream formatted;
    formatted << "0x" << std::uppercase << std::hex << value;
    return formatted.str();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: xex_image <input.xex> <private-output.bin>\n";
        return 2;
    }

    const std::filesystem::path inputPath(argv[1]);
    const std::filesystem::path outputPath(argv[2]);
    const auto input = LoadFile(inputPath);
    if (input.size() < 4 || input[0] != 'X' || input[1] != 'E'
            || input[2] != 'X' || input[3] != '2') {
        std::cerr << "xex_image: input is not an XEX2 file\n";
        return 2;
    }
    if (!outputPath.has_parent_path()
            || !std::filesystem::is_directory(outputPath.parent_path())) {
        std::cerr << "xex_image: output parent directory does not exist\n";
        return 2;
    }
    if (std::filesystem::exists(outputPath)) {
        std::cerr << "xex_image: refusing to overwrite an existing output file\n";
        return 2;
    }

    Image image = Image::ParseImage(input.data(), input.size());
    if (image.data == nullptr || image.size == 0) {
        std::cerr << "xex_image: XEX2 image could not be decoded\n";
        return 1;
    }

    std::ofstream output(outputPath, std::ios::binary);
    if (!output) {
        std::cerr << "xex_image: output file could not be opened\n";
        return 1;
    }
    output.write(
            reinterpret_cast<const char*>(image.data.get()),
            static_cast<std::streamsize>(image.size));
    output.close();
    if (!output) {
        std::cerr << "xex_image: output file could not be written completely\n";
        return 1;
    }

    std::cout << "{\n"
              << "  \"schema_version\": 1,\n"
              << "  \"image_base\": " << jsonString(hexAddress(image.base)) << ",\n"
              << "  \"image_size\": " << image.size << ",\n"
              << "  \"entry_point\": " << jsonString(hexAddress(image.entry_point)) << ",\n"
              << "  \"resource_offset\": " << image.resource_offset << ",\n"
              << "  \"resource_size\": " << image.resource_size << ",\n"
              << "  \"sections\": [\n";

    std::size_t index = 0;
    for (const auto& section : image.sections) {
        std::cout << "    {\"name\": " << jsonString(section.name)
                  << ", \"base\": " << jsonString(hexAddress(section.base))
                  << ", \"size\": " << section.size
                  << ", \"code\": "
                  << (((section.flags & SectionFlags_Code) != 0) ? "true" : "false")
                  << "}";
        if (++index != image.sections.size()) {
            std::cout << ',';
        }
        std::cout << '\n';
    }
    std::cout << "  ]\n}\n";
    return 0;
}
