// Multicore plan WP2: registry wake notifications (RECOMP_GUEST_THREADS > 0).
// Real host threads play the waiter and the signaller. The bound wait handler
// emulates the planned executor's guarded suspension: snapshot a per-token wake
// sequence, evaluate the predicate, then block until the sequence changes. There is
// NO poll fallback, so a missing notification is a test failure, not a slow pass.
// Every delivered wake also proves the registry mutex was already released: a
// separate probe thread must be able to take it while the signaller is still
// inside the notification (open call).
#include "guest_execution_guard.h"
#include "guest_import_registry.h"
#include "guest_parallel_wake_test_stubs.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

using namespace recomp::bootstrap;
using namespace std::chrono_literals;
namespace {
#if defined(RECOMP_GUEST_THREADS) && RECOMP_GUEST_THREADS > 0
constexpr bool Parallel = true;
#else
constexpr bool Parallel = false;
#endif
constexpr std::uint32_t Parent = 0x63001000, Code = 0x82001000, Scratch = 0x30000000, Out = 0x30000100,
    Timeout = 0x30000200;
constexpr std::uint32_t StatusSuccess = 0;

void put(GuestAddressSpace& s, std::uint32_t a, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) s.base()[a + i] = static_cast<std::uint8_t>(v >> (24 - i * 8));
}
std::uint32_t get(GuestAddressSpace& s, std::uint32_t a) {
    const auto* b = s.base() + a;
    return (std::uint32_t{b[0]} << 24) | (std::uint32_t{b[1]} << 16) | (std::uint32_t{b[2]} << 8) | b[3];
}
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
    if (result.state != GuestImportDispatchState::Handled) std::fprintf(stderr, "fixture %s failed\n", name);
    assert(result.state == GuestImportDispatchState::Handled);
    return result.value;
}

thread_local std::uint32_t self = 0; // The guest token of the calling test thread.

// Per-token wake sequences: the executor side of the contract.
class Hub {
public:
    std::uint64_t snapshot(std::uint32_t token) { std::lock_guard l(m_); return seq_[token] + all_; }
    bool changed(std::uint32_t token, std::uint64_t seen, std::chrono::milliseconds limit) {
        std::unique_lock l(m_);
        return cv_.wait_for(l, limit, [&] { return seq_[token] + all_ != seen; });
    }
    void deliver(const recomp::test::WakeRecord& w) {
        { std::lock_guard l(m_); if (w.all) ++all_; else ++seq_[w.token]; }
        cv_.notify_all();
    }
    void markParked(std::uint32_t token) { { std::lock_guard l(m_); parked_.insert(token); } cv_.notify_all(); }
    void awaitParked(std::uint32_t token) {
        std::unique_lock l(m_);
        assert(cv_.wait_for(l, 5s, [&] { return parked_.count(token) != 0; }));
        parked_.erase(token);
    }
private:
    std::mutex m_; std::condition_variable cv_;
    std::map<std::uint32_t, std::uint64_t> seq_; std::uint64_t all_{0};
    std::set<std::uint32_t> parked_;
};

struct Delivered { recomp::test::WakeRecord wake; bool registryUnlocked; };

