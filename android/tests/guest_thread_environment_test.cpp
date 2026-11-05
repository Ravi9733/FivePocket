#include "guest_address_space.h"
#include "guest_thread_environment.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

using recomp::bootstrap::GuestAddressSpace;
using recomp::bootstrap::GuestThreadEnvironment;
using recomp::bootstrap::GuestThreadEnvironmentLayout;
using recomp::bootstrap::GuestThreadEnvironmentState;
using recomp::bootstrap::guestThreadEnvironmentStateName;

static_assert(!std::is_copy_constructible_v<GuestThreadEnvironment>);
static_assert(!std::is_copy_assignable_v<GuestThreadEnvironment>);
static_assert(!std::is_move_constructible_v<GuestThreadEnvironment>);
static_assert(!std::is_move_assignable_v<GuestThreadEnvironment>);

namespace {

constexpr std::array<std::uint8_t, 11> Template{
        0x81, 0x00, 0xFE, 0x7F, 0x12, 0x34, 0x56, 0x78, 0xA5, 0x5A, 0xFF};

GuestThreadEnvironmentLayout validLayout() {
    return {
            0x01000000U,
            0x01001000U,
            0x01002000U,
            0x00100000U,
            0x00001000U,
            0x02000000U,
            0x00010000U,
            0x00100123U, // A byte template need not be word-aligned.
            static_cast<std::uint32_t>(Template.size()),
            0x20U,
            4U,
            0x10203040U,
    };
}

void writeU32(std::uint8_t* bytes, std::uint32_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value >> 24U);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 16U);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 8U);
    bytes[offset + 3] = static_cast<std::uint8_t>(value);
}

void assertFilled(const std::uint8_t* bytes, std::size_t size, std::uint8_t value) {
    assert(std::all_of(bytes, bytes + size, [value](std::uint8_t byte) {
        return byte == value;
    }));
}

struct ScratchRange {
    std::uint32_t address;
    std::uint32_t size;
};

std::array<ScratchRange, 3> scratchRanges(const GuestThreadEnvironmentLayout& layout) {
    return {{{layout.pcrAddress, GuestThreadEnvironment::PcrSize},
             {layout.threadAddress, GuestThreadEnvironment::ThreadSize},
             {layout.tlsAddress, layout.tlsDataSize + layout.tlsSlotCount * 4U}}};
}

void assertUnchangedInputs(
        const GuestAddressSpace& addressSpace,
        const GuestThreadEnvironmentLayout& layout,
        const std::vector<std::uint8_t>& image) {
    assert(std::memcmp(addressSpace.translate(layout.imageBase), image.data(), image.size()) == 0);
    assertFilled(addressSpace.translate(layout.stackBase), layout.stackSize, 0xC3);
}

