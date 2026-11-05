// Host tests for the WP1 permit scheduler (local-analysis/parallel-design/executor.md section 5)
// and the per-function checkpoint fast path (guest_parallel_wake.h).
//
// Build the executor translation unit and this test with the SAME mode define, for example:
//   clang++ -std=c++20 -g -O1 -fsanitize=thread -DRECOMP_GUEST_THREADS=2 \
//     -Iandroid/app/src/main/cpp android/tests/guest_parallel_executor_test.cpp \
//     local-analysis/attempts/v12/src/guest_cooperative_executor.cpp \
//     local-analysis/attempts/v12/src/guest_execution_guard.cpp -o /tmp/parallel_lanes
// Mode 0 (cooperative) runs the fast-path/parity subset; modes 1 (gil) and 2 (lanes) run everything
// that applies to them.
#include "guest_cooperative_executor.h"
#include "guest_parallel_wake.h"
#include "guest_thread_priority.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifndef RECOMP_GUEST_THREADS
#define RECOMP_GUEST_THREADS 0
#endif

using namespace recomp::bootstrap;
using Clock = std::chrono::steady_clock;

namespace {
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #condition); std::abort(); } } while (false)

// Shaped like PPCContext for the countdown slot: alignas(64), a v127 member, tail padding after it.
// The constructor zeroes every byte. Value-initialisation does not reliably zero padding, and when
// the context is a local visible to the optimiser, reading undetermined padding would be folded as
// undefined. Generated code never has that problem: it only receives PPCContext& through an
// opaque call (recomp_host_guest_call_indirect), and an arbitrary initial value is bounded by
// GuestCheckpointCountdownLimit.
struct alignas(64) FakeContext {
    FakeContext() { std::memset(static_cast<void*>(this), 0, sizeof(*this)); }
    std::uint64_t registers[40];
    struct { std::uint8_t bytes[16]; } v127;
};
static_assert(offsetof(FakeContext, v127) + 16 + 4 <= sizeof(FakeContext));
// The synthetic test context has no v127: every entry must take the slow path.
struct PlainContext { std::uint32_t marker{0}; };

void fast(FakeContext& ctx, const char* label) { guestCheckpoint(ctx, label, std::strlen(label)); }
void native(const char* label) { GuestExecutionGuard::checkpoint(label); }
[[maybe_unused]] void spinFor(std::chrono::microseconds duration) {
    const auto until = Clock::now() + duration;
    while (Clock::now() < until) {}
}
const auto noBind = [](std::uint32_t) {};

struct Watchdog {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::thread thread;
    explicit Watchdog(int seconds) : thread([this, seconds] {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, std::chrono::seconds(seconds), [&] { return done; })) {
            std::fprintf(stderr, "WATCHDOG: test exceeded %d s (deadlock or lost wakeup)\n", seconds);
            std::abort();
        }
    }) {}
    ~Watchdog() {
        { std::lock_guard lock(mutex); done = true; }
        cv.notify_all();
        thread.join();
    }
};

// ---------------------------------------------------------------------------------------------
// Every mode: the fast path keeps entry accounting, the global budget and scope budgets exact.
// ---------------------------------------------------------------------------------------------
void testFastPathAccounting() {
    GuestCooperativeExecutor executor;
    const auto result = executor.run(1, [&] {
        FakeContext ctx{};
        for (int i = 0; i < 100000; ++i) fast(ctx, "fast_accounting");
        PlainContext plain{};
        for (int i = 0; i < 10; ++i) guestCheckpoint(plain, "plain", 5);
    }, {1000000, 1024, 2}, noBind);
    CHECK(result.execution.completed());
#if RECOMP_GUEST_THREADS == 0
    CHECK(result.execution.completedFunctionEntries == 100010); // Every entry takes the slow path.
#else
    // Fast entries are counted at the next slow path of the same context; entries after the last
    // one (here: before the switch to the plain context) are not, so totals can undercount by less
    // than one countdown batch (1024) per context switch or task end. Budget stops stay exact.
    const auto total = result.execution.completedFunctionEntries;
    CHECK(total <= 100010 && total + 1024 > 100010);
    const auto stats = guestParallelStats();
    CHECK(stats.slowCheckpoints < 1000); // About one slow path per quantum (1024 entries).
    std::printf("FAST_PATH entries=100010 counted=%llu slow_paths=%llu\n", static_cast<unsigned long long>(total),
                static_cast<unsigned long long>(stats.slowCheckpoints));
#endif
}