class Fixture {
public:
    explicit Fixture(GuestAddressSpace& s) : s_(s) {
        assert(r.initialize(s, 0x10000000, 0x40000));
        assert(r.bindGuestThreadToken(Parent)); put(s, Parent + 0x14C, 1);
        assert(r.bindInitialThreadObject(Parent, 1));
        assert(r.bindThreadStorageArena({{0x64000000, 0x100000}, {0x82000000, 0x10000}, {Code, 0x1000},
            0, 0, 32, 4, 0x4000, 3}, {}, [](auto a) { return a == Code; }));
        assert(r.bindThreadResumeHandler([](const auto&, auto) { return false; }));
        assert(r.bindThreadWaitHandler([this](const auto& ready) { return park(ready, std::nullopt); }));
        assert(r.bindThreadTimedWaitHandler([this](const auto& ready, auto deadline) { return park(ready, deadline); }));
        assert(r.bindNativeCriticalSectionWaits());
        assert(r.bindNativeEventWakeBoostHandler([this](const std::uint32_t* tokens, std::size_t count) {
            // Planned executor contract: a live waiter that registered but has not
            // parked yet gets a PENDING boost instead of a rejection. Runs under R.
            std::lock_guard l(boostMutex_);
            boosts.insert(boosts.end(), tokens, tokens + count);
            return true;
        }));
        recomp::test::setWakeObserver([this](const recomp::test::WakeRecord& w) {
            const bool unlocked = registryUnlocked();
            { std::lock_guard l(deliveredMutex_); delivered_.push_back({w, unlocked}); }
            hub.deliver(w);
        });
        (void)recomp::test::takeWakeRecords();
    }
    ~Fixture() {
        recomp::test::setWakeObserver({});
        std::lock_guard l(probeMutex_);
        for (auto& probe : probes_) probe.join();
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    std::uint32_t worker() {
        std::uint32_t token = 0;
        assert(r.bindGuestThreadToken(Parent));
        assert(r.createNativeCallbackWorker(Code, 0, [&](const auto& t) { token = t.layout.threadAddress; return true; }));
        return token;
    }
    std::uint32_t heapPage() {
        assert(r.bindGuestThreadToken(Parent));
        put(s_, Scratch, 0); put(s_, Scratch + 4, 0x1000);
        assert(invoke(r, "__imp__NtAllocateVirtualMemory", {Scratch, Scratch + 4, 0x3000, 4, 0}) == 0);
        return get(s_, Scratch);
    }
    void event(std::uint32_t at, bool synchronization) {
        put(s_, at, synchronization ? 0x01000000 : 0); put(s_, at + 4, 0); put(s_, at + 8, at + 8); put(s_, at + 12, at + 8);
    }
    std::vector<Delivered> takeDelivered() {
        std::lock_guard l(deliveredMutex_);
        auto out = std::move(delivered_); delivered_.clear();
        (void)recomp::test::takeWakeRecords();
        return out;
    }
    // Runs `body` on a new host thread, as guest execution bound to `token`.
    template<class Body> std::thread guest(std::uint32_t token, Body body) {
        return std::thread([this, token, body] {
            GuestExecutionGuard guard;
            const auto result = guard.run(1000, [&] {
                self = token;
                assert(r.bindGuestThreadToken(token));
                body();
            });
            assert(result.completed());
        });
    }

    GuestImportRegistry r;
    Hub hub;
    std::vector<std::uint32_t> boosts;
    std::function<void(std::uint32_t)> beforePark; // Runs in the waiter, after R was released.
    std::atomic<unsigned> missedWakes{0};

private:
    bool park(const GuestImportRegistry::ThreadWaitReady& ready, std::optional<std::chrono::steady_clock::time_point> deadline) {
        const auto token = self;
        if (beforePark) beforePark(token);
        for (;;) {
            const auto seen = hub.snapshot(token);
            if (ready()) return true;
            hub.markParked(token);
            auto limit = std::chrono::milliseconds(3000);
            if (deadline) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - std::chrono::steady_clock::now());
                if (left <= 0ms) continue; // Timed readiness becomes true at the deadline.
                limit = std::min(limit, left + 1ms);
                if (!hub.changed(token, seen, limit)) continue; // The deadline, not a missed wake.
                continue;
            }
            if (!Parallel) { std::this_thread::sleep_for(1ms); continue; } // Cooperative: poll.
            if (!hub.changed(token, seen, limit)) { missedWakes.fetch_add(1); return false; }
        }
    }
    bool registryUnlocked() {
        auto done = std::make_shared<std::atomic<bool>>(false);
        {
            std::lock_guard l(probeMutex_);
            probes_.emplace_back([this, done] { (void)r.createdThreadCount(); done->store(true); });
        }
        const auto until = std::chrono::steady_clock::now() + 2s;
        while (!done->load() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(50us);
        return done->load();
    }

    GuestAddressSpace& s_;
    std::mutex boostMutex_, deliveredMutex_, probeMutex_;
    std::vector<Delivered> delivered_;
    std::vector<std::thread> probes_;
};