void testValidAndReset(GuestAddressSpace& addressSpace) {
    GuestThreadEnvironment environment;
    const auto layout = validLayout();
    const auto ranges = scratchRanges(layout);
    std::memset(addressSpace.translate(layout.imageBase), 0xA5, layout.imageSize);
    std::memcpy(addressSpace.translate(layout.tlsRawDataAddress), Template.data(), Template.size());
    const std::vector<std::uint8_t> image(
            addressSpace.translate(layout.imageBase),
            addressSpace.translate(layout.imageBase) + layout.imageSize);
    std::memset(addressSpace.translate(layout.stackBase), 0xC3, layout.stackSize);
    for (const auto& range : ranges) {
        std::memset(addressSpace.translate(range.address - 1), 0xD7, range.size + 2U);
    }

    assert(environment.initialize(addressSpace, layout));
    assert(environment.ready());
    assert(environment.result().succeeded());
    assert(environment.result().layout.threadId == layout.threadId);
    assert(std::string(guestThreadEnvironmentStateName(environment.result().state)) == "ready");
    assert(environment.dynamicTlsAddress() == layout.tlsAddress + layout.tlsDataSize);
    assert(environment.tlsStorageSize() == 0x30U);

    // Compare every byte, including all fields intentionally left zero.
    std::array<std::uint8_t, GuestThreadEnvironment::PcrSize> expectedPcr{};
    writeU32(expectedPcr.data(), 0x00U, layout.tlsAddress);
    writeU32(expectedPcr.data(), 0x30U, layout.pcrAddress);
    writeU32(expectedPcr.data(), 0x70U, layout.stackBase + layout.stackSize);
    writeU32(expectedPcr.data(), 0x74U, layout.stackBase);
    writeU32(expectedPcr.data(), 0x100U, layout.threadAddress);
    assert(std::memcmp(addressSpace.translate(layout.pcrAddress),
            expectedPcr.data(), expectedPcr.size()) == 0);

    std::array<std::uint8_t, GuestThreadEnvironment::ThreadSize> expectedThread{};
    writeU32(expectedThread.data(), 0x5CU, layout.stackBase + layout.stackSize);
    writeU32(expectedThread.data(), 0x60U, layout.stackBase);
    writeU32(expectedThread.data(), 0x68U, layout.tlsAddress);
    writeU32(expectedThread.data(), 0x14CU, layout.threadId);
    assert(std::memcmp(addressSpace.translate(layout.threadAddress),
            expectedThread.data(), expectedThread.size()) == 0);
    const auto* tls = addressSpace.translate(layout.tlsAddress);
    assert(std::memcmp(tls, Template.data(), Template.size()) == 0);
    assertFilled(tls + Template.size(), environment.tlsStorageSize() - Template.size(), 0);
    for (const auto& range : ranges) {
        assert(*addressSpace.translate(range.address - 1) == 0xD7);
        assert(*addressSpace.translate(range.address + range.size) == 0xD7);
    }
    assertUnchangedInputs(addressSpace, layout, image);

    // Ownership cannot silently move or overwrite a live environment.
    auto replacement = layout;
    replacement.pcrAddress = layout.tlsAddress;
    assert(!environment.initialize(addressSpace, replacement));
    assert(environment.ready());
    assert(environment.result().layout.pcrAddress == layout.pcrAddress);
    assert(std::memcmp(addressSpace.translate(layout.pcrAddress),
            expectedPcr.data(), expectedPcr.size()) == 0);
    assert(std::memcmp(tls, Template.data(), Template.size()) == 0);

    writeU32(addressSpace.translate(environment.dynamicTlsAddress()), 12U, 0xAABBCCDDU);
    writeU32(addressSpace.translate(layout.threadAddress), 0x160U, 0x12345678U);
    environment.reset();
    assert(!environment.ready());
    assert(environment.result().state == GuestThreadEnvironmentState::Idle);
    assert(environment.dynamicTlsAddress() == 0);
    assert(environment.tlsStorageSize() == 0);
    for (const auto& range : ranges) {
        assertFilled(addressSpace.translate(range.address), range.size, 0);
        assert(*addressSpace.translate(range.address - 1) == 0xD7);
        assert(*addressSpace.translate(range.address + range.size) == 0xD7);
    }
    assertUnchangedInputs(addressSpace, layout, image);
    environment.reset();
    assert(environment.initialize(addressSpace, layout));
    environment.reset();
}