void testFastPathBudgetExact() {
    GuestCooperativeExecutor executor;
    std::uint32_t current = 0;
    const auto result = executor.run(1, [&] {
        CHECK(executor.enqueue(2, [&] { FakeContext ctx{}; for (;;) fast(ctx, "budget_worker"); }));
        FakeContext ctx{};
        for (;;) fast(ctx, "budget_main");
    }, {5000, 7, 3}, [&](auto token) { current = token; });
    CHECK(result.execution.reason == GuestExecutionStopReason::FunctionEntryBudgetExceeded);
    CHECK(result.execution.detail == "Global cooperative function-entry budget exhausted");
    CHECK(result.execution.completedFunctionEntries == 5000);
    CHECK(result.workersEntered == 1);
}

void testScopeBudgetExact() {
    GuestCooperativeExecutor executor;
    auto result = executor.run(1, [&] {
        FakeContext ctx{};
        for (int i = 0; i < 3000; ++i) fast(ctx, "before_scope"); // Leaves a partly used countdown.
        CHECK(executor.runNativeNonYielding([&] { for (int i = 0; i < 10; ++i) fast(ctx, "in_scope"); }, 10));
        for (int i = 0; i < 100; ++i) fast(ctx, "after_scope");
    }, {1000000, 1024, 2}, noBind);
    CHECK(result.execution.completed());
#if RECOMP_GUEST_THREADS == 0
    CHECK(result.execution.completedFunctionEntries == 3110);
#else
    CHECK(result.execution.completedFunctionEntries <= 3110 && result.execution.completedFunctionEntries >= 3010);
#endif
    result = executor.run(1, [&] {
        FakeContext ctx{};
        for (int i = 0; i < 3000; ++i) fast(ctx, "before_scope");
        (void)executor.runNativeNonYielding([&] { for (int i = 0; i < 11; ++i) fast(ctx, "in_scope"); }, 10);
        CHECK(false && "the eleventh scope entry must stop the run");
    }, {1000000, 1024, 2}, noBind);
    CHECK(result.execution.reason == GuestExecutionStopReason::FunctionEntryBudgetExceeded);
    CHECK(result.execution.detail == "Native non-yielding function-entry budget exhausted");
    CHECK(result.execution.completedFunctionEntries == 3010);
}

void testTaskCreationEvidence() {
    // Plausible context pointer: its first two big-endian words appear in RENDER_DIAG_TASK_CREATED.
    std::vector<std::uint8_t> guest(0x20000, 0);
    const std::uint8_t words[8] = {0x82, 0x21, 0x00, 0x40, 0x83, 0x64, 0xA9, 0x40};
    std::memcpy(guest.data() + 0x10000, words, sizeof(words));
    GuestCooperativeExecutor executor;
    std::uint32_t seen = 0;
    const auto result = executor.run(1, [&] {
        CHECK(currentGuestThreadToken() == 1);
        noteGuestThreadStart(2, 0x8364A940, 0x10000, 0x82211000, guest.data());
        CHECK(executor.enqueue(2, [&] { seen = currentGuestThreadToken(); native("evidence_worker"); }, 1));
        noteGuestThreadStart(3, 0x8364A940, 0x10001, 0, guest.data()); // Unaligned: no peek.
        CHECK(executor.enqueue(3, [] {}));
        native("evidence_main");
        CHECK(executor.waitUntil([&] { return seen != 0; }));
    }, {1000, 4, 4}, noBind);
    CHECK(result.execution.completed() && seen == 2);
    CHECK(currentGuestThreadToken() == 0);
    wakeGuestWaiters(2); // Harmless outside a run in every mode.
    wakeAllGuestWaiters();
}

