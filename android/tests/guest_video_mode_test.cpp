#include "guest_import_registry.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>

using namespace recomp::bootstrap;
void testQuery(GuestAddressSpace& space, const char* name) {
    GuestImportRegistry registry;
    assert(!registry.bindVirtualVideoMode720p60());
    assert(registry.initialize(space, 0x10000000, 0x10000));
    const auto* descriptor = &*std::find_if(registry.implementedBootImports().begin(),
            registry.implementedBootImports().end(), [&](auto d) { return d.name == name; });
    assert(!descriptor->writesReturnValue);
    constexpr std::uint32_t Out = 0x3000;
    auto call = [&](std::uint32_t address, bool handled) {
        GuestImportCall c; c.arguments[0] = address; c.returnValue = 0x123456789ABCDEF0;
        assert(registry.dispatch(name, c).handled() == handled);
        assert(c.returnValue == 0x123456789ABCDEF0); // Void ABI.
    };
    auto* out = space.translate(Out);
    std::fill_n(out - 4, 56, 0xA5);
    call(Out, false);
    assert(std::all_of(out, out + 48, [](auto b) { return b == 0xA5; }));
    assert(registry.bindVirtualVideoMode720p60());
    assert(!registry.bindVirtualVideoMode720p60());
    call(Out, true);
    constexpr std::array<std::uint32_t, 12> expected{
            1280, 720, 0, 1, 1, 0x42700000, 1, 0, 0, 0, 0, 0};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        for (unsigned j = 0; j < 4; ++j) {
            assert(out[4 * i + j] == static_cast<std::uint8_t>(expected[i] >> (24 - 8 * j)));
        }
    }
    assert(std::all_of(out - 4, out, [](auto b) { return b == 0xA5; }));
    assert(std::all_of(out + 48, out + 52, [](auto b) { return b == 0xA5; }));
    for (auto bad : {0U, Out + 1, 0xFFFFFFD4U, 0x7FC80000U - 44, 0x7FC8112CU, 0x7FC8FFFCU}) { call(bad, false); }
    call(0xFFFFFFD0U, true); // Exact end-of-address-space boundary.
    assert(registry.bindGuestThreadToken(0x5000));
    assert(registry.bindGuestThreadTlsStorage(0x5000, 0x6000, 4));
    for (auto bad : {0x6000U, 0x6004U, 0x6000U - 44}) { call(bad, false); }
    registry.reset(); call(Out, false);
    assert(registry.initialize(space, 0x10000000, 0x10000));
    call(Out, false);
    assert(registry.bindVirtualVideoMode720p60()); call(Out, true);
}
int main() {
    GuestAddressSpace space; assert(space.reserve());
    for (const auto name : {"__imp__XGetVideoMode", "__imp__VdQueryVideoMode"}) testQuery(space, name);
    std::puts("DEFAULT_VIDEO_MODE: both48byte queries, unknown words zero, canaries, void ABI, TLS/MMIO/bounds PASS");
}
