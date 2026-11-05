#include "guest_address_space.h"
#include "guest_image_loader.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

using recomp::bootstrap::GuestAddressSpace;
using recomp::bootstrap::GuestImageLoader;
using recomp::bootstrap::GuestImageLoadState;
using recomp::bootstrap::guestImageLoadStateName;

namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto nonce = std::chrono::high_resolution_clock::now()
                                   .time_since_epoch()
                                   .count();
        for (int attempt = 0; attempt < 100; ++attempt) {
            path_ = std::filesystem::temp_directory_path()
                    / ("recomp-guest-image-loader-" + std::to_string(nonce)
                            + '-' + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
                return;
            }
        }
        assert(false && "could not create synthetic test directory");
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void writeSyntheticImage(
        const std::filesystem::path& path,
        const std::array<std::uint8_t, 32>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output.is_open());
    output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    assert(output.good());
}

} // namespace

int main() {
    TemporaryDirectory temporary;
    const auto imagePath = temporary.path() / "synthetic-decoded-image.bin";
    const std::array<std::uint8_t, 32> imageBytes{
            0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
            0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
            0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10,
            0x0F, 0x1E, 0x2D, 0x3C, 0x4B, 0x5A, 0x69, 0x78,
    };
    writeSyntheticImage(imagePath, imageBytes);

    GuestImageLoader loader;
    assert(loader.result().state == GuestImageLoadState::Idle);
    assert(!loader.result().succeeded());
    assert(std::string(guestImageLoadStateName(loader.result().state)) == "idle");

    GuestAddressSpace unavailableAddressSpace;
    assert(!loader.loadExplicitDecodedImage(
            unavailableAddressSpace, imagePath, 0x10000U, imageBytes.size()));
    assert(loader.result().state == GuestImageLoadState::AddressSpaceUnavailable);
    assert(loader.result().bytesLoaded == 0);

    GuestAddressSpace addressSpace;
    assert(addressSpace.reserve());
    constexpr std::uint32_t GuestBase = 0x20000U;
    assert(loader.loadExplicitDecodedImage(
            addressSpace, imagePath, GuestBase, imageBytes.size()));
    assert(loader.result().succeeded());
    assert(loader.result().state == GuestImageLoadState::Loaded);
    assert(loader.result().guestBase == GuestBase);
    assert(loader.result().expectedSize == imageBytes.size());
    assert(loader.result().observedFileSize == imageBytes.size());
    assert(loader.result().bytesRead == imageBytes.size());
    assert(loader.result().bytesLoaded == imageBytes.size());
    assert(loader.result().diagnostic.find("execution remains disabled") != std::string::npos);
    for (std::size_t index = 0; index < imageBytes.size(); ++index) {
        assert(addressSpace.translate(GuestBase)[index] == imageBytes[index]);
    }

    assert(!loader.loadExplicitDecodedImage(
            addressSpace, imagePath, GuestBase + 0x100U, imageBytes.size() - 1U));
    assert(loader.result().state == GuestImageLoadState::SizeMismatch);
    assert(loader.result().observedFileSize == imageBytes.size());
    assert(loader.result().bytesRead == 0);
    assert(loader.result().bytesLoaded == 0);

    assert(!loader.loadExplicitDecodedImage(
            addressSpace, imagePath, 0xFFFFFFF0U, imageBytes.size()));
    assert(loader.result().state == GuestImageLoadState::AddressRangeInvalid);
    assert(loader.result().bytesRead == 0);

    assert(!loader.loadExplicitDecodedImage(addressSpace, imagePath, GuestBase, 0));
    assert(loader.result().state == GuestImageLoadState::InvalidRequest);

    assert(!loader.loadExplicitDecodedImage(
            addressSpace,
            std::filesystem::path("relative-decoded-image.bin"),
            GuestBase,
            imageBytes.size()));
    assert(loader.result().state == GuestImageLoadState::InvalidPath);

    assert(!loader.loadExplicitDecodedImage(
            addressSpace,
            temporary.path() / "missing-decoded-image.bin",
            GuestBase,
            imageBytes.size()));
    assert(loader.result().state == GuestImageLoadState::OpenFailed);

    assert(!loader.loadExplicitDecodedImage(
            addressSpace, temporary.path(), GuestBase, imageBytes.size()));
    assert(loader.result().state == GuestImageLoadState::NotRegularFile);

    const auto symlinkPath = temporary.path() / "decoded-image-link.bin";
    std::error_code symlinkError;
    std::filesystem::create_symlink(imagePath, symlinkPath, symlinkError);
    assert(!symlinkError);
    assert(!loader.loadExplicitDecodedImage(
            addressSpace, symlinkPath, GuestBase, imageBytes.size()));
    assert(loader.result().state == GuestImageLoadState::SymlinkRejected);
    assert(loader.result().bytesLoaded == 0);

    loader.reset();
    assert(loader.result().state == GuestImageLoadState::Idle);
    assert(loader.result().guestBase == 0);
    assert(loader.result().expectedSize == 0);
    assert(loader.result().bytesRead == 0);
    assert(loader.result().diagnostic == "No decoded image load requested");
}
