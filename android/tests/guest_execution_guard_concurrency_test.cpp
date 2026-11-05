// Concurrency test for guest_execution_guard.cpp's MMIO handler exclusion and clocks (multicore plan
// WP4, local-analysis/attempts/v12/src/guest_execution_guard.cpp). Build twice: default
// (cooperative) and with -DRECOMP_GUEST_THREADS=2 (lanes) for both this file and the guard.
// - CP_RB_WPTR submission: same-thread re-entry and another thread's store while a handler runs are
//   rejected in every mode (the former global flag's fail-closed contract); accepted handler bodies
//   never overlap (plain counter under 8 threads).
// - XMA registers: an overlapping access is rejected in cooperative builds and waits in multicore
//   builds; handler bodies never overlap.
// - Clocks (multicore builds): concurrent uptime and GPU-clock readers all succeed, each thread sees
//   non-decreasing values, no value is below one already returned to another thread, and a source
//   regressing on one thread is still rejected.
#include "guest_execution_guard.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace recomp::bootstrap;

namespace {
[[noreturn]] void failTest(const char* what, unsigned line) {
    std::fprintf(stderr, "GUARD_CONCURRENCY_FAIL line=%u %s\n", line, what);
    std::abort();
}
#define CHECK(condition) do { if (!(condition)) failTest(#condition, __LINE__); } while (0)
#if defined(RECOMP_GUEST_THREADS) && RECOMP_GUEST_THREADS != 0
constexpr bool Parallel = true;
#else
constexpr bool Parallel = false;
#endif
template<class F> void inGuest(F&& body) {
    GuestExecutionGuard guard;
    const auto result = guard.run(1, [&] { body(); });
    CHECK(result.completed());
}
bool submit(std::uint32_t word) { return tryWriteGuestGraphicsSubmission(0x7FC80714, 4, word); }
bool xma(std::uint32_t address) { std::uint64_t value = 0; return tryAccessGuestXmaRegisters(address, 4, false, value); }
}