void expectTargeted(const std::vector<Delivered>& got, std::initializer_list<std::uint32_t> tokens) {
    std::vector<std::uint32_t> seen;
    for (const auto& d : got) { assert(!d.wake.all && d.registryUnlocked); seen.push_back(d.wake.token); }
    assert(seen == std::vector<std::uint32_t>(tokens));
}

// KeSetEvent with boost reaches a waiter that registered but has not parked yet
// (requirement 3): not rejected, signal reserved, exactly one wake after unlock.
void directEventBoostBeforePark(GuestAddressSpace& s) {
    Fixture f(s);
    const auto waiter = f.worker();
    const auto at = f.heapPage(); f.event(at, true);
    std::mutex m; std::condition_variable cv; bool registered = false, released = false;
    f.beforePark = [&](std::uint32_t token) {
        if (token != waiter) return;
        std::unique_lock l(m); registered = true; cv.notify_all();
        cv.wait(l, [&] { return released; });
    };
    Result waited{};
    auto w = f.guest(waiter, [&] { waited = call(f.r, "__imp__KeWaitForSingleObject", {at, 3, 1, 0, 0}); });
    auto signaller = f.guest(Parent, [&] {
        { std::unique_lock l(m); cv.wait(l, [&] { return registered; }); }
        assert(invoke(f.r, "__imp__KeSetEvent", {at, 1, 0}) == 0); // Previous state.
        { std::lock_guard l(m); released = true; } cv.notify_all();
    });
    signaller.join(); w.join();
    assert(waited.state == GuestImportDispatchState::Handled && waited.value == StatusSuccess);
    assert(get(s, at + 4) == 0); // The auto-reset signal went to the reserved waiter.
    assert((f.boosts == std::vector<std::uint32_t>{waiter}));
    if (Parallel) expectTargeted(f.takeDelivered(), {waiter});
    else assert(f.takeDelivered().empty());
    assert(!f.missedWakes);
}

// A notification event wakes every parked waiter, by name, after the unlock.
void notificationEventWakesAll(GuestAddressSpace& s) {
    Fixture f(s);
    const auto first = f.worker(), second = f.worker();
    const auto at = f.heapPage(); f.event(at, false);
    Result a{}, b{};
    auto wa = f.guest(first, [&] { a = call(f.r, "__imp__KeWaitForSingleObject", {at, 3, 1, 0, 0}); });
    f.hub.awaitParked(first);
    auto wb = f.guest(second, [&] { b = call(f.r, "__imp__KeWaitForSingleObject", {at, 3, 1, 0, 0}); });
    f.hub.awaitParked(second);
    auto signaller = f.guest(Parent, [&] { assert(invoke(f.r, "__imp__KeSetEvent", {at, 0, 0}) == 0); });
    signaller.join(); wa.join(); wb.join();
    assert(a.value == StatusSuccess && b.value == StatusSuccess && get(s, at + 4) == 1);
    if (Parallel) expectTargeted(f.takeDelivered(), {first, second});
    assert(!f.missedWakes);
}

