#include "guest_import_registry.h"
#include <algorithm>
#include <cassert>

using namespace recomp::bootstrap;
int main() {
    GuestAddressSpace space; assert(space.reserve());
    GuestImportRegistry registry;
    assert(!registry.bindNoLaunchData());
    assert(registry.initialize(space, 0x10000000, 0x10000));
    constexpr auto Size = "__imp__XamLoaderGetLaunchDataSize";
    constexpr auto Data = "__imp__XamLoaderGetLaunchData";
    constexpr std::uint32_t Out = 0x3000;
    auto* memory = space.translate(Out);
    auto fill = [&] { std::fill_n(memory - 4, 16, 0xA5); };
    auto unchanged = [&] { assert(std::all_of(memory - 4, memory + 12, [](auto b) { return b == 0xA5; })); };
    auto call = [&](const char* name, std::uint32_t output, std::uint32_t bytes,
                    bool expected, std::uint64_t value) {
        GuestImportCall c; c.arguments = {output, bytes, 0, 0, 0, 0, 0, 0};
        c.returnValue = 0xABCDEF0123456789;
        assert(registry.dispatch(name, c).handled() == expected);
        assert(c.returnValue == (expected ? value : 0xABCDEF0123456789));
    };
    fill(); call(Size, Out, 0, false, 0); unchanged();
    call(Data, Out, 8, false, 0); unchanged();
    assert(registry.bindNoLaunchData());
    call(Size, Out, 0, true, 1168);
    for (int i = -4; i < 12; ++i) assert(memory[i] == (i >= 0 && i < 4 ? 0 : 0xA5));
    fill(); call(Size, 0, 0, true, 87); unchanged();
    for (auto bad : {Out + 1, 0xFFFFFFFEU, 0x7FC80000U}) {
        call(Size, bad, 0, false, 0); unchanged();
    }
    for (auto output : {0U, Out, 0xFFFFFFFFU}) {
        call(Data, output, 0xFFFFFFFFU, true, 1168); unchanged();
    }
    assert(registry.bindGuestThreadToken(0x5000));
    assert(registry.bindGuestThreadTlsStorage(0x5000, 0x6000, 4));
    call(Size, 0x6000, 0, false, 0); unchanged();
    registry.reset();
    assert(registry.initialize(space, 0x10000000, 0x10000));
    call(Size, Out, 0, false, 0); unchanged();
    call(Data, Out, 8, false, 0); unchanged();
}