#if RECOMP_GUEST_THREADS == 0
// Cooperative: the fast path must reproduce the old per-entry chain exactly.
void testCooperativeParity() {
    const auto run = [](bool viaFastPath) {
        GuestCooperativeExecutor executor;
        std::vector<std::string> trace;
        std::uint32_t current = 0;
        const auto result = executor.run(1, [&] {
            FakeContext ctx{};
            const auto entry = [&](const char* label) { if (viaFastPath) fast(ctx, label); else native(label); };
            entry("m0");
            CHECK(executor.enqueue(2, [&] {
                FakeContext wctx{};
                const auto workerEntry = [&](const char* label) { if (viaFastPath) fast(wctx, label); else native(label); };
                workerEntry("w0");
                workerEntry("w1");
            }));
            entry("m1");
            entry("m2");
        }, {20, 1, 3}, [&](auto token) { current = token; },
        [&](auto label) { trace.emplace_back(std::to_string(current) + ":" + std::string(label)); });
        CHECK(result.execution.completed() && result.execution.completedFunctionEntries == 5);
        return trace;
    };
    const auto old = run(false);
    CHECK((old == std::vector<std::string>{"1:m0", "1:m1", "2:w0", "1:m2", "2:w1"}));
    CHECK(run(true) == old);
    CHECK(guestParallelStats().mode == 0);
    std::puts("COOPERATIVE_FAST_PATH_PARITY trace_identical=1");
}
#else
// ---------------------------------------------------------------------------------------------
// Parallel modes.
// ---------------------------------------------------------------------------------------------
void testPingPong(bool acrossLanes) {
    setGuestThreadDomainPolicy(acrossLanes ? GuestThreadDomainPolicy(
        [](std::uint32_t token, std::uint32_t, std::uint32_t) { return token == 2 ? 1U : 0U; }) : GuestThreadDomainPolicy{});
    GuestCooperativeExecutor executor;
    std::mutex mutex;
    int turn = 1;
    constexpr int Rounds = 3000;
    const auto started = Clock::now();
    const auto result = executor.run(1, [&] {
        CHECK(executor.enqueue(2, [&] {
            FakeContext ctx{};
            for (int i = 0; i < Rounds; ++i) {
                noteGuestWaitSite("ping_pong_worker");
                CHECK(executor.waitUntil([&] { std::lock_guard lock(mutex); return turn == 2; }));
                fast(ctx, "pong");
                { std::lock_guard lock(mutex); turn = 1; }
                wakeGuestWaiters(1);
            }
        }));
        FakeContext ctx{};
        for (int i = 0; i < Rounds; ++i) {
            { std::lock_guard lock(mutex); turn = 2; }
            wakeGuestWaiters(2);
            noteGuestWaitSite("ping_pong_main");
            CHECK(executor.waitUntil([&] { std::lock_guard lock(mutex); return turn == 1; }));
            fast(ctx, "ping");
        }
    }, {100000000, 1024, 4}, noBind);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    CHECK(result.execution.completed());
    const auto stats = guestParallelStats();
    CHECK(stats.lateWakeups == 0);
    std::printf("PING_PONG lanes=%d rounds=%d elapsed_ms=%lld max_concurrent=%llu probes=%llu late=0\n", acrossLanes ? 1 : 0,
                Rounds, static_cast<long long>(ms), static_cast<unsigned long long>(stats.maxConcurrentPermits),
                static_cast<unsigned long long>(stats.probes));
    setGuestThreadDomainPolicy({});
}

