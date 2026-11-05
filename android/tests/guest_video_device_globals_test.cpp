#include "guest_video_device_globals.h"
#include <cassert>
#include <cstring>
using namespace recomp::bootstrap;
void put(GuestAddressSpace& space, std::uint32_t address, std::uint32_t value) {
    auto* b = space.translate(address);
    for (unsigned i = 0; i < 4; ++i) b[i] = static_cast<std::uint8_t>(value >> ((3 - i) * 8));
}
int main() {
    GuestAddressSpace space;
    GuestVideoDeviceGlobals globals;
    assert(!globals.matchesDevice(0) && !globals.matchesDevice(0x12345678));
    const GuestVideoDeviceGlobals::Layout layout{0x4000, 0x1000, 0x4100, 0x4FFC, 0x10000};
    assert(!globals.initialize(space, layout, {}));
    assert(space.reserve());
    put(space, layout.deviceSlot, 0xBE010100); // Swapped import descriptor.
    put(space, layout.xamDeviceSlot, 0x000101BF); // Original big-endian descriptor.
    const std::array<std::uint32_t, 5> invalidStorage{0, 0x10001, 0x4000, 0x4FFC, 0xFFFFFFFC};
    for (auto storage : invalidStorage) { auto bad = layout; bad.storage = storage; assert(!globals.initialize(space, bad, {})); }
    for (auto slot : {0x3FFCu, 0x5000u, 0x4101u, 0x4100u}) {
        auto bad = layout; bad.xamDeviceSlot = slot; assert(!globals.initialize(space, bad, {}));
    }
    assert(!globals.initialize(space, layout, {{0xFFF0, 17}}));
    assert(!globals.initialize(space, layout, {{0x10004, 4}}));
    assert(!globals.initialize(space, layout, {{0xFFFFFFFC, 8}}));
    put(space, layout.storage, 1); assert(!globals.initialize(space, layout, {})); put(space, layout.storage, 0);
    put(space, layout.xamDeviceSlot, 0x010101BF); assert(!globals.initialize(space, layout, {}));
    put(space, layout.xamDeviceSlot, 0x000101BF);
    assert(space.translate(layout.deviceSlot)[0] == 0xBE); // No partial mutation on failure.
    assert(globals.initialize(space, layout, {{0xFFF0, 16}, {0x10008, 4}}));
    assert(!globals.initialize(space, layout, {}));
    const std::array<std::uint8_t, 4> devicePointer{0, 1, 0, 0}, xamPointer{0, 1, 0, 4};
    assert(!std::memcmp(space.translate(layout.deviceSlot), devicePointer.data(), 4));
    assert(!std::memcmp(space.translate(layout.xamDeviceSlot), xamPointer.data(), 4));
    assert(!globals.device() && !globals.xamDevice());
    put(space, layout.storage, 0x12345678); put(space, layout.storage + 4, 0xFEDCBA98);
    assert(globals.device() == 0x12345678 && globals.xamDevice() == 0xFEDCBA98);
    assert(globals.matchesDevice(0x12345678) && !globals.matchesDevice(0x12345679) && !globals.matchesDevice(0));
    for (unsigned bit=0;bit<32;++bit) {
        put(space,layout.deviceSlot,layout.storage^(1u<<bit));assert(!globals.matchesDevice(0x12345678));
        put(space,layout.deviceSlot,layout.storage);
        put(space,layout.xamDeviceSlot,(layout.storage+4)^(1u<<bit));assert(!globals.matchesDevice(0x12345678));
        put(space,layout.xamDeviceSlot,layout.storage+4);
    }
    globals.reset(); assert(!globals.device());
    assert(!globals.matchesDevice(0x12345678));
    assert(space.translate(layout.deviceSlot)[0] == 0xBE);
    assert(space.translate(layout.xamDeviceSlot)[3] == 0xBF);
    for (unsigned i = 0; i < 8; ++i) assert(space.translate(layout.storage)[i] == 0);
    { GuestVideoDeviceGlobals scope; assert(scope.initialize(space, layout, {})); }
    assert(space.translate(layout.deviceSlot)[0] == 0xBE);
}
