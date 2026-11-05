#include "guest_address_space.h"
#include "guest_title_environment.h"

#include <cassert>
#include <cstdint>
#include <string>

using recomp::bootstrap::GuestAddressSpace;
using recomp::bootstrap::GuestTitleEnvironment;
using recomp::bootstrap::GuestTitleEnvironmentLayout;
using recomp::bootstrap::GuestTitleEnvironmentState;
using recomp::bootstrap::guestTitleEnvironmentStateName;

namespace {

void writeU32(std::uint8_t* memory, std::uint32_t address, std::uint32_t value) {
    memory[address] = static_cast<std::uint8_t>(value >> 24U);
    memory[address + 1] = static_cast<std::uint8_t>(value >> 16U);
    memory[address + 2] = static_cast<std::uint8_t>(value >> 8U);
    memory[address + 3] = static_cast<std::uint8_t>(value);
}

std::uint16_t readU16(const std::uint8_t* memory, std::uint32_t address) {
    return static_cast<std::uint16_t>(memory[address] << 8U) | memory[address + 1];
}

std::uint32_t readU32(const std::uint8_t* memory, std::uint32_t address) {
    return (static_cast<std::uint32_t>(memory[address]) << 24U)
            | (static_cast<std::uint32_t>(memory[address + 1]) << 16U)
            | (static_cast<std::uint32_t>(memory[address + 2]) << 8U)
            | static_cast<std::uint32_t>(memory[address + 3]);
}

GuestTitleEnvironmentLayout validLayout() {
    constexpr std::uint32_t ImageBase = 0x00100000U;
    return {
            ImageBase + 0x200U,
            0x01000000U,
            0x01000100U,
            0x01001000U,
            ImageBase,
            0x00200000U,
            ImageBase + 0x1000U,
    };
}

} // namespace