int main() {
    // ---- Submission: rejection of nested and overlapping stores ----
    std::atomic<int> phase{0};
    std::atomic<bool> nestedRejected{false};
    CHECK(bindGuestGraphicsSubmission([&](std::uint32_t word) {
        if (word == 1) {
            nestedRejected = !submit(2); // Same-thread re-entry.
            phase = 1;
            while (phase.load() != 2) std::this_thread::yield();
        }
        return true;
    }));
    std::thread holder([&] { inGuest([&] { CHECK(submit(1)); }); });
    while (phase.load() != 1) std::this_thread::yield();
    inGuest([&] { CHECK(!submit(3)); }); // Another thread inside the handler: rejected in every mode.
    phase = 2;
    holder.join();
    CHECK(nestedRejected.load());
    inGuest([&] { CHECK(submit(4)); });
    resetGuestGraphicsSubmission();
    // ---- Submission: accepted bodies never overlap ----
    unsigned long long inside = 0; // Plain: only an exclusive handler may touch it.
    CHECK(bindGuestGraphicsSubmission([&](std::uint32_t) { ++inside; return true; }));
    constexpr unsigned Threads = 8, Calls = 20000;
    std::atomic<unsigned long long> accepted{0}, rejected{0};
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < Threads; ++t) workers.emplace_back([&] {
        inGuest([&] { for (unsigned i = 0; i < Calls; ++i) (submit(i) ? accepted : rejected)++; });
    });
    for (auto& w : workers) w.join();
    workers.clear();
    CHECK(inside == accepted.load() && accepted.load() + rejected.load() == Threads * Calls && accepted.load() > 0);
    resetGuestGraphicsSubmission();

    // ---- XMA registers: rejected (cooperative) or serialized (multicore) ----
    std::atomic<int> xmaPhase{0};
    unsigned long long xmaInside = 0;
    CHECK(bindGuestXmaRegisters([&](std::uint32_t address, bool, std::uint32_t& value) {
        if (address == 0x7FEA1800 && xmaPhase.load() == 0) {
            xmaPhase = 1;
            while (xmaPhase.load() != 2) std::this_thread::yield();
        }
        ++xmaInside;
        value = 0;
        return true;
    }));
    std::thread xmaHolder([&] { inGuest([&] { CHECK(xma(0x7FEA1800)); }); });
    while (xmaPhase.load() != 1) std::this_thread::yield();
    std::atomic<bool> overlapDone{false}, overlapAccepted{false};
    std::thread overlap([&] { inGuest([&] { overlapAccepted = xma(0x7FEA1804); overlapDone = true; }); });
    if constexpr (Parallel) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(!overlapDone.load()); // Waits for the holder instead of being rejected.
        xmaPhase = 2;
        overlap.join();
        CHECK(overlapAccepted.load());
    } else {
        overlap.join();
        CHECK(overlapDone.load() && !overlapAccepted.load()); // Former global-flag contract.
        xmaPhase = 2;
    }
    xmaHolder.join();
    std::atomic<unsigned long long> xmaAccepted{0};
    for (unsigned t = 0; t < Threads; ++t) workers.emplace_back([&] {
        inGuest([&] { for (unsigned i = 0; i < Calls; ++i) if (xma(0x7FEA1818)) ++xmaAccepted; });
    });
    for (auto& w : workers) w.join();
    workers.clear();
    CHECK(xmaInside == xmaAccepted.load() + (Parallel ? 2 : 1));
    if constexpr (Parallel) CHECK(xmaAccepted.load() == Threads * Calls);
    resetGuestXmaRegisters();

    // ---- Clocks under concurrent readers (multicore contract) ----
    if constexpr (Parallel) {
        const auto start = std::chrono::steady_clock::now();
        CHECK(bindGuestScalarUptimeClock(0x4000, [start] {
            return std::optional<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count());
        }));
        CHECK(bindExperimentalGuestGpuClock([start]() -> std::optional<std::uint64_t> {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        }));
        std::atomic<std::uint64_t> uptimeSeen{0}, gpuSeen{0};
        for (unsigned t = 0; t < Threads; ++t) workers.emplace_back([&] {
            inGuest([&] {
                std::uint64_t lastUptime = 0, lastGpu = 0;
                for (unsigned i = 0; i < Calls; ++i) {
                    const auto floor = uptimeSeen.load();
                    std::uint64_t value = 0;
                    CHECK(tryReadGuestScalarUptime(0x4010, 4, value));
                    CHECK(value >= floor && value >= lastUptime);
                    lastUptime = value;
                    auto seen = uptimeSeen.load();
                    while (seen < value && !uptimeSeen.compare_exchange_weak(seen, value)) {}
                    const auto gpuFloor = gpuSeen.load();
                    CHECK(tryReadExperimentalGuestGpuClock(0x7FC8112C, 4, value));
                    CHECK(value >= gpuFloor && value >= lastGpu); // ns/2 stays far below 2^32 here
                    lastGpu = value;
                    seen = gpuSeen.load();
                    while (seen < value && !gpuSeen.compare_exchange_weak(seen, value)) {}
                }
            });
        });
        for (auto& w : workers) w.join();
        workers.clear();
        CHECK(experimentalGuestGpuClockReads() == Threads * Calls);
        resetGuestScalarUptimeClock();
        resetExperimentalGuestGpuClock();
        // A source that regresses on one thread is still a source error.
        thread_local std::uint64_t step = 10;
        CHECK(bindExperimentalGuestGpuClock([]() -> std::optional<std::uint64_t> { return step--; }));
        std::thread regress([] {
            std::uint64_t value = 0;
            CHECK(tryReadExperimentalGuestGpuClock(0x7FC8112C, 4, value) && value == 5);
            CHECK(!tryReadExperimentalGuestGpuClock(0x7FC8112C, 4, value));
        });
        regress.join();
        resetExperimentalGuestGpuClock();
    }
    std::printf("GUARD_CONCURRENCY_PASS mode=%s submissions_accepted=%llu rejected=%llu xma_accepted=%llu\n",
        Parallel ? "multicore" : "cooperative", accepted.load(), rejected.load(), xmaAccepted.load());
}
