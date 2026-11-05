#include "guest_import_registry.h"
#include <cassert>
#include <cstring>

using namespace recomp::bootstrap;
namespace {
void put(GuestAddressSpace& space, std::uint32_t address, std::uint32_t value) {
    auto* bytes = space.translate(address);
    for (unsigned i = 0; i < 4; ++i) bytes[i] = static_cast<std::uint8_t>(value >> ((3 - i) * 8));
}
}
int main() {
    GuestAddressSpace space; assert(space.reserve());
    GuestImportRegistry registry;
    auto validator = [](std::uint32_t address) { return address >= 0x8000 && address < 0x9000 && !(address & 3); };
    assert(!registry.bindTitleTerminationNotifications(0x4000, 0x1000, validator));
    assert(registry.initialize(space, 0x10000000, 0x10000));
    auto invoke = [&](std::uint64_t record, std::uint32_t create, bool expected) {
        GuestImportCall call; call.arguments[0] = record; call.arguments[1] = create;
        call.returnValue = 0x12345678ABCDEF01ULL;
        assert(registry.dispatch("__imp__ExRegisterTitleTerminateNotification", call).handled() == expected);
        assert(call.returnValue == 0x12345678ABCDEF01ULL);
    };
    invoke(0x4000, 1, false);
    assert(!registry.bindTitleTerminationNotifications(0, 0x1000, validator));
    assert(!registry.bindTitleTerminationNotifications(0x4001, 0x1000, validator));
    assert(!registry.bindTitleTerminationNotifications(0xFFFFFFF4, 16, validator));
    assert(!registry.bindTitleTerminationNotifications(0x4000, 15, validator));
    assert(!registry.bindTitleTerminationNotifications(0x4000, 0x1000, {}));
    assert(registry.bindTitleTerminationNotifications(0x4000, 0x1000, validator));
    assert(!registry.bindTitleTerminationNotifications(0x4000, 0x1000, validator));
    for (auto bad : {0u, 0x3FFCu, 0x4001u, 0x4FF4u, 0xFFFFFFFCu}) invoke(bad, 1, false);
    put(space, 0x4000, 0x8000); put(space, 0x4004, 0xFFFFFFFF);
    put(space, 0x4008, 0xDEADBEEF); put(space, 0x400C, 0x01020304);
    std::array<std::uint8_t, 16> original{};
    std::memcpy(original.data(), space.translate(0x4000), 16);
    invoke(0x4000, 2, false); invoke(0x4000, 0, false);
    invoke(0xFFFFFFFF00004000ULL, 1, true);
    assert(std::memcmp(original.data(), space.translate(0x4000), 16) == 0);
    auto snapshot = registry.titleTerminationRegistrations();
    assert(snapshot.size() == 1 && snapshot[0].record == 0x4000 &&
        snapshot[0].routine == 0x8000 && snapshot[0].priority == 0xFFFFFFFF);
    invoke(0x4000, 1, false);
    put(space, 0x4010, 0x8000); invoke(0x4010, 1, false); invoke(0x4010, 0, false);
    put(space, 0x4004, 1); invoke(0x4000, 0, false);
    put(space, 0x4004, 0xFFFFFFFF); invoke(0x4000, 0, true);
    assert(registry.titleTerminationRegistrations().empty());
    assert(snapshot.size() == 1); // Returned snapshot is independent of later changes.
    for (auto invalidRoutine : {0u, 0x8001u, 0x9000u}) {
        put(space, 0x4000, invalidRoutine); invoke(0x4000, 1, false);
    }
    for (std::uint32_t i = 0; i < GuestImportRegistry::MaximumTitleTerminationRegistrations; ++i) {
        put(space, 0x4000 + i * 16, 0x8000 + i * 4);
        put(space, 0x4004 + i * 16, i);
        invoke(0x4000 + i * 16, 1, true);
    }
    put(space, 0x4FF0, 0x8800); invoke(0x4FF0, 1, false);
    assert(registry.titleTerminationRegistrations().size() == 64);
    invoke(0x4000, 0, true); invoke(0x4FF0, 1, true); // Exact end boundary, freed capacity.
    registry.reset(); assert(registry.titleTerminationRegistrations().empty());
    assert(registry.initialize(space, 0x10000000, 0x10000));
    invoke(0x4FF0, 1, false);
    assert(registry.bindTitleTerminationNotifications(0x4000, 0x1000, validator));
    invoke(0x4FF0, 1, true);
    assert(!GuestImportRegistry::isImplemented("__imp__XexTerminateTitle"));
}
