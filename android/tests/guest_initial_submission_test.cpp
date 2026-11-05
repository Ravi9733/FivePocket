#include "guest_import_registry.h"
#include "guest_execution_guard.h"
#include <cassert>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <cstdio>
using namespace recomp::bootstrap;
namespace {
class Backend final : public GuestGraphicsBackend {
public:
    bool initialize() override { live = true; return true; }
    bool ready() const noexcept override { return live; }
    bool completeInitialEmptyCacheCoherency(const NativeInitialCoherency& request) override {
        ++calls; assert(request.physicalBase == 0x20000 && request.byteCount == 0x1000);
        beforeComplete();
        if (throws) throw std::runtime_error("fixture");
        return accept;
    }
    std::function<void()> beforeComplete = [] {};
    bool live = false, accept = false, throws = false;
    unsigned calls = 0;
};
}
int main() {
    GuestAddressSpace space; assert(space.reserve()); GuestImportRegistry registry;
    auto write = [&](std::uint32_t address, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) space.translate(address)[i] = std::uint8_t(value >> (24 - 8 * i));
    };
    auto read = [&](std::uint32_t address) {
        auto* b = space.translate(address);
        return (std::uint32_t{b[0]} << 24) | (std::uint32_t{b[1]} << 16) | (std::uint32_t{b[2]} << 8) | b[3];
    };
    auto dispatch = [&](const char* name, std::initializer_list<std::uint32_t> args, std::uint32_t stack = 0) {
        GuestImportCall c; c.stackPointer = stack; std::copy(args.begin(), args.end(), c.arguments.begin());
        const auto result = registry.dispatch(name, c);
        if (!result.handled()) std::fprintf(stderr, "fixture import rejected: %s\n", name);
        assert(result.handled()); return std::uint32_t(c.returnValue);
    };
    assert(!registry.bindNativeInitialCoherencySubmission());
    assert(!registry.submitNativeInitialCoherency(22));
    assert(registry.initialize(space, 0x10000000, 0x10000));
    auto backend = std::make_shared<Backend>();
    assert(registry.bindGraphicsBackend(backend, 0x4000, 0x1000, [](auto a) { return a == 0x4000; }));
    assert(!registry.bindNativeInitialCoherencySubmission());
    assert(registry.bindPhysicalMemoryArena(0xA0000000, 0x100000, 0));
    assert(registry.bindNativeInitialCoherencySubmission()); assert(!registry.bindNativeInitialCoherencySubmission());
    dispatch("__imp__VdInitializeEngines", {0x53020000, 0x4000, 0, 0x4100, 0x4200});
    auto allocate = [&](std::uint32_t bytes) { return dispatch("__imp__MmAllocatePhysicalMemoryEx", {1,bytes,4,0,0xFFFFFFFF,0}); };
    const auto ring = allocate(0x10000), wb = allocate(0x1000), ib = allocate(0x1000);
    const auto physical = [](auto a) { return a - 0xA0000000; };
    dispatch("__imp__VdInitializeRingBuffer", {physical(ring),13});
    assert(!registry.submitNativeInitialCoherency(22));
    dispatch("__imp__VdEnableRingBufferRPtrWriteBack", {physical(wb),7});
    dispatch("__imp__VdSetSystemCommandBufferGpuIdentifierAddress", {wb+8});
    std::vector<std::uint32_t> root(22);
    root[0] = 0xC0114800; root[1] = 0x3FF;
    for (unsigned i = 0; i < 8; ++i) root[4+i] = i * 0x80;
    root[12] = 0x10800; root[13] = 7; root[19] = 0xC0013F00; root[20] = physical(ib); root[21] = 11;
    const std::vector<std::uint32_t> child{0xA31,0x01000000,0x10A2F,0x1000,0x20000,0xC0043C00,3,0xA31,0,0x80000000,8};
    auto publish = [&] {
        for (unsigned i = 0; i < root.size(); ++i) write(ring + 4*i, root[i]);
        for (unsigned i = 0; i < child.size(); ++i) write(ib + 4*i, child[i]);
        for (unsigned i = 0; i < 19; ++i) write(0x12050 + 4*i, root[i]);
        std::fill_n(space.translate(wb), 8, 0xA5);
    };
    publish(); assert(!registry.submitNativeInitialCoherency(22)); // Verifier is required.
    dispatch("__imp__KiApcNormalRoutineNop", {0x12050,19}, 0x12000);
    auto unchanged = [&] {
        assert(!registry.completedNativeInitialCoherency()); assert(registry.graphicsRingConfiguration()->readWord == 0);
        assert(read(wb) == 0xA5A5A5A5 && read(wb+4) == 0xA5A5A5A5);
    };
    for (auto word : {0u,19u,21u,23u,0xFFFFFFFFu}) { assert(!registry.submitNativeInitialCoherency(word)); unchanged(); }
    for (unsigned i = 0; i < 22; ++i) {
        write(ring+4*i, root[i]^1); assert(!registry.submitNativeInitialCoherency(22)); unchanged(); publish();
    }
    for (unsigned i = 0; i < 11; ++i) {
        write(ib+4*i, child[i]^1); assert(!registry.submitNativeInitialCoherency(22)); unchanged(); publish();
    }
    for (auto invalid : {physical(wb), physical(ring), 0x80000u, 0x1FFFFFFCu}) {
        write(ring+80, invalid); assert(!registry.submitNativeInitialCoherency(22)); unchanged(); publish();
    }
    assert(backend->calls == 0);
    backend->beforeComplete = [&] { assert(read(wb) == 0xA5A5A5A5); };
    assert(!registry.submitNativeInitialCoherency(22)); assert(backend->calls == 1); unchanged();
    backend->throws = true; assert(!registry.submitNativeInitialCoherency(22)); unchanged(); backend->throws = false;
    backend->live = false; assert(!registry.submitNativeInitialCoherency(22)); unchanged(); backend->live = true;
    backend->accept = true;
    // The scalar store routes only inside an active guarded execution. A
    // handled MMIO store never changes flat MMIO backing memory.
    GuestExecutionGuard guard;
    assert(!bindGuestGraphicsSubmission({}));
    assert(bindGuestGraphicsSubmission([&](auto word) { return registry.submitNativeInitialCoherency(word); }));
    assert(!bindGuestGraphicsSubmission([](auto) { return true; }));
    assert(!tryWriteGuestGraphicsSubmission(0x7FC80714,4,22));
    auto success = guard.run(1, [&] { storeGuestScalar<std::uint32_t>(space.base(),0x7FC80714,22); });
    assert(success.completed() && read(0x7FC80714) == 0);
    assert(registry.completedNativeInitialCoherency()->physicalBase == 0x20000);
    assert(read(wb) == 22 && read(wb+4) == 0xA5A5A5A5 && registry.graphicsRingConfiguration()->readWord == 22);
    for (unsigned i = 0; i < root.size(); ++i) assert(read(ring+4*i) == root[i]);
    for (unsigned i = 0; i < child.size(); ++i) assert(read(ib+4*i) == child[i]);
    auto repeated = guard.run(1, [&] { storeGuestScalar<std::uint32_t>(space.base(),0x7FC80714,22); });
    assert(repeated.reason == GuestExecutionStopReason::UnsupportedScalarAccess && read(wb) == 22);
    resetGuestGraphicsSubmission();
    unsigned calls = 0;
    assert(bindGuestGraphicsSubmission([&](auto) { ++calls; return true; }));
    auto invalidStore = [&](auto value, std::uint32_t address = 0x7FC80714u) {
        auto result = guard.run(1, [&] { storeGuestScalar(space.base(),address,value); });
        assert(result.reason == GuestExecutionStopReason::UnsupportedScalarAccess);
    };
    invalidStore(std::uint8_t{22}); invalidStore(std::uint16_t{22}); invalidStore(std::uint64_t{22});
    invalidStore(std::uint32_t{22},0x7FC80710u); assert(calls == 0);
    resetGuestGraphicsSubmission(); invalidStore(std::uint32_t{22});
    assert(bindGuestGraphicsSubmission([](auto) -> bool { throw std::runtime_error("fixture"); }));
    invalidStore(std::uint32_t{22}); resetGuestGraphicsSubmission();
    calls = 0;
    assert(bindGuestGraphicsSubmission([&](auto) {
        ++calls;
        resetGuestGraphicsSubmission(); // Active reset must not destroy self.
        assert(!bindGuestGraphicsSubmission([](auto) { return true; }));
        assert(!tryWriteGuestGraphicsSubmission(0x7FC80714,4,22)); // No reentry.
        return false;
    }));
    invalidStore(std::uint32_t{22}); invalidStore(std::uint32_t{22}); assert(calls == 2);
    resetGuestGraphicsSubmission();
    registry.reset(); assert(!registry.completedNativeInitialCoherency());
    assert(!registry.submitNativeInitialCoherency(22));
}
