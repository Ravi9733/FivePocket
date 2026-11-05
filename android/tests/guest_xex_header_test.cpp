#include "guest_xex_header.h"

#include <cstdint>
#include <cstring>

using recomp::bootstrap::GuestAddressSpace;
using recomp::bootstrap::GuestXexHeader;
using recomp::bootstrap::GuestXexHeaderLookupState;
using recomp::bootstrap::GuestXexHeaderValueKind;

namespace {

constexpr std::uint32_t HeaderAddress = 0x20000000U;

void writeU32(std::uint8_t* base, std::uint32_t address, std::uint32_t value) {
    auto* bytes = base + address;
    bytes[0] = static_cast<std::uint8_t>(value >> 24U);
    bytes[1] = static_cast<std::uint8_t>(value >> 16U);
    bytes[2] = static_cast<std::uint8_t>(value >> 8U);
    bytes[3] = static_cast<std::uint8_t>(value);
}

void initializeHeader(GuestAddressSpace& addressSpace) {
    auto* base = addressSpace.base();
    std::memcpy(base + HeaderAddress, "XEX2", 4);
    writeU32(base, HeaderAddress + 8, 0x200);
    writeU32(base, HeaderAddress + 20, 3);
    writeU32(base, HeaderAddress + 24, 0x00010000);
    writeU32(base, HeaderAddress + 28, 0x12345678);
    writeU32(base, HeaderAddress + 32, 0x00020401);
    writeU32(base, HeaderAddress + 36, 0x00100000);
    writeU32(base, HeaderAddress + 40, 0x00040006);
    writeU32(base, HeaderAddress + 44, 0x100);
}

} // namespace

int main() {
    GuestAddressSpace addressSpace;
    if (!addressSpace.reserve()) {
        return 1;
    }
    initializeHeader(addressSpace);

    const auto immediate = GuestXexHeader::lookup(
            addressSpace, HeaderAddress, 0x00010000);
    if (!immediate.found()
            || immediate.kind != GuestXexHeaderValueKind::Immediate
            || immediate.value != 0x12345678) {
        return 2;
    }

    const auto valuePointer = GuestXexHeader::lookup(
            addressSpace, HeaderAddress, 0x00020401);
    if (!valuePointer.found()
            || valuePointer.kind != GuestXexHeaderValueKind::ValuePointer
            || valuePointer.value != HeaderAddress + 36) {
        return 3;
    }

    const auto dataPointer = GuestXexHeader::lookup(
            addressSpace, HeaderAddress, 0x00040006);
    if (!dataPointer.found()
            || dataPointer.kind != GuestXexHeaderValueKind::DataPointer
            || dataPointer.value != HeaderAddress + 0x100) {
        return 4;
    }

    const auto missing = GuestXexHeader::lookup(
            addressSpace, HeaderAddress, 0x00010201);
    if (missing.state != GuestXexHeaderLookupState::NotFound
            || missing.value != 0) {
        return 5;
    }

    writeU32(addressSpace.base(), HeaderAddress + 20, 4097);
    const auto tooMany = GuestXexHeader::lookup(
            addressSpace, HeaderAddress, 0x00010000);
    if (tooMany.state != GuestXexHeaderLookupState::InvalidHeader) {
        return 6;
    }

    initializeHeader(addressSpace);
    writeU32(addressSpace.base(), HeaderAddress + 44, 0x200);
    const auto outOfRange = GuestXexHeader::lookup(
            addressSpace, HeaderAddress, 0x00040006);
    if (outOfRange.state != GuestXexHeaderLookupState::InvalidHeader) {
        return 7;
    }

    initializeHeader(addressSpace);
    std::memcpy(addressSpace.base() + HeaderAddress, "BAD!", 4);
    const auto badMagic = GuestXexHeader::lookup(
            addressSpace, HeaderAddress, 0x00010000);
    if (badMagic.state != GuestXexHeaderLookupState::InvalidHeader) {
        return 8;
    }

    return 0;
}