void testInvalidRequests(GuestAddressSpace& addressSpace) {
    const auto layout = validLayout();
    struct InvalidCase {
        GuestThreadEnvironmentLayout layout;
        GuestThreadEnvironmentState state;
    };
    std::vector<InvalidCase> cases;
    const auto add = [&](auto alter, GuestThreadEnvironmentState state) {
        auto invalid = layout;
        alter(invalid);
        cases.push_back({invalid, state});
    };
    using State = GuestThreadEnvironmentState;
    for (auto field : {&GuestThreadEnvironmentLayout::pcrAddress,
                       &GuestThreadEnvironmentLayout::threadAddress,
                       &GuestThreadEnvironmentLayout::tlsAddress,
                       &GuestThreadEnvironmentLayout::stackBase,
                       &GuestThreadEnvironmentLayout::stackSize}) {
        add([field](auto& value) { value.*field = 0; }, State::InvalidRequest);
        add([field](auto& value) { value.*field |= 1U; }, State::InvalidRequest);
    }
    add([](auto& v) { v.imageSize = 0; }, State::InvalidRequest);
    add([](auto& v) { v.threadId = 0; }, State::InvalidRequest);
    add([](auto& v) { v.tlsDataSize |= 1U; }, State::InvalidRequest);
    add([](auto& v) { v.tlsDataSize = GuestThreadEnvironment::MaxStaticTlsSize + 4U; },
            State::InvalidRequest);
    add([](auto& v) { v.tlsSlotCount = GuestThreadEnvironment::MaxTlsSlots + 1U; },
            State::InvalidRequest);
    add([](auto& v) { v.tlsSlotCount = 0xFFFFFFFFU; }, State::InvalidRequest);
    add([](auto& v) { v.tlsRawDataSize = v.tlsDataSize + 1U; }, State::InvalidRequest);
    add([](auto& v) { v.tlsRawDataAddress = 0; }, State::InvalidRequest);
    add([](auto& v) { v.tlsRawDataSize = v.tlsDataSize = v.tlsSlotCount = 0; },
            State::InvalidRequest);

    for (auto field : {&GuestThreadEnvironmentLayout::pcrAddress,
                       &GuestThreadEnvironmentLayout::threadAddress,
                       &GuestThreadEnvironmentLayout::tlsAddress,
                       &GuestThreadEnvironmentLayout::imageBase,
                       &GuestThreadEnvironmentLayout::stackBase}) {
        add([field](auto& v) { v.*field = 0xFFFFFFFCU; }, State::AddressRangeInvalid);
    }
    add([](auto& v) { v.stackBase = 0xFFFF0000U; }, State::AddressRangeInvalid);
    add([](auto& v) { v.tlsRawDataAddress = 0xFFFFFFFCU; }, State::AddressRangeInvalid);
    add([](auto& v) { v.tlsRawDataAddress = v.imageBase - 1U; }, State::AddressRangeInvalid);
    add([](auto& v) { v.tlsRawDataAddress = v.imageBase + v.imageSize - 1U; },
            State::AddressRangeInvalid);

    for (auto scratch : {&GuestThreadEnvironmentLayout::pcrAddress,
                         &GuestThreadEnvironmentLayout::threadAddress,
                         &GuestThreadEnvironmentLayout::tlsAddress}) {
        add([scratch](auto& v) { v.*scratch = v.imageBase; }, State::AddressOverlap);
        add([scratch](auto& v) { v.*scratch = v.imageBase - 4U; }, State::AddressOverlap);
        add([scratch](auto& v) { v.*scratch = v.stackBase; }, State::AddressOverlap);
        add([scratch](auto& v) { v.*scratch = v.stackBase + v.stackSize - 4U; },
                State::AddressOverlap);
    }
    add([](auto& v) { v.threadAddress = v.pcrAddress; }, State::AddressOverlap);
    add([](auto& v) { v.threadAddress = v.pcrAddress + GuestThreadEnvironment::PcrSize - 4U; },
            State::AddressOverlap);
    add([](auto& v) { v.pcrAddress = v.threadAddress + GuestThreadEnvironment::ThreadSize - 4U; },
            State::AddressOverlap);
    add([](auto& v) { v.tlsAddress = v.pcrAddress; }, State::AddressOverlap);
    add([](auto& v) { v.tlsAddress = v.threadAddress; }, State::AddressOverlap);
    add([](auto& v) { v.stackBase = v.imageBase; }, State::AddressOverlap);

    const std::array<ScratchRange, 5> observed{{
            {layout.pcrAddress - 4U, GuestThreadEnvironment::PcrSize + 8U},
            {layout.threadAddress - 4U, GuestThreadEnvironment::ThreadSize + 8U},
            {layout.tlsAddress - 4U, 0x38U},
            {layout.imageBase - 4U, layout.imageSize + 8U},
            {layout.stackBase - 4U, layout.stackSize + 8U},
    }};
    for (const auto& test : cases) {
        for (const auto& range : observed) {
            std::memset(addressSpace.translate(range.address), 0x9B, range.size);
        }
        GuestThreadEnvironment environment;
        assert(!environment.initialize(addressSpace, test.layout));
        assert(!environment.ready());
        assert(!environment.result().succeeded());
        assert(environment.result().state == test.state);
        assert(environment.dynamicTlsAddress() == 0);
        assert(environment.tlsStorageSize() == 0);
        environment.reset();
        for (const auto& range : observed) {
            assertFilled(addressSpace.translate(range.address), range.size, 0x9B);
        }
    }
}