void testLostWakeupWindow() {
    GuestCooperativeExecutor executor;
    std::mutex mutex;
    std::condition_variable requested;
    int request = 0, flag = 0;
    bool quit = false;
    constexpr int Rounds = 2000;
    std::thread producer([&] { // A host thread outside the executor, like a native producer.
        std::mt19937 random(12345);
        std::uniform_int_distribution<int> delay(0, 80);
        for (int round = 1; round <= Rounds; ++round) {
            {
                std::unique_lock lock(mutex);
                requested.wait(lock, [&] { return request >= round || quit; });
                if (quit) return;
            }
            spinFor(std::chrono::microseconds(delay(random))); // Lands before, during or after the park.
            std::lock_guard lock(mutex); // The predicate sees the flag only after the wake is issued.
            flag = round;
            wakeGuestWaiters(1);
        }
    });
    const auto result = executor.run(1, [&] {
        for (int round = 1; round <= Rounds; ++round) {
            { std::lock_guard lock(mutex); request = round; }
            requested.notify_all();
            const auto deadline = Clock::now() + std::chrono::seconds(5); // Timed: never quiescent.
            noteGuestWaitSite("lost_wakeup_window");
            CHECK(executor.waitUntil([&] {
                std::lock_guard lock(mutex);
                return flag == round || Clock::now() >= deadline;
            }, deadline));
            std::lock_guard lock(mutex);
            CHECK(flag == round);
        }
    }, {100000000, 1024, 2}, noBind);
    { std::lock_guard lock(mutex); quit = true; }
    requested.notify_all();
    producer.join();
    CHECK(result.execution.completed());
    const auto stats = guestParallelStats();
    CHECK(stats.lateWakeups == 0); // A lost notification would complete through the poll: late.
    std::printf("LOST_WAKEUP_WINDOW rounds=%d parks=%llu notified=%llu poll=%llu late=0\n", Rounds,
                static_cast<unsigned long long>(stats.parks), static_cast<unsigned long long>(stats.wakeNotified),
                static_cast<unsigned long long>(stats.wakePoll));
}

void testPollFallback() {
    GuestCooperativeExecutor executor;
    std::atomic<bool> flag{false};
    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        flag.store(true); // Deliberately no wakeGuestWaiters: a non-notifying producer.
    });
    const auto result = executor.run(1, [&] {
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        noteGuestWaitSite("test_poll_site");
        CHECK(executor.waitUntil([&] { return flag.load() || Clock::now() >= deadline; }, deadline));
        CHECK(flag.load());
    }, {1000000, 1024, 2}, noBind);
    producer.join();
    CHECK(result.execution.completed());
    const auto stats = guestParallelStats();
    CHECK(stats.lateWakeups == 1 && stats.wakePoll >= 1);
    CHECK(stats.lateSites[0].count == 1 && std::string(stats.lateSites[0].site) == "test_poll_site");
    std::printf("POLL_FALLBACK late=1 site=%s polls=%llu\n", stats.lateSites[0].site,
                static_cast<unsigned long long>(stats.wakePoll));
}

void testStopPropagation() {
    GuestCooperativeExecutor executor;
    std::atomic<int> unwound{0};
    struct Unwind { std::atomic<int>& count; ~Unwind() { count.fetch_add(1); } };
    const auto started = Clock::now();
    const auto result = executor.run(1, [&] {
        CHECK(executor.enqueue(2, [&] { Unwind u{unwound}; FakeContext ctx{}; for (;;) fast(ctx, "spinner"); }));
        CHECK(executor.enqueue(3, [&] { Unwind u{unwound}; (void)executor.waitUntil([] { return false; });
                                        CHECK(false && "parked task must be cancelled"); }));
        CHECK(executor.enqueue(4, [&] { Unwind u{unwound}; (void)executor.delayUntil(Clock::now() + std::chrono::seconds(10));
                                        CHECK(false && "delay must be cancelled"); }));
        CHECK(executor.enqueue(5, [&] { Unwind u{unwound}; FakeContext ctx{};
                                        for (int i = 0; i < 5000; ++i) fast(ctx, "trapper");
                                        GuestExecutionGuard::stopForGuestTrap(); }));
        CHECK(executor.enqueue(6, [&] { Unwind u{unwound}; FakeContext ctx{}; for (;;) fast(ctx, "spinner2"); }));
        Unwind u{unwound};
        FakeContext ctx{};
        for (;;) fast(ctx, "main_spinner");
    }, {100000000, 64, 8}, noBind);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    CHECK(result.execution.reason == GuestExecutionStopReason::GuestTrap);
    CHECK(unwound.load() == 6);
    CHECK(ms < 3000); // The 10 s delay was interrupted.
    std::printf("STOP_PROPAGATION unwound=6 elapsed_ms=%lld\n", static_cast<long long>(ms));
}