int main() {
    GuestTitleEnvironment environment;
    assert(environment.result().state == GuestTitleEnvironmentState::Idle);
    assert(!environment.result().succeeded());
    assert(std::string(guestTitleEnvironmentStateName(environment.result().state)) == "idle");

    GuestAddressSpace unavailableAddressSpace;
    assert(!environment.initialize(unavailableAddressSpace, validLayout()));
    assert(environment.result().state
            == GuestTitleEnvironmentState::AddressSpaceUnavailable);

    GuestAddressSpace addressSpace;
    assert(addressSpace.reserve());
    auto* memory = addressSpace.base();
    const auto layout = validLayout();
    constexpr std::uint32_t OriginalImportValue = 0x93010000U;
    writeU32(memory, layout.executableModuleHandleSlot, OriginalImportValue);
    for (std::uint32_t offset = 0;
         offset < GuestTitleEnvironment::ModuleStructureSize;
         ++offset) {
        memory[layout.moduleAddress + offset] = 0xA5;
    }
    memory[layout.xexHeaderAddress] = 0x58;
    memory[layout.xexHeaderAddress + 1] = 0x45;
    memory[layout.xexHeaderAddress + 2] = 0x58;
    memory[layout.xexHeaderAddress + 3] = 0x32;

    assert(environment.initialize(addressSpace, layout));
    assert(environment.result().succeeded());
    assert(environment.result().state == GuestTitleEnvironmentState::Ready);
    assert(environment.result().layout.executableModuleHandleSlot
            == layout.executableModuleHandleSlot);
    assert(environment.result().diagnostic.find("initialized")
            != std::string::npos);
    assert(std::string(guestTitleEnvironmentStateName(environment.result().state)) == "ready");

    assert(readU32(memory, layout.executableModuleHandleSlot)
            == layout.exportedVariableAddress);
    assert(readU32(memory, layout.exportedVariableAddress) == layout.moduleAddress);
    assert(readU32(
            memory,
            layout.moduleAddress + GuestTitleEnvironment::ModuleImageBaseOffset)
            == layout.imageBase);
    assert(readU32(memory, layout.moduleAddress + 0x20U) == 0);
    assert(readU32(
            memory,
            layout.moduleAddress + GuestTitleEnvironment::ModuleFullImageSizeOffset)
            == layout.imageSize);
    assert(readU32(
            memory,
            layout.moduleAddress + GuestTitleEnvironment::ModuleEntryPointOffset)
            == layout.entryPoint);
    assert(readU16(
            memory,
            layout.moduleAddress + GuestTitleEnvironment::ModuleLoadCountOffset)
            == 1);
    assert(readU32(
            memory,
            layout.moduleAddress + GuestTitleEnvironment::ModuleXexHeaderOffset)
            == layout.xexHeaderAddress);
    assert(readU32(memory, layout.xexHeaderAddress) == 0x58455832U);

    environment.reset();
    assert(environment.result().state == GuestTitleEnvironmentState::Idle);
    assert(readU32(memory, layout.executableModuleHandleSlot) == OriginalImportValue);
    assert(readU32(memory, layout.exportedVariableAddress) == 0);
    for (std::uint32_t offset = 0;
         offset < GuestTitleEnvironment::ModuleStructureSize;
         ++offset) {
        assert(memory[layout.moduleAddress + offset] == 0);
    }
    assert(readU32(memory, layout.xexHeaderAddress) == 0x58455832U);

    auto invalid = validLayout();
    invalid.executableModuleHandleSlot += 1;
    assert(!environment.initialize(addressSpace, invalid));
    assert(environment.result().state == GuestTitleEnvironmentState::InvalidRequest);

    invalid = validLayout();
    invalid.moduleAddress = 0xFFFFFFC0U;
    assert(!environment.initialize(addressSpace, invalid));
    assert(environment.result().state == GuestTitleEnvironmentState::AddressRangeInvalid);

    invalid = validLayout();
    invalid.xexHeaderAddress = invalid.imageBase + 0x100U;
    assert(!environment.initialize(addressSpace, invalid));
    assert(environment.result().state == GuestTitleEnvironmentState::AddressOverlap);

    invalid = validLayout();
    invalid.exportedVariableAddress = invalid.moduleAddress;
    assert(!environment.initialize(addressSpace, invalid));
    assert(environment.result().state == GuestTitleEnvironmentState::AddressOverlap);

    invalid = validLayout();
    invalid.executableModuleHandleSlot = invalid.imageBase + invalid.imageSize;
    assert(!environment.initialize(addressSpace, invalid));
    assert(environment.result().state == GuestTitleEnvironmentState::InvalidRequest);

    auto typed = layout;
    typed.threadObjectTypeSlot = layout.imageBase + 0x204U;
    typed.threadObjectTypeAddress = 0x01002000U;
    constexpr std::uint32_t EncodedType = 0x0001001BU;
    writeU32(memory, typed.threadObjectTypeSlot, EncodedType);
    writeU32(memory, typed.threadObjectTypeAddress, 0xA5A5A5A5U);
    assert(environment.initialize(addressSpace, typed));
    assert(readU32(memory, typed.threadObjectTypeSlot) == typed.threadObjectTypeAddress);
    assert(readU32(memory, typed.threadObjectTypeAddress) == 0);
    assert(readU32(memory, layout.executableModuleHandleSlot) == layout.exportedVariableAddress);
    environment.reset();
    assert(readU32(memory, typed.threadObjectTypeSlot) == EncodedType);
    assert(readU32(memory, layout.executableModuleHandleSlot) == OriginalImportValue);
    writeU32(memory, typed.threadObjectTypeSlot, 0x1B000100U);
    assert(environment.initialize(addressSpace, typed));
    assert(readU32(memory, typed.threadObjectTypeSlot) == typed.threadObjectTypeAddress);
    environment.reset();
    assert(readU32(memory, typed.threadObjectTypeSlot) == 0x1B000100U);
    writeU32(memory, typed.threadObjectTypeSlot, EncodedType);
    for (auto alter : {0, 1, 2, 3, 4, 5, 6, 7}) {
        invalid = typed;
        if (alter == 0) { invalid.threadObjectTypeSlot = 0; }
        if (alter == 1) { invalid.threadObjectTypeAddress = 0; }
        if (alter == 2) { invalid.threadObjectTypeSlot = layout.executableModuleHandleSlot; }
        if (alter == 3) { invalid.threadObjectTypeAddress = layout.moduleAddress + 4; }
        if (alter == 4) { invalid.threadObjectTypeAddress = layout.imageBase; }
        if (alter == 5) { invalid.threadObjectTypeSlot = layout.imageBase + layout.imageSize; }
        if (alter == 6) { invalid.threadObjectTypeAddress |= 1; }
        if (alter == 7) { invalid.threadObjectTypeSlot |= 1; }
        assert(!environment.initialize(addressSpace, invalid));
        assert(readU32(memory, typed.threadObjectTypeSlot) == EncodedType);
        assert(readU32(memory, layout.executableModuleHandleSlot) == OriginalImportValue);
    }
    for (auto word : {0x0000001AU, 0x0100001BU, 0x1A000100U, 0x1B000101U, 0xD01BBEEFU}) {
        writeU32(memory, typed.threadObjectTypeSlot, word);
        assert(!environment.initialize(addressSpace, typed));
        assert(readU32(memory, typed.threadObjectTypeSlot) == word);
        assert(readU32(memory, layout.executableModuleHandleSlot) == OriginalImportValue);
    }

    return 0;
}
