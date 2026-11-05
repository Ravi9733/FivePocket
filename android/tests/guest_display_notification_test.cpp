#include "guest_import_registry.h"
#include <algorithm>
#include <cassert>
#include <stdexcept>

using namespace recomp::bootstrap;
namespace {
class Backend : public GuestGraphicsBackend {
public:
    bool initialize() override { active = true; return true; }
    bool ready() const noexcept override { return active; }
    bool configureDisplay(const NativeDisplayConfiguration& c) override {
        ++calls;
        if (throwing) throw std::runtime_error("fixture");
        if (!accept) return false;
        state = c; return true;
    }
    bool active = false, accept = false, throwing = false;
    int calls = 0;
    std::optional<NativeDisplayConfiguration> state;
};
}
int main() {
    GuestAddressSpace s; assert(s.reserve()); GuestImportRegistry r;
    assert(r.initialize(s, 0x10000000, 0x10000));
    constexpr std::uint32_t Stack = 0x3000, Data = Stack + 144;
    NativeDisplayConfiguration c{{1280, 720, 1280, 720}, {0, 0, 1280, 720, 1280, 720, 0, 0, 0, 0, 0, 0, 0, 0}};
    const auto initial = c;
    auto publish = [&] {
        auto* b = s.translate(Data); std::fill_n(b - 4, 72, 0xA5);
        for (unsigned i = 0; i < 4; ++i) for (unsigned j = 0; j < 2; ++j)
            b[i * 2 + j] = std::uint8_t(c.dimensions[i] >> (8 - j * 8));
        for (unsigned i = 0; i < 14; ++i) for (unsigned j = 0; j < 4; ++j)
            b[8 + i * 4 + j] = std::uint8_t(c.scaler[i] >> (24 - j * 8));
    };
    auto invoke = [&](std::uint32_t type = 1, std::uint32_t stack = Stack, std::uint32_t data = Data) {
        GuestImportCall call; call.arguments[0] = type; call.arguments[1] = data;
        call.stackPointer = stack; call.returnValue = 0x123456789ABCDEF0;
        const auto result = r.dispatch("__imp__VdCallGraphicsNotificationRoutines", call);
        assert(call.returnValue == 0x123456789ABCDEF0); return result.handled();
    };
    publish(); assert(!invoke()); auto backend = std::make_shared<Backend>();
    assert(r.bindGraphicsBackend(backend, 0x4000, 0x1000, [](auto a) { return a == 0x4000; }));
    GuestImportCall init; init.arguments = {0x53020000, 0x4000, 0, 0x4100, 0x4200};
    assert(r.dispatch("__imp__VdInitializeEngines", init).handled());
    assert(!invoke() && backend->calls == 0); assert(r.bindVirtualVideoMode720p60());
    assert(!invoke() && backend->calls == 1 && !backend->state);
    backend->accept = true;
    std::array<std::uint8_t, 72> before{}; std::copy_n(s.translate(Data - 4), 72, before.begin());
    assert(invoke() && backend->state->dimensions == c.dimensions && backend->state->scaler == c.scaler);
    assert(std::equal(before.begin(), before.end(), s.translate(Data - 4)));
    for (auto type : {0U, 9U, UINT32_MAX}) assert(!invoke(type));
    assert(!invoke(1, 0)); assert(!invoke(1, Stack + 1)); assert(!invoke(1, Stack, Data + 4));
    assert(!invoke(1, 0xFFFFFF70, 0)); assert(!invoke(1, 0x7FC80000, 0x7FC80090));
    const int calls = backend->calls;
    for (unsigned i = 0; i < 4; ++i) {
        c = initial; c.dimensions[i] = 0; publish(); assert(!invoke());
        c = initial; c.dimensions[i] = 4097; publish(); assert(!invoke());
    }
    for (unsigned i = 0; i < 14; ++i) {
        c = initial; c.scaler[i] = UINT32_MAX; publish(); assert(!invoke());
    }
    c = initial; c.dimensions[2] = 640; c.scaler[4] = 640; publish(); assert(!invoke());
    assert(backend->calls == calls && backend->state->dimensions == initial.dimensions);
    c = initial; publish(); backend->throwing = true;
    bool threw = false; try { (void)invoke(); } catch (const std::runtime_error&) { threw = true; }
    assert(threw); backend->throwing = false; assert(invoke());
    backend->active = false; assert(!invoke()); backend->active = true;
    r.reset(); assert(!invoke());
}