void testStopDuringHandoffAndExclusiveWait() {
    GuestCooperativeExecutor executor;
    auto result = executor.run(1, [&] {
        CHECK(executor.enqueue(2, [&] { FakeContext ctx{}; for (int i = 0; i < 100; ++i) fast(ctx, "handoff_runaway"); }));
        (void)executor.runNativeWorkerNext(2, 8);
        CHECK(false && "the handoff parent must be cancelled");
    }, {1000000, 64, 3}, noBind);
    CHECK(result.execution.reason == GuestExecutionStopReason::FunctionEntryBudgetExceeded);
    CHECK(result.execution.detail == "Exclusive native worker handoff budget or ownership boundary");
#if RECOMP_GUEST_THREADS == 2
    // An exclusive request waits for another lane's safepoint; that lane stops the run first.
    setGuestThreadDomainPolicy([](std::uint32_t token, std::uint32_t, std::uint32_t) { return token == 2 ? 1U : 0U; });
    std::atomic<bool> laneStarted{false}, scopeRan{false};
    result = executor.run(1, [&] {
        CHECK(executor.enqueue(2, [&] {
            FakeContext ctx{};
            fast(ctx, "lane_entry");
            laneStarted.store(true);
            spinFor(std::chrono::milliseconds(50)); // No checkpoint: cannot reach a safepoint.
            GuestExecutionGuard::stopForUnimplementedImport("__imp__LaneStop");
        }));
        FakeContext ctx{};
        fast(ctx, "arm_lane");
        while (!laneStarted.load()) {}
        (void)executor.runNativeNonYielding([&] { scopeRan.store(true); }, 1);
        CHECK(false && "the exclusive requester must be cancelled");
    }, {1000000, 64, 3}, noBind);
    CHECK(result.execution.reason == GuestExecutionStopReason::UnimplementedImport);
    CHECK(result.execution.detail == "__imp__LaneStop" && !scopeRan.load());
    setGuestThreadDomainPolicy({});
#endif
    std::puts("STOP_HANDOFF_AND_EXCLUSIVE_WAIT pass=1");
}

void testExclusivity() {
    if (RECOMP_GUEST_THREADS == 2)
        setGuestThreadDomainPolicy([](std::uint32_t token, std::uint32_t, std::uint32_t) {
            return token == 2 ? 1U : token == 3 ? 2U : 0U; });
    GuestCooperativeExecutor executor;
    std::atomic<int> busy{0}, violations{0}, predicateInScope{0};
    std::atomic<bool> inside{false}, stop{false};
    constexpr int Scopes = 400;
    const auto result = executor.run(1, [&] {
        CHECK(executor.enqueue(2, [&] { // Busy guest code between checkpoints.
            FakeContext ctx{};
            while (!stop.load()) {
                busy.store(1);
                spinFor(std::chrono::microseconds(2));
                busy.store(0);
                fast(ctx, "busy_lane");
            }
        }));
        CHECK(executor.enqueue(3, [&] { // A waiter whose predicate is evaluated often.
            while (!stop.load()) {
                const auto deadline = Clock::now() + std::chrono::microseconds(200);
                (void)executor.waitUntil([&] {
                    if (inside.load()) predicateInScope.fetch_add(1);
                    return stop.load() || Clock::now() >= deadline;
                }, deadline);
            }
        }));
        FakeContext ctx{};
        for (int i = 0; i < Scopes; ++i) {
            for (int j = 0; j < 200; ++j) fast(ctx, "outside_scope");
            CHECK(executor.runNativeNonYielding([&] {
                inside.store(true);
                if (busy.load()) violations.fetch_add(1);
                spinFor(std::chrono::microseconds(20));
                if (busy.load()) violations.fetch_add(1);
                inside.store(false);
            }, 1));
        }
        stop.store(true);
        wakeGuestWaiters(3);
    }, {1000000000, 64, 4}, noBind);
    CHECK(result.execution.completed());
    CHECK(violations.load() == 0 && predicateInScope.load() == 0);
    const auto stats = guestParallelStats();
    CHECK(stats.exclusiveScopes == Scopes);
    std::printf("EXCLUSIVITY scopes=%d violations=0 predicates_in_scope=0 max_concurrent=%llu tts_max_us=%llu\n", Scopes,
                static_cast<unsigned long long>(stats.maxConcurrentPermits),
                static_cast<unsigned long long>(stats.safepointMaxNs / 1000));
    setGuestThreadDomainPolicy({});
}