void testBoundsAndEmptyTemplate(GuestAddressSpace& addressSpace) {
    GuestThreadEnvironment environment;
    auto layout = validLayout();
    layout.tlsRawDataAddress = layout.tlsRawDataSize = layout.tlsDataSize = 0;
    layout.tlsSlotCount = 1;
    layout.tlsAddress = 0xFFFFFFFCU;
    assert(environment.initialize(addressSpace, layout));
    assert(environment.dynamicTlsAddress() == 0xFFFFFFFCU);
    assert(environment.tlsStorageSize() == 4);
    assertFilled(addressSpace.translate(layout.tlsAddress), 4, 0);
    environment.reset();

    layout = validLayout();
    layout.tlsSlotCount = 0;
    layout.tlsRawDataSize = 0;
    assert(environment.initialize(addressSpace, layout));
    assert(environment.dynamicTlsAddress() == 0);
    assert(environment.tlsStorageSize() == layout.tlsDataSize);
    assertFilled(addressSpace.translate(layout.tlsAddress), layout.tlsDataSize, 0);
    environment.reset();

    layout = validLayout();
    layout.tlsDataSize = GuestThreadEnvironment::MaxStaticTlsSize;
    layout.tlsSlotCount = GuestThreadEnvironment::MaxTlsSlots;
    std::memcpy(addressSpace.translate(layout.tlsRawDataAddress), Template.data(), Template.size());
    assert(environment.initialize(addressSpace, layout));
    assert(environment.tlsStorageSize() == GuestThreadEnvironment::MaxStaticTlsSize + 8192U);
    assert(environment.dynamicTlsAddress() == layout.tlsAddress + layout.tlsDataSize);
    assert(std::memcmp(addressSpace.translate(layout.tlsAddress), Template.data(), Template.size()) == 0);
    assertFilled(addressSpace.translate(layout.tlsAddress) + Template.size(),
            environment.tlsStorageSize() - Template.size(), 0);
    environment.reset();

    // Adjacent scratch ranges have no overlap, including the PCR/thread boundary.
    layout = validLayout();
    layout.threadAddress = layout.pcrAddress + GuestThreadEnvironment::PcrSize;
    layout.tlsAddress = layout.threadAddress + GuestThreadEnvironment::ThreadSize;
    assert(environment.initialize(addressSpace, layout));
    environment.reset();
}

} // namespace

int main() {
    GuestThreadEnvironment environment;
    assert(!environment.ready());
    assert(environment.result().state == GuestThreadEnvironmentState::Idle);
    assert(!environment.result().succeeded());
    assert(std::string(guestThreadEnvironmentStateName(environment.result().state)) == "idle");
    assert(environment.dynamicTlsAddress() == 0);
    assert(environment.tlsStorageSize() == 0);
    GuestAddressSpace unavailable;
    assert(!environment.initialize(unavailable, validLayout()));
    assert(environment.result().state == GuestThreadEnvironmentState::AddressSpaceUnavailable);

    GuestAddressSpace addressSpace;
    assert(addressSpace.reserve());
    testValidAndReset(addressSpace);
    testInvalidRequests(addressSpace);
    testBoundsAndEmptyTemplate(addressSpace);

    assert(environment.initialize(addressSpace, validLayout()));
    addressSpace.release();
    assert(!environment.ready());
    assert(environment.dynamicTlsAddress() == 0);
    assert(environment.tlsStorageSize() == 0);
    environment.reset(); // Releasing the address space first does not dereference it.
    return 0;
}
