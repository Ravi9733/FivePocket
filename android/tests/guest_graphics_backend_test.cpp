#include "guest_import_registry.h"
#include <cassert>
#include <memory>

using namespace recomp::bootstrap;
namespace {
// Synthetic fault injection only. Real traces bind VulkanEngine, not this class.
class FixtureBackend final : public GuestGraphicsBackend {
public:
    bool initialize() override { ++calls; initialized = allowReady; return allowInitialize; }
    bool ready() const noexcept override { return initialized; }
    bool initialized = false, allowInitialize = true, allowReady = true;
    unsigned calls = 0;
};
}
int main() {
    GuestAddressSpace space; assert(space.reserve());
    GuestImportRegistry registry;
    auto backend = std::make_shared<FixtureBackend>();
    const auto validate = [](std::uint32_t address) { return address == 0x4000; };
    assert(!registry.bindGraphicsBackend(backend, 0x4000, 0x1000, validate));
    assert(registry.initialize(space, 0x10000000, 0x10000));
    GuestImportCall input; input.arguments = {{0x53020000, 0x4000, 0, 0x4100, 0x4200, 0, 0, 0}};
    input.returnValue = 0xABCDEF0123456789;
    auto invoke = [&](GuestImportCall call, bool expected) {
        assert(registry.dispatch("__imp__VdInitializeEngines", call).handled() == expected);
        assert(call.returnValue == (expected ? 1 : input.returnValue));
    };
    auto interrupt = [&](std::uint64_t callback, std::uint64_t user, bool expected) {
        GuestImportCall call; call.arguments[0] = callback; call.arguments[1] = user;
        call.returnValue = 0xFEDCBA9876543210;
        assert(registry.dispatch("__imp__VdSetGraphicsInterruptCallback", call).handled() == expected);
        assert(call.returnValue == 0xFEDCBA9876543210);
    };
    interrupt(0x4000, 0xABCD1234, false);
    auto identifier = [&](std::uint32_t address, bool expected) {
        GuestImportCall call; call.arguments[0] = address; call.returnValue = 0xFEDCBA9876543210;
        assert(registry.dispatch("__imp__VdSetSystemCommandBufferGpuIdentifierAddress", call).handled() == expected);
        assert(call.returnValue == 0xFEDCBA9876543210); // Void, including initial clear.
    };
    identifier(0, false);
    invoke(input, false);
    assert(!registry.graphicsInitialized());
    assert(!registry.bindGraphicsBackend(nullptr, 0x4000, 0x1000, validate));
    assert(!registry.bindGraphicsBackend(backend, 0x4000, 0x1000, {}));
    for (auto address : {0u, 1u, 0xFFFFF004u})
        assert(!registry.bindGraphicsBackend(backend, address, 0x1000, validate));
    assert(registry.bindGraphicsBackend(backend, 0x4000, 0x1000, validate));
    assert(!registry.bindGraphicsBackend(backend, 0x4000, 0x1000, validate));
    for (const auto& [argument, value] : std::array<std::pair<unsigned, std::uint32_t>, 10>{{
        {0, 0}, {1, 0}, {1, 0x4004}, {2, 1}, {3, 0x3FFC}, {3, 0x5000},
        {3, 0x4101}, {4, 0xFFFFFFFF}, {4, 0x4100}, {4, 0}}}) {
        auto bad = input; bad.arguments[argument] = value; invoke(bad, false);
    }
    assert(backend->calls == 0 && !registry.graphicsInitialized());
    backend->allowInitialize = false; backend->allowReady = false;
    invoke(input, false); assert(backend->calls == 1 && !registry.graphicsInitialized());
    backend->allowInitialize = true;
    invoke(input, false); assert(backend->calls == 2 && !registry.graphicsInitialized());
    backend->allowReady = true;
    auto extended = input;
    for (auto i : {1, 3, 4}) extended.arguments[i] |= 0xFFFFFFFF00000000ULL;
    invoke(extended, true);
    assert(registry.graphicsInitialized() && backend->calls == 3);
    identifier(0, true); identifier(0, true);
    identifier(0x4100, false); identifier(0xFFFFFFFF, false);
    interrupt(0xFFFFFFFF00004000, 0xFFFFFFFFABCD1234, true);
    auto registered = registry.graphicsInterruptRegistration();
    assert(registered && registered->routine == 0x4000 && registered->userData == 0xABCD1234);
    interrupt(0x4004, 0, false); interrupt(0, 1, false);
    assert(registry.graphicsInterruptRegistration()->userData == 0xABCD1234);
    interrupt(0x4000, 0, true); assert(registry.graphicsInterruptRegistration()->userData == 0);
    interrupt(0, 0, true); assert(!registry.graphicsInterruptRegistration());
    interrupt(0x4000, 123, true);
    backend->initialized = false; interrupt(0x4000, 456, false);
    identifier(0, false);
    assert(registry.graphicsInterruptRegistration()->userData == 123);
    backend->initialized = true;
    invoke(input, false); assert(backend->calls == 3);
    registry.reset(); assert(!registry.graphicsInitialized());
    identifier(0, false);
    assert(!registry.graphicsInterruptRegistration());
    assert(registry.initialize(space, 0x10000000, 0x10000));
    assert(!registry.bindGraphicsBackend(backend, 0x4000, 0x1000, validate)); // Already-live resource is not rebound.
    invoke(input, false);
}