void testHandoffParity() {
    if (RECOMP_GUEST_THREADS == 2) // Peer and handoff worker both in lane 1: a cross-lane handoff.
        setGuestThreadDomainPolicy([](std::uint32_t token, std::uint32_t, std::uint32_t) {
            return token == 2 || token == 3 ? 1U : 0U; });
    GuestCooperativeExecutor executor;
    std::atomic<int> peerBusy{0}, violations{0};
    std::atomic<bool> stopPeer{false}, armed{false}, workerRan{false};
    const auto result = executor.run(1, [&] {
        CHECK(executor.enqueue(2, [&] {
            FakeContext ctx{};
            while (!stopPeer.load()) {
                peerBusy.store(1);
                spinFor(std::chrono::microseconds(1));
                peerBusy.store(0);
                fast(ctx, "peer");
            }
        }));
        FakeContext ctx{};
        for (int i = 0; i < 2000; ++i) fast(ctx, "let_peer_run");
        CHECK(executor.enqueue(3, [&] {
            CHECK(armed.load()); // Started only through the handoff.
            workerRan.store(true);
            FakeContext wctx{};
            for (int i = 0; i < 10; ++i) {
                if (peerBusy.load()) violations.fetch_add(1);
                fast(wctx, "handoff_worker");
            }
            CHECK(!executor.waitUntil([] { return true; })); // Waits reject inside a handoff.
            CHECK(!executor.runNativeWorkerNext(2, 8));      // No nested handoff.
            CHECK(executor.runNativeNonYielding([&] { fast(wctx, "nested_scope"); }, 4));
            if (peerBusy.load()) violations.fetch_add(1);
        }));
        armed.store(true);
        CHECK(executor.runNativeWorkerNext(3, 64));
        CHECK(workerRan.load());
        CHECK(!executor.runNativeWorkerNext(3, 64)); // Finished workers cannot be handed off again.
        CHECK(!executor.runNativeWorkerNext(1, 64)); // Nor the caller itself.
        for (int i = 0; i < 2000; ++i) fast(ctx, "after_handoff");
        stopPeer.store(true);
    }, {1000000000, 64, 4}, noBind);
    CHECK(result.execution.completed() && violations.load() == 0);
    CHECK(guestParallelStats().handoffs == 1);
    std::puts("HANDOFF_PARITY worker_started_only_by_handoff=1 peers_parked=1 waits_rejected=1 nested_rejected=1");
    setGuestThreadDomainPolicy({});
}

