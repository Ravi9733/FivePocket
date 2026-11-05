#include <openssl/aes.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t kKeySize = 32;
constexpr std::size_t kBlockSize = 16;

std::vector<std::uint8_t> readFile(
        const std::filesystem::path& path,
        std::size_t maximumSize = std::numeric_limits<std::size_t>::max()) {
    const auto status = std::filesystem::symlink_status(path);
    if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status)) {
        return {};
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    stream.seekg(0, std::ios::end);
    const auto size = stream.tellg();
    if (size <= 0) {
        return {};
    }
    stream.seekg(0, std::ios::beg);
    const auto bytesToRead = std::min(static_cast<std::size_t>(size), maximumSize);
    std::vector<std::uint8_t> data(bytesToRead);
    stream.read(
            reinterpret_cast<char*>(data.data()),
            static_cast<std::streamsize>(bytesToRead));
    return stream ? data : std::vector<std::uint8_t>{};
}

std::uint32_t readBig32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24U)
            | (static_cast<std::uint32_t>(data[1]) << 16U)
            | (static_cast<std::uint32_t>(data[2]) << 8U)
            | static_cast<std::uint32_t>(data[3]);
}

bool looksLikeRoot(
        const std::array<std::uint8_t, kBlockSize>& record,
        std::uint32_t entryCount,
        std::uint32_t namesSize,
        std::uint32_t nameShift) {
    const std::uint32_t resourceAndOffset =
            (static_cast<std::uint32_t>(record[0]) << 16U)
            | (static_cast<std::uint32_t>(record[1]) << 8U)
            | static_cast<std::uint32_t>(record[2]);
    const std::uint32_t storedSize =
            (static_cast<std::uint32_t>(record[3]) << 16U)
            | (static_cast<std::uint32_t>(record[4]) << 8U)
            | static_cast<std::uint32_t>(record[5]);
    const std::uint32_t nameOffset =
            ((static_cast<std::uint32_t>(record[6]) << 8U) | record[7]) << nameShift;
    const std::uint32_t firstChild = readBig32(record.data() + 8);
    const std::uint32_t childCount = readBig32(record.data() + 12);
    return resourceAndOffset == 0x7FFFFFU
            && storedSize == 0
            && nameOffset < namesSize
            && firstChild <= entryCount
            && childCount > 0
            && childCount <= entryCount - firstChild;
}

bool parseSize(std::string_view text, std::size_t& value) {
    try {
        std::size_t consumed = 0;
        const unsigned long long parsed = std::stoull(std::string(text), &consumed, 0);
        if (consumed != text.size() || parsed > std::numeric_limits<std::size_t>::max()) {
            return false;
        }
        value = static_cast<std::size_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3 || argc > 6) {
        std::cerr << "Usage: rpf7_key_scan_native <decoded-image> <archive> [start] [size] [stride]\n";
        return 2;
    }
    const auto image = readFile(argv[1]);
    const auto archive = readFile(argv[2], 32);
    if (image.size() < kKeySize || archive.size() < 32
            || std::string_view(reinterpret_cast<const char*>(archive.data()), 4) != "RPF7") {
        std::cerr << "rpf7_key_scan_native: invalid image or RPF7 archive\n";
        return 2;
    }

    std::size_t start = 0;
    std::size_t size = image.size();
    std::size_t stride = 1;
    if ((argc >= 4 && !parseSize(argv[3], start))
            || (argc >= 5 && !parseSize(argv[4], size))
            || (argc >= 6 && !parseSize(argv[5], stride))
            || stride == 0 || start > image.size() || size > image.size() - start
            || size < kKeySize) {
        std::cerr << "rpf7_key_scan_native: invalid scan range\n";
        return 2;
    }

    const std::uint32_t entryCount = readBig32(archive.data() + 4);
    const std::uint32_t namesInfo = readBig32(archive.data() + 8);
    const std::uint32_t namesSize = namesInfo & 0x0FFFFFFFU;
    const std::uint32_t nameShift = (namesInfo >> 28U) & 0x7U;
    const auto* encryptedRoot = archive.data() + 16;

    std::size_t scanned = 0;
    std::size_t candidates = 0;
    for (std::size_t offset = start; offset <= start + size - kKeySize; offset += stride) {
        AES_KEY key{};
        if (AES_set_decrypt_key(image.data() + offset, 256, &key) != 0) {
            std::cerr << "rpf7_key_scan_native: AES key setup failed\n";
            return 1;
        }
        std::array<std::uint8_t, kBlockSize> plaintext{};
        AES_decrypt(encryptedRoot, plaintext.data(), &key);
        ++scanned;
        if (looksLikeRoot(plaintext, entryCount, namesSize, nameShift)) {
            std::cout << "candidate_offset=0x" << std::hex << std::uppercase << offset
                      << std::dec << '\n';
            ++candidates;
        }
    }
    std::cout << "windows_scanned=" << scanned << '\n'
              << "candidates_found=" << candidates << '\n';
    return candidates == 0 ? 1 : 0;
}
