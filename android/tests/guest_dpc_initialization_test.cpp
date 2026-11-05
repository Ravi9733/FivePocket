#include "guest_import_registry.h"
#include <algorithm>
#include <array>
#include <cassert>

using namespace recomp::bootstrap;
int main() {
    GuestAddressSpace space; assert(space.reserve());
    GuestImportRegistry registry;
    auto validator = [](auto a) { return a == 0x4000; };
    assert(!registry.bindDpcInitialization(validator));
    assert(registry.initialize(space, 0x10000000, 0x10000));
    auto init = [&](std::uint32_t address, std::uint32_t routine, bool expected) {
        GuestImportCall call; call.arguments[0] = address; call.arguments[1] = routine;
        call.arguments[2] = 0xFEDCBA98; call.returnValue = 0x123456789ABCDEF0;
        assert(registry.dispatch("__imp__KeInitializeDpc", call).handled() == expected);
        assert(call.returnValue == 0x123456789ABCDEF0);
    };
    init(0x10000000, 0x4000, false);
    assert(!registry.bindDpcInitialization({}));
    assert(registry.bindDpcInitialization(validator));
    assert(!registry.bindDpcInitialization(validator));
    assert(registry.bindPhysicalMemoryArena(0xA0000000, 0x10000, 0));
    auto allocate = [&](std::uint32_t protection) {
        GuestImportCall c; c.arguments = {1, 0x1000, protection, 0, UINT32_MAX, 0};
        assert(registry.dispatch("__imp__MmAllocatePhysicalMemoryEx", c).handled());
        assert(c.returnValue); return static_cast<std::uint32_t>(c.returnValue);
    };
    const auto owner = allocate(4), ro = allocate(2);
    auto* bytes = space.translate(owner);
    std::fill_n(bytes, 0x1000, 0xA5);
    for (auto bad : {0U, owner + 1, owner + 0x1000 - 24, 0xA000E000U, ro, 0x7FC8112CU, 0xFFFFFFF0U})
        init(bad, 0x4000, false);
    for (auto bad : {0U, 0x4001U, 0x4004U, 0xFFFFFFFFU}) init(owner + 4, bad, false);
    assert(std::all_of(bytes, bytes + 0x1000, [](auto b) { return b == 0xA5; }));
    for (const auto offset : {4U, 0x1000U - 28}) {
        std::fill_n(bytes, 0x1000, 0xA5); init(owner + offset, 0x4000, true);
        constexpr std::array<std::uint32_t, 7> words{0x13000000, 0, 0, 0x4000, 0xFEDCBA98, 0, 0};
        for (unsigned i = 0; i < 0x1000; ++i) {
            const bool in = i >= offset && i < offset + 28;
            const auto expected = in ? std::uint8_t(words[(i-offset)/4] >> ((3-(i-offset)%4)*8)) : 0xA5;
            assert(bytes[i] == expected);
        }
    }
    // No host queue state: reinitializing just resets the same bounded record.
    init(owner + 4, 0x4000, true);
    registry.unbindPhysicalMemoryArena(); init(owner + 4, 0x4000, false);
    // The title's current DPC record is inside the portable virtual arena.
    auto put = [&](std::uint32_t a, std::uint32_t v) {
        auto* p = space.translate(a);
        for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (24-i*8));
    };
    put(0x3000, 0); put(0x3004, 0x1000);
    GuestImportCall alloc; alloc.arguments = {0x3000, 0x3004, 0x3000, 4, 0};
    assert(registry.dispatch("__imp__NtAllocateVirtualMemory", alloc).handled() && alloc.returnValue == 0);
    std::fill_n(space.translate(0x10000000), 0x1000, 0xA5);
    init(0x10000FE4, 0x4000, true); // Exact allocation end is accepted.
    assert(space.translate(0x10000FE4)[0] == 0x13 && space.translate(0x10000FE4)[12] == 0);
    init(0x10000FE8, 0x4000, false);
    registry.reset(); assert(registry.initialize(space, 0x10000000, 0x10000));
    init(0x10000000, 0x4000, false); assert(registry.bindDpcInitialization(validator));
}