void testQuiescence() {
    GuestCooperativeExecutor executor;
    // Finish-initiated: a worker finishes while main waits forever (cooperative finish() result).
    auto result = executor.run(1, [&] {
        CHECK(executor.enqueue(2, [] {}));
        (void)executor.waitUntil([] { return false; });
        CHECK(false && "a stalled run must unwind instead of claiming a wake");
    }, {10, 32, 2}, noBind);
    CHECK(result.execution.reason == GuestExecutionStopReason::NoRunnableTask);
    // Wait-initiated: the only task waits without a producer; its wait returns false.
    result = executor.run(1, [&] {
        CHECK(!executor.waitUntil([] { return false; }));
        CHECK(executor.waitUntil([] { return true; }));
    }, {10, 32, 2}, noBind);
    CHECK(result.execution.completed() && result.waitOperations == 2);
    // Several waiters: exactly one wait (the last to park) reports no runnable producer.
    std::atomic<int> falseReturns{0};
    result = executor.run(1, [&] {
        for (std::uint32_t token = 2; token <= 3; ++token) {
            CHECK(executor.enqueue(token, [&] {
                if (!executor.waitUntil([] { return false; })) {
                    falseReturns.fetch_add(1);
                    GuestExecutionGuard::stopForRejectedImport("__imp__NoProducer");
                }
            }));
        }
        if (!executor.waitUntil([] { return false; })) {
            falseReturns.fetch_add(1);
            GuestExecutionGuard::stopForRejectedImport("__imp__NoProducer");
        }
    }, {100, 32, 3}, noBind);
    CHECK(result.execution.reason == GuestExecutionStopReason::RejectedImport);
    CHECK(result.execution.detail == "__imp__NoProducer" && falseReturns.load() == 1);
    // No false positive: a timed peer keeps the run alive until it notifies.
    std::mutex mutex;
    bool flag = false;
    result = executor.run(1, [&] {
        CHECK(executor.enqueue(2, [&] {
            for (int i = 0; i < 40; ++i) {
                const auto deadline = Clock::now() + std::chrono::milliseconds(1);
                CHECK(executor.waitUntil([&] { return Clock::now() >= deadline; }, deadline));
            }
            { std::lock_guard lock(mutex); flag = true; }
            wakeGuestWaiters(1);
        }));
        CHECK(executor.waitUntil([&] { std::lock_guard lock(mutex); return flag; }));
    }, {100000, 32, 2}, noBind);
    CHECK(result.execution.completed());
    std::puts("QUIESCENCE finish_no_runnable=1 wait_false=1 one_false_of_three=1 timed_peer_no_false_positive=1");
}

[[maybe_unused]] void testLanesConcurrency() {
    setGuestThreadDomainPolicy([](std::uint32_t token, std::uint32_t, std::uint32_t) {
        return token == 2 ? 1U : token >= 3 ? 2U : 0U; });
    GuestCooperativeExecutor executor;
    std::atomic<int> arrived{0}, inLane2{0}, laneViolations{0};
    bool mainSaw = false, workerSaw = false;
    const auto started = Clock::now();
    const auto result = executor.run(1, [&] {
        CHECK(executor.enqueue(2, [&] {
            FakeContext ctx{};
            fast(ctx, "lane1_entry");
            arrived.fetch_add(1);
            const auto until = Clock::now() + std::chrono::seconds(2);
            while (arrived.load() < 2 && Clock::now() < until) {} // No checkpoint while waiting.
            workerSaw = arrived.load() >= 2;
            for (int i = 0; i < 200000; ++i) fast(ctx, "lane1_work");
        }));
        for (std::uint32_t token = 3; token <= 4; ++token) { // Same lane: must never overlap.
            CHECK(executor.enqueue(token, [&] {
                FakeContext ctx{};
                for (int i = 0; i < 500; ++i) {
                    if (inLane2.fetch_add(1) != 0) laneViolations.fetch_add(1);
                    spinFor(std::chrono::microseconds(2));
                    inLane2.fetch_sub(1);
                    for (int j = 0; j < 64; ++j) fast(ctx, "lane2_work");
                }
            }));
        }
        FakeContext ctx{};
        fast(ctx, "arm_lanes");
        arrived.fetch_add(1);
        const auto until = Clock::now() + std::chrono::seconds(2);
        while (arrived.load() < 2 && Clock::now() < until) {}
        mainSaw = arrived.load() >= 2;
        for (int i = 0; i < 200000; ++i) fast(ctx, "lane0_work");
    }, {1000000000, 1024, 5}, noBind);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    CHECK(result.execution.completed());
    CHECK(mainSaw && workerSaw);            // Lanes 0 and 1 ran guest code at the same time.
    CHECK(laneViolations.load() == 0);      // Tasks sharing lane 2 never did.
    const auto stats = guestParallelStats();
    CHECK(stats.maxConcurrentPermits >= 2);
    std::printf("LANES_CONCURRENCY overlap=1 same_lane_violations=0 max_concurrent=%llu elapsed_ms=%lld\n",
                static_cast<unsigned long long>(stats.maxConcurrentPermits), static_cast<long long>(ms));
    setGuestThreadDomainPolicy({});
}