// NT handle waits: release/set/close wake the handle's waiters only.
void handleWaits(GuestAddressSpace& s) {
    Fixture f(s);
    const auto waiter = f.worker();
    assert(invoke(f.r, "__imp__NtCreateSemaphore", {Out, 0, 0, 10}) == 0);
    const auto semaphore = get(s, Out);
    assert(invoke(f.r, "__imp__NtCreateSemaphore", {Out, 0, 0, 10}) == 0);
    const auto idle = get(s, Out);
    // No waiter: no notification at all (cost).
    assert(invoke(f.r, "__imp__NtReleaseSemaphore", {idle, 1, 0}) == 0);
    assert(f.takeDelivered().empty());

    Result waited{};
    auto w = f.guest(waiter, [&] { waited = call(f.r, "__imp__NtWaitForSingleObjectEx", {semaphore, 1, 0, 0}); });
    f.hub.awaitParked(waiter);
    assert(invoke(f.r, "__imp__NtReleaseSemaphore", {idle, 1, 0}) == 0); // Another handle: nobody.
    assert(f.takeDelivered().empty());
    auto signaller = f.guest(Parent, [&] { assert(invoke(f.r, "__imp__NtReleaseSemaphore", {semaphore, 1, 0}) == 0); });
    signaller.join(); w.join();
    assert(waited.state == GuestImportDispatchState::Handled && waited.value == StatusSuccess);
    if (Parallel) expectTargeted(f.takeDelivered(), {waiter});

    assert(invoke(f.r, "__imp__NtCreateEvent", {Out, 0, 1, 0}) == 0);
    const auto event = get(s, Out);
    w = f.guest(waiter, [&] { waited = call(f.r, "__imp__NtWaitForSingleObjectEx", {event, 1, 0, 0}); });
    f.hub.awaitParked(waiter);
    signaller = f.guest(Parent, [&] { assert(invoke(f.r, "__imp__NtSetEvent", {event, 0}) == 0); });
    signaller.join(); w.join();
    assert(waited.value == StatusSuccess);
    if (Parallel) expectTargeted(f.takeDelivered(), {waiter});

    // A closed handle must be observed by its parked waiter (fails closed, as today).
    w = f.guest(waiter, [&] { waited = call(f.r, "__imp__NtWaitForSingleObjectEx", {event, 1, 0, 0}); });
    f.hub.awaitParked(waiter);
    signaller = f.guest(Parent, [&] { assert(invoke(f.r, "__imp__NtClose", {event}) == 0); });
    signaller.join(); w.join();
    assert(waited.state == GuestImportDispatchState::Rejected);
    if (Parallel) expectTargeted(f.takeDelivered(), {waiter});
    assert(!f.missedWakes);
}

// Re-arming a timer wakes its waiter, which re-parks until the new due time.
void timerRearm(GuestAddressSpace& s) {
    Fixture f(s);
    const auto waiter = f.worker();
    assert(invoke(f.r, "__imp__NtCreateTimer", {Out, 0, 1}) == 0); // Synchronization timer.
    const auto timer = get(s, Out);
    Result waited{};
    auto w = f.guest(waiter, [&] { waited = call(f.r, "__imp__NtWaitForSingleObjectEx", {timer, 1, 0, 0}); });
    f.hub.awaitParked(waiter);
    put(s, Timeout, 0xFFFFFFFF); put(s, Timeout + 4, 0xFFFE7960); // Relative 10 ms.
    auto signaller = f.guest(Parent, [&] { assert(invoke(f.r, "__imp__NtSetTimerEx", {timer, Timeout, 0, 1, 0, 0, 0, 0}) == 0); });
    signaller.join(); w.join();
    assert(waited.state == GuestImportDispatchState::Handled && waited.value == StatusSuccess);
    if (Parallel) expectTargeted(f.takeDelivered(), {waiter});
    assert(!f.missedWakes);
}

