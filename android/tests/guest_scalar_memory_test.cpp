#include "guest_execution_guard.h"
#include "trace_prelude.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <stdexcept>

using namespace recomp::bootstrap;

template<class UInt> void testWidth() {
    GuestExecutionGuard guard;
    std::array<std::uint8_t, 32> memory{};
    const UInt value = static_cast<UInt>(0xFEDCBA9876543210ULL);
    for (std::uint32_t offset = 0; offset < 16; ++offset) {
        memory.fill(0xAA);
        storeGuestScalar<UInt>(memory.data(), offset, value);
        assert(loadGuestScalar<UInt>(memory.data(), offset) == value);
        for (std::size_t byte = 0; byte < memory.size(); ++byte) {
            const bool inside = byte >= offset && byte < offset + sizeof(UInt);
            const auto expected = inside
                ? std::uint8_t(std::uint64_t{value} >> ((sizeof(UInt) - 1 - (byte - offset)) * 8))
                : 0xAA;
            assert(memory[byte] == expected);
        }
    }
    for (auto address : {0x7FC80000u, 0x7FC8FFFFu, 0x7FC83214u}) {
        const auto loaded = guard.run(1, [&] { (void)loadGuestScalar<UInt>(nullptr, address); });
        assert(loaded.reason == GuestExecutionStopReason::UnsupportedScalarAccess);
        assert(loaded.scalarAccess && loaded.scalarAccess->address == address);
        assert(loaded.scalarAccess->width == sizeof(UInt) && !loaded.scalarAccess->write);
        assert(loaded.scalarAccess->value == 0 && loaded.scalarAccess->region == GuestScalarRegion::GpuRegisters);
        const auto stored = guard.run(1, [&] { storeGuestScalar<UInt>(nullptr, address, value); });
        assert(stored.scalarAccess && stored.scalarAccess->write && stored.scalarAccess->value == value);
        assert(!stored.importArguments);
    }
    if constexpr (sizeof(UInt) > 1) {
        const auto crossing = guard.run(1, [&] { storeGuestScalar<UInt>(nullptr, 0x7FC80000 - sizeof(UInt) + 1, value); });
        assert(crossing.scalarAccess && crossing.scalarAccess->region == GuestScalarRegion::GpuRegisters);
        const auto wrapped = guard.run(1, [&] { (void)loadGuestScalar<UInt>(nullptr, 0xFFFFFFFF); });
        assert(wrapped.scalarAccess && wrapped.scalarAccess->region == GuestScalarRegion::AddressOverflow);
    }
    assert(!guard.run(1, [] {}).scalarAccess);
}

int main() {
    testWidth<std::uint8_t>(); testWidth<std::uint16_t>();
    testWidth<std::uint32_t>(); testWidth<std::uint64_t>();
    for (const std::uint32_t width : {1, 2, 4, 8}) {
        assert(classifyGuestScalarAccess(0x7FC80000 - width, width) == GuestScalarRegion::Ordinary);
        assert(classifyGuestScalarAccess(0x7FC90000, width) == GuestScalarRegion::Ordinary);
        assert(classifyGuestScalarAccess(0xFFFFFFFF - width + 1, width) == GuestScalarRegion::Ordinary);
    }
    bool rejected = false;
    try { (void)loadGuestScalar<std::uint32_t>(nullptr, 0x7FC80000); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected && !GuestExecutionGuard::isActive());
    std::array<std::uint8_t, 32> bytes{};
    auto* base = bytes.data(); // Same lexical base contract as generated code.
    unsigned addressEvaluations = 0, valueEvaluations = 0;
    const auto address = [&] { ++addressEvaluations; return 3u; };
    const auto value = [&] { ++valueEvaluations; return 0x12345678u; };
    PPC_STORE_U32(address(), value());
    assert(addressEvaluations == 1 && valueEvaluations == 1);
    assert(PPC_LOAD_U32(address()) == 0x12345678 && addressEvaluations == 2);
    PPC_STORE_U8(0, 0xFE); assert(PPC_LOAD_U8(0) == 0xFE);
    PPC_STORE_U16(1, 0xABCD); assert(PPC_LOAD_U16(1) == 0xABCD);
    PPC_STORE_U64(9, 0xFEDCBA9876543210ULL); assert(PPC_LOAD_U64(9) == 0xFEDCBA9876543210ULL);
    GuestExecutionGuard guard;
    assert(guard.run(1, [&] { PPC_STORE_U32(0x7FC83214, 7); }).scalarAccess);
    assert(guard.run(1, [&] { (void)PPC_LOAD_U32(0x7FC83214); }).scalarAccess);
    for (auto invalid : std::vector<std::vector<std::uint32_t>>{{0}, {1}, {4,4}, {0x7FC80000}, std::vector<std::uint32_t>(257, 4)}) {
        assert(!bindGuestScalarDataImportGuard(invalid));
    }
    assert(bindGuestScalarDataImportGuard({0x8000, 0x4000}));
    assert(!bindGuestScalarDataImportGuard({}));
    for (const auto address : {0x3FFDu, 0x4000u, 0x4003u, 0x7FFDu, 0x8000u, 0x8003u}) {
        const auto stopped = guard.run(1, [&] { (void)loadGuestScalar<std::uint32_t>(nullptr, address); });
        assert(stopped.scalarAccess && stopped.scalarAccess->region == GuestScalarRegion::UnboundDataImport);
        assert(stopped.scalarAccess->address == address && !stopped.scalarAccess->write);
        const auto write = guard.run(1, [&] { storeGuestScalar<std::uint32_t>(nullptr, address, 123); });
        assert(write.scalarAccess && write.scalarAccess->write && write.scalarAccess->value == 123);
    }
    for (auto width : {1u, 2u, 4u, 8u}) {
        for (std::uint32_t address = 0x3FF0; address < 0x8010; ++address) {
            const bool expected = (address < 0x4004 && address + width > 0x4000) ||
                (address < 0x8004 && address + width > 0x8000);
            assert(isUnboundGuestDataImportAccess(address, width) == expected);
        }
    }
    assert(!isUnboundGuestDataImportAccess(0x4000, 0));
    resetGuestScalarDataImportGuard();
    assert(!isUnboundGuestDataImportAccess(0x4000, 4));
    assert(bindGuestScalarDataImportGuard({}));
    resetGuestScalarDataImportGuard();
}