void testBoostPendingAndPriorities() {
    GuestCooperativeExecutor executor;
    auto result = executor.run(1, [&] {
        CHECK(executor.enqueue(2, [&] { native("boosted_worker"); }));
        const std::uint32_t worker = 2, self = 1, unknown = 99, twice[2] = {2, 2};
        CHECK(executor.boostEventWaiters(&worker, 1)); // Registered but not parked: pending boost.
        CHECK(!executor.boostEventWaiters(&self, 1));
        CHECK(!executor.boostEventWaiters(&unknown, 1));
        CHECK(!executor.boostEventWaiters(twice, 2));
        CHECK(!executor.setPriority(1, 3) && !executor.setPriority(99, 0) && executor.setPriority(2, 2));
    }, {100, 32, 2}, noBind);
    CHECK(result.execution.completed() && guestParallelStats().boostsPending == 1);
    // Priority weights within one domain (gil semantics): slices of quantum << shift.
    for (const auto priority : {-2, 0, 2}) {
        std::atomic<long> mainCount{0}, workerCount{0};
        result = executor.run(1, [&] {
            CHECK(executor.setPriority(1, priority));
            CHECK(executor.enqueue(2, [&] { FakeContext ctx{}; for (;;) { workerCount.fetch_add(1); fast(ctx, "weighted_worker"); } }));
            FakeContext ctx{};
            for (;;) { mainCount.fetch_add(1); fast(ctx, "weighted_main"); }
        }, {40001, 16, 2}, noBind);
        CHECK(result.execution.reason == GuestExecutionStopReason::FunctionEntryBudgetExceeded);
        const auto shift = portablePrioritySliceShift(priority);
        const double expected = shift >= 0 ? double(1 << shift) : 1.0 / double(1 << -shift);
        const double ratio = double(mainCount.load()) / double(std::max<long>(1, workerCount.load()));
        CHECK(ratio > expected * 0.8 && ratio < expected * 1.25);
        std::printf("PRIORITY priority=%d ratio=%.3f expected=%.3f\n", priority, ratio, expected);
    }
}

void testDelayYieldsAndWaits() {
    GuestCooperativeExecutor executor;
    std::atomic<bool> workerRan{false};
    const auto result = executor.run(1, [&] {
        native("before_worker");
        CHECK(executor.enqueue(2, [&] { workerRan.store(true); }));
        CHECK(!workerRan.load());
        CHECK(executor.delayUntil(Clock::now())); // Zero delay relinquishes the permit once.
        CHECK(workerRan.load());
        const auto deadline = Clock::now() + std::chrono::milliseconds(3);
        CHECK(executor.delayUntil(deadline));
        CHECK(Clock::now() >= deadline);
    }, {100, 32, 2}, noBind);
    CHECK(result.execution.completed() && result.waitOperations == 2);
    std::puts("DELAY zero_delay_yields=1 deadline_respected=1");
}
#endif
} // namespace

int main() {
    Watchdog watchdog(240);
    std::printf("GUEST_PARALLEL_EXECUTOR_TEST mode=%d\n", RECOMP_GUEST_THREADS);
    testFastPathAccounting();
    testFastPathBudgetExact();
    testScopeBudgetExact();
    testTaskCreationEvidence();
#if RECOMP_GUEST_THREADS == 0
    testCooperativeParity();
#else
    testPingPong(false);
    testLostWakeupWindow();
    testPollFallback();
    testStopPropagation();
    testStopDuringHandoffAndExclusiveWait();
    testExclusivity();
    testHandoffParity();
    testQuiescence();
    testBoostPendingAndPriorities();
    testDelayYieldsAndWaits();
#if RECOMP_GUEST_THREADS == 2
    testPingPong(true);
    testLanesConcurrency();
#endif
#endif
    std::printf("GUEST_PARALLEL_EXECUTOR_TEST_PASS mode=%d\n", RECOMP_GUEST_THREADS);
    return 0;
}