// Critical sections: leave wakes the FIFO front only; the front's own leave
// wakes the next waiter.
void criticalSectionFifo(GuestAddressSpace& s) {
    Fixture f(s);
    const auto first = f.worker(), second = f.worker();
    const auto cs = f.heapPage() + 0x100;
    std::vector<std::uint32_t> order; std::mutex orderMutex;
    auto owner = std::make_unique<std::thread>();
    std::mutex m; std::condition_variable cv; unsigned step = 0;
    const auto advance = [&](unsigned to) { { std::lock_guard l(m); step = to; } cv.notify_all(); };
    const auto await = [&](unsigned at) { std::unique_lock l(m); cv.wait(l, [&] { return step >= at; }); };
    *owner = f.guest(Parent, [&] {
        (void)invoke(f.r, "__imp__RtlInitializeCriticalSection", {cs});
        (void)invoke(f.r, "__imp__RtlEnterCriticalSection", {cs});
        advance(1); await(2);
        (void)invoke(f.r, "__imp__RtlLeaveCriticalSection", {cs});
    });
    await(1);
    const auto contender = [&](std::uint32_t token) {
        return f.guest(token, [&, token] {
            (void)invoke(f.r, "__imp__RtlEnterCriticalSection", {cs});
            { std::lock_guard l(orderMutex); order.push_back(token); }
            assert(get(s, cs + 24) == token);
            (void)invoke(f.r, "__imp__RtlLeaveCriticalSection", {cs});
        });
    };
    auto a = contender(first); f.hub.awaitParked(first);
    auto b = contender(second); f.hub.awaitParked(second);
    (void)f.takeDelivered();
    advance(2);
    owner->join(); a.join(); b.join();
    assert((order == std::vector<std::uint32_t>{first, second}));
    if (Parallel) expectTargeted(f.takeDelivered(), {first, second});
    assert(get(s, cs + 16) == 0xFFFFFFFF && !get(s, cs + 20) && !get(s, cs + 24));
    assert(!f.missedWakes);
}

// Thread exit broadcasts, but only while a registry wait may be parked.
void threadExitBroadcast(GuestAddressSpace& s) {
    Fixture f(s);
    const auto idle = f.worker(), exiting = f.worker(), waiter = f.worker();
    assert(f.r.bindGuestThreadToken(idle));
    assert(f.r.markGuestThreadReturned(idle, 0)); // Nobody waits: silent.
    assert(f.takeDelivered().empty());
    assert(f.r.bindGuestThreadToken(Parent));
    assert(invoke(f.r, "__imp__NtCreateSemaphore", {Out, 0, 0, 1}) == 0);
    const auto semaphore = get(s, Out);
    Result waited{};
    auto w = f.guest(waiter, [&] { waited = call(f.r, "__imp__NtWaitForSingleObjectEx", {semaphore, 1, 0, 0}); });
    f.hub.awaitParked(waiter);
    assert(f.r.bindGuestThreadToken(exiting));
    assert(f.r.markGuestThreadReturned(exiting, 7));
    const auto broadcast = f.takeDelivered();
    if (Parallel) assert(broadcast.size() == 1 && broadcast[0].wake.all && broadcast[0].registryUnlocked);
    else assert(broadcast.empty());
    assert(f.r.bindGuestThreadToken(Parent));
    assert(invoke(f.r, "__imp__NtReleaseSemaphore", {semaphore, 1, 0}) == 0);
    w.join();
    assert(waited.value == StatusSuccess);
    (void)f.takeDelivered();
    assert(!f.missedWakes);
}
} // namespace

int main() {
    GuestAddressSpace s; assert(s.reserve());
    directEventBoostBeforePark(s);
    if (Parallel) { // Concurrent waits need the per-thread identity of the parallel build.
        notificationEventWakesAll(s);
        handleWaits(s);
        timerRearm(s);
        criticalSectionFifo(s);
    }
    threadExitBroadcast(s);
    std::printf("REGISTRY_WAKE_V1 mode=%s boost_before_park=1 targeted_after_unlock=%s handle_waiters=%s"
        " timer_rearm=%s cs_fifo=%s broadcast_only_when_waiting=1 missed_wakes=0\n",
        Parallel ? "parallel" : "cooperative", Parallel ? "1" : "n/a", Parallel ? "1" : "n/a",
        Parallel ? "1" : "n/a", Parallel ? "1" : "n/a");
}
