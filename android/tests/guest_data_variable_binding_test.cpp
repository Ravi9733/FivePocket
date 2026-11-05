#include "guest_data_variable_binding.h"
#include "guest_kernel_version.h"
#include <cassert>
#include <algorithm>
using namespace recomp::bootstrap;
int main() {
    GuestAddressSpace space; GuestDataVariableBinding binding;
    const std::vector<std::uint8_t> profile{0,0,0,0x20,6,0,0,0,0,0,0,0,0,0,0,0};
    auto bind = [&](std::uint32_t slot = 0x4000, std::uint32_t storage = 0x10000) {
        return binding.initialize(space, {0x4000, 0x1000}, slot, 0x156, storage, profile, {{0x6000, 0x1000}});
    };
    assert(!bind()); assert(space.reserve());
    const std::uint8_t descriptor[]{0x56, 0x01, 0x01, 0};
    std::memcpy(space.translate(0x4000), descriptor, 4);
    for (auto address : {0u, 0x4000u, 0x6000u, 0x5FFCu, 0x10001u, 0xFFFFFFF4u}) assert(!bind(0x4000, address));
    for (auto slot : {0x3FFCu, 0x4001u, 0x5000u}) assert(!bind(slot));
    assert(!binding.initialize(space, {0x4000, 3}, 0x4000, 0x156, 0x10000, profile, {}));
    assert(!binding.initialize(space, {0x4000, 0x1000}, 0x4000, 0x157, 0x10000, profile, {}));
    assert(!binding.initialize(space, {0x4000, 0x1000}, 0x4000, 0x156, 0x10000, {}, {}));
    assert(!binding.initialize(space, {0x4000, 0x1000}, 0x4000, 0x156, 0x10000, std::vector<std::uint8_t>(65), {}));
    space.translate(0x1000F)[0] = 1; assert(!bind()); space.translate(0x1000F)[0] = 0;
    assert(std::memcmp(space.translate(0x4000), descriptor, 4) == 0);
    assert(bind()); assert(!bind());
    assert(space.translate(0x4000)[1] == 1 && space.translate(0x4000)[0] == 0);
    assert(!std::memcmp(space.translate(0x10000), profile.data(), 16));
    binding.reset(); binding.reset();
    assert(!std::memcmp(space.translate(0x4000), descriptor, 4));
    for (unsigned i = 0; i < 16; ++i) assert(!space.translate(0x10000)[i]);
    { GuestDataVariableBinding scope;
      assert(scope.initialize(space, {0x4000, 0x1000}, 0x4000, 0x156, 0x10000, profile, {})); }
    assert(!std::memcmp(space.translate(0x4000), descriptor, 4));
    // Native no-Xbox-debugger profile: both endian descriptor encodings,
    // two indirections, exact four-byte extent and reversible publication.
    const auto read = [&](std::uint32_t address) {
        const auto* b = space.translate(address);
        return (std::uint32_t{b[0]} << 24) | (std::uint32_t{b[1]} << 16) |
               (std::uint32_t{b[2]} << 8) | b[3];
    };
    const auto version = guestKernelVersionBytes(0x203F4A00);
    const std::array<std::uint8_t, 8> expectedVersion{0,2,0,0,0x3F,0x4A,0,0};
    assert(version && *version == expectedVersion);
    assert(!guestKernelVersionBytes(0x103F4A00));
    assert(!guestKernelVersionBytes(0x303F4A00));
    assert((*guestKernelVersionBytes(0x21ABCD12) ==
        std::array<std::uint8_t, 8>{0,2,0,1,0xAB,0xCD,0,0x12}));
    const std::uint8_t kernelDescriptor[]{0x58,0x01,0x01,0};
    std::memcpy(space.translate(0x4000), kernelDescriptor, 4);
    space.translate(0x1FFFF)[0] = 0xA5; space.translate(0x20008)[0] = 0x5A;
    assert(binding.initialize(space, {0x4000,0x1000}, 0x4000, 0x158, 0x20000,
        std::vector<std::uint8_t>(version->begin(), version->end()), {}));
    assert(read(0x4000) == 0x20000);
    assert(std::memcmp(space.translate(0x20000), expectedVersion.data(), 8) == 0);
    binding.reset();
    assert(std::memcmp(space.translate(0x4000), kernelDescriptor, 4) == 0);
    assert(space.translate(0x1FFFF)[0] == 0xA5 && space.translate(0x20008)[0] == 0x5A);
    for (const auto ordinal : {0x59u,0x266u}) for (const auto little : {false,true}) {
        std::vector<std::uint8_t> bytes{0,1,static_cast<std::uint8_t>(ordinal >> 8),static_cast<std::uint8_t>(ordinal)};
        if (little) std::reverse(bytes.begin(),bytes.end());
        std::memcpy(space.translate(0x4000), bytes.data(), 4);
        space.translate(0xFFFF)[0] = 0xA5; space.translate(0x10004)[0] = 0x5A;
        assert(!binding.initialize(space, {0x4000, 0x1000}, 0x4000, 0x5A, 0x10000, {0,0,0,0}, {}));
        assert(binding.initialize(space, {0x4000, 0x1000}, 0x4000, ordinal, 0x10000, {0,0,0,0}, {}));
        assert(read(0x4000) == 0x10000 && read(read(0x4000)) == 0);
        binding.reset();
        assert(!std::memcmp(space.translate(0x4000), bytes.data(), 4));
        assert(read(0x10000) == 0);
        assert(space.translate(0xFFFF)[0] == 0xA5 && space.translate(0x10004)[0] == 0x5A);
    }
}
