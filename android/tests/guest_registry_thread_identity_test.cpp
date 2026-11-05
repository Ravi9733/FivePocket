// Multicore plan WP2: the registry's "current guest thread" is the calling host
// thread's own identity while that thread executes guest code (RECOMP_GUEST_THREADS
// > 0). The cooperative build keeps the registry-wide member; there the same
// interleaving demonstrably misattributes the caller, which is why the change exists.
#include "guest_execution_guard.h"
#include "guest_import_registry.h"

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <initializer_list>
#include <mutex>
#include <thread>

using namespace recomp::bootstrap;
namespace {
#if defined(RECOMP_GUEST_THREADS) && RECOMP_GUEST_THREADS > 0
constexpr bool Parallel = true;
#else
constexpr bool Parallel = false;
#endif
constexpr std::uint32_t TokenA = 0x64010000, TokenB = 0x64020000, TokenC = 0x64030000, TokenM = 0x64040000;
constexpr std::uint32_t Heap = 0x10000000;

struct Result { GuestImportDispatchState state; std::uint64_t value; };
Result call(GuestImportRegistry& r, const char* name, std::initializer_list<std::uint32_t> args) {
    GuestImportCall c;
    std::copy(args.begin(), args.end(), c.arguments.begin());
    c.returnValue = 0xFEDCBA9876543210ULL;
    const auto result = r.dispatch(name, c);
    return {result.state, c.returnValue};
}
std::uint64_t invoke(GuestImportRegistry& r, const char* name, std::initializer_list<std::uint32_t> args) {
    const auto result = call(r, name, args);
    assert(result.state == GuestImportDispatchState::Handled);
    return result.value;
}
// Runs `body` as guest execution on this host thread (GuestExecutionGuard::isActive).
template<class Body> void asGuest(Body&& body) {
    GuestExecutionGuard guard;
    const auto result = guard.run(1000, [&] { body(); });
    assert(result.completed());
}
// Small step sequencer for deterministic interleavings between host threads.
class Steps {
public:
    void advance(unsigned to) { { std::lock_guard l(m_); step_ = to; } cv_.notify_all(); }
    void await(unsigned at) { std::unique_lock l(m_); cv_.wait(l, [&] { return step_ >= at; }); }
private:
    std::mutex m_; std::condition_variable cv_; unsigned step_{0};
};

std::uint32_t allocateSlot(GuestImportRegistry& r) {
    assert(r.bindGuestThreadToken(TokenM));
    const auto slot = static_cast<std::uint32_t>(invoke(r, "__imp__KeTlsAlloc", {}));
    assert(slot < 64);
    return slot;
}

// A binds, then B binds, then A stores and loads its TLS slot. With the member,
// A's store lands in B's slot (B was bound last). With per-thread identity it does not.
void deterministicInterleaving(GuestAddressSpace& s) {
    GuestImportRegistry r; assert(r.initialize(s, Heap, 0x10000));
    const auto slot = allocateSlot(r);
    Steps steps; std::uint64_t aSaw = 0, bSaw = 0;
    std::thread a([&] { asGuest([&] {
        assert(r.bindGuestThreadToken(TokenA)); steps.advance(1); steps.await(2);
        assert(invoke(r, "__imp__KeTlsSetValue", {slot, 0xAAAA0001}) == 1);
        aSaw = invoke(r, "__imp__KeTlsGetValue", {slot}); steps.advance(3);
    }); });
    std::thread b([&] { asGuest([&] {
        steps.await(1); assert(r.bindGuestThreadToken(TokenB)); steps.advance(2); steps.await(3);
        bSaw = invoke(r, "__imp__KeTlsGetValue", {slot});
    }); });
    a.join(); b.join();
    if (Parallel) assert(aSaw == 0xAAAA0001 && bSaw == 0);
    else assert(aSaw == 0xAAAA0001 && bSaw == 0xAAAA0001); // Documented hazard: B sees A's store.
}

// Unsynchronized stress: each thread stores its own marker and reads it back.
unsigned stress(GuestAddressSpace& s, unsigned iterations) {
    GuestImportRegistry r; assert(r.initialize(s, Heap, 0x10000));
    const auto slot = allocateSlot(r);
    std::atomic<unsigned> mismatches{0};
    std::atomic<bool> go{false};
    const auto body = [&](std::uint32_t token) { asGuest([&] {
        assert(r.bindGuestThreadToken(token));
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        for (unsigned i = 0; i < iterations; ++i) {
            const auto marker = token ^ i;
            assert(invoke(r, "__imp__KeTlsSetValue", {slot, marker}) == 1);
            if (invoke(r, "__imp__KeTlsGetValue", {slot}) != marker) mismatches.fetch_add(1);
            if (!(i & 63)) std::this_thread::yield();
        }
    }); };
    std::thread a(body, TokenA), b(body, TokenB);
    go.store(true, std::memory_order_release);
    a.join(); b.join();
    return mismatches.load();
}

// A bound host thread that is NOT executing guest code keeps today's member
// semantics (host callbacks, setup, post-run inspection).
void outsideGuestExecution(GuestAddressSpace& s) {
    GuestImportRegistry r; assert(r.initialize(s, Heap, 0x10000));
    const auto slot = allocateSlot(r);
    Steps steps;
    std::thread c([&] {
        assert(r.bindGuestThreadToken(TokenC)); steps.advance(1); steps.await(2);
        assert(invoke(r, "__imp__KeTlsSetValue", {slot, 0xC0C0C0C0}) == 1); // Member is TokenM here.
        steps.advance(3);
    });
    steps.await(1); assert(r.bindGuestThreadToken(TokenM)); steps.advance(2); steps.await(3);
    c.join();
    assert(invoke(r, "__imp__KeTlsGetValue", {slot}) == 0xC0C0C0C0); // Stored for the member, as today.
}

// reset()/initialize() invalidate every earlier per-thread binding.
void resetInvalidates(GuestAddressSpace& s) {
    GuestImportRegistry r; assert(r.initialize(s, Heap, 0x10000));
    Steps steps; Result after{};
    std::thread d([&] { asGuest([&] {
        assert(r.bindGuestThreadToken(TokenA)); steps.advance(1); steps.await(2);
        after = call(r, "__imp__KeTlsAlloc", {}); // Registry was reset: no identity bound.
    }); });
    steps.await(1); r.reset(); assert(r.initialize(s, Heap, 0x10000)); steps.advance(2);
    d.join();
    assert(after.state == GuestImportDispatchState::Rejected);
}
} // namespace

int main() {
    GuestAddressSpace s; assert(s.reserve());
    deterministicInterleaving(s);
    const auto mismatches = stress(s, 20000);
    if (Parallel) assert(mismatches == 0);
    outsideGuestExecution(s);
    resetInvalidates(s);
    std::printf("REGISTRY_THREAD_IDENTITY_V1 mode=%s deterministic_interleaving=%s stress_mismatches=%u"
        " outside_guest_member=1 reset_invalidates=1\n", Parallel ? "parallel" : "cooperative",
        Parallel ? "own_identity" : "misattributed_as_expected", mismatches);
}
