#include "guest_import_registry.h"
#include <algorithm>
#include <cassert>

using namespace recomp::bootstrap;
namespace {
class Backend : public GuestGraphicsBackend {
public:
    bool initialize() override { active = true; return true; }
    bool ready() const noexcept override { return active; }
    bool queryDisplayGamma(std::uint32_t& t, std::uint32_t& p) const noexcept override {
        t = type; p = 0x3F800000; return configured;
    }
    bool active = false, configured = false;
    std::uint32_t type = 1;
};
}
int main() {
    GuestAddressSpace s; assert(s.reserve()); GuestImportRegistry r;
    assert(r.initialize(s, 0x10000000, 0x10000));
    auto backend = std::make_shared<Backend>();
    assert(r.bindGraphicsBackend(backend, 0x4000, 0x1000, [](auto a) { return a == 0x4000; }));
    auto invoke = [&](std::uint32_t type = 0x3004, std::uint32_t power = 0x3000) {
        GuestImportCall c; c.arguments = {type, power}; c.returnValue = 0x123456789ABCDEF0;
        const auto result = r.dispatch("__imp__VdGetCurrentDisplayGamma", c);
        assert(c.returnValue == 0x123456789ABCDEF0); return result.handled();
    };
    std::fill_n(s.translate(0x2FFC), 16, 0xA5); assert(!invoke());
    GuestImportCall init; init.arguments = {0x53020000, 0x4000, 0, 0x4100, 0x4200};
    assert(r.dispatch("__imp__VdInitializeEngines", init).handled());
    assert(!invoke()); assert(r.bindVirtualVideoMode720p60()); assert(!invoke());
    backend->configured = true; backend->type = 2; assert(!invoke()); backend->type = 1;
    for (auto bad : {0U, 0x3001U, 0xFFFFFFFEU, 0x7FC8112CU}) {
        assert(!invoke(bad, 0x3000)); assert(!invoke(0x3004, bad));
    }
    assert(!invoke(0x3000, 0x3000));
    assert(r.bindGuestThreadToken(0x6000)); assert(r.bindGuestThreadTlsStorage(0x6000, 0x7000, 4));
    assert(!invoke(0x7000, 0x3000)); assert(!invoke(0x3004, 0x7004));
    assert(std::all_of(s.translate(0x2FFC), s.translate(0x300C), [](auto b) { return b == 0xA5; }));
    assert(invoke());
    constexpr std::array<std::uint8_t, 16> expected{0xA5,0xA5,0xA5,0xA5,0x3F,0x80,0,0,0,0,0,1,0xA5,0xA5,0xA5,0xA5};
    assert(std::equal(expected.begin(), expected.end(), s.translate(0x2FFC)));
    assert(invoke(0xFFFFFFFC, 0x3000)); // Complete four-byte last word.
    backend->active = false; assert(!invoke()); backend->active = true;
    r.reset(); assert(!invoke());
}
