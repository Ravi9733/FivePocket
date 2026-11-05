#pragma once
// main-wait (agent package "main-wait", 2026-10-03): honour the guest's critical-section spin count before queueing.
//
// The game initializes its hot critical sections with RtlInitializeCriticalSectionAndSpinCount(cs, 1000) (e.g. the RAGE task
// pool lock 0x83B9D348: sub_837126A0; the scene locks 0x83C5B780 / 0x83BD4930; 0x83C4D6E0), and our registry stores the spin
// metadata byte ((1000 + 255) >> 8 = 4, header byte 1) but never spins: every contended RtlEnterCriticalSection queues and parks
// at once (futex), and the FIFO front is woken at the owner's leave. On the 360 kernel a contended enter first retries for
// SpinCount iterations and only then waits. Timelines (v393/v407 drive, v414/v419 house, v419 in-car): 900-1600 CS parks/s,
// leave -> woken front 15-30 us p50 (60-100 us p90), 9-23 such handoffs per frame on main's W critical path (0.24-0.84
// ms/frame), the task pool lock alone 0.15-0.31 ms/frame; main itself parks on CS objects 0.2-0.4 ms per W.
//
// With files/private-trace/diag-mw-cs-spin (40 us) or diag-mw-cs-spin-100 (100 us) present (the renderer's 60-present window
// poll stores the budget in recomp_mw_cs_spin_us; 0 = off = v425 behaviour), a contended enter of a section whose spin byte
// is nonzero, that is owned by another thread and has NO queued waiter, releases the registry lock and the CS stripes, watches
// the section's guest OwningThread word (read-only, relaxed) until it reads 0 or the budget is spent (or a world stop begins),
// relocks and re-runs the unchanged acquisition logic from the top. Exactness: the acquisition rule is untouched (a thread
// acquires only a free section with no queued waiter, or its own); a queued waiter keeps FIFO priority (a section with
// waiters is never spun on, and a retry that finds a queue queues behind it); the watcher writes nothing and is not visible as
// a waiter. An enter that watched is indistinguishable from the same enter issued later - an interleaving the original kernel
// produces whenever it spins. Only when a contended enter completes changes.
#include <atomic>
#include <chrono>
#include <cstdint>

extern "C" std::atomic<unsigned> recomp_mw_cs_spin_us; // strong in guest_import_registry.cpp

namespace recomp_mw {

struct CsSpinCounters {
    std::atomic<unsigned long long> watches{0};   // pre-queue watches started
    std::atomic<unsigned long long> cleared{0};   // watches that saw the owner word clear
    std::atomic<unsigned long long> expired{0};   // watches that spent the budget (then the enter queued as before)
    std::atomic<unsigned long long> stopped{0};   // watches ended by a world stop (exclusive scope)
    std::atomic<unsigned long long> acquired{0};  // enters that acquired after a watch without queueing
    std::atomic<unsigned long long> queued{0};    // enters that queued after a watch (budget spent or a queue appeared)
    std::atomic<unsigned long long> watchNs{0};   // time spent watching
};
inline CsSpinCounters& counters() noexcept { static CsSpinCounters c; return c; }

inline std::uint32_t loadBigEndian32(const std::uint8_t* p) noexcept {
    const std::uint32_t raw = __atomic_load_n(reinterpret_cast<const std::uint32_t*>(p), __ATOMIC_RELAXED);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap32(raw);
#else
    return raw;
#endif
}

enum class WatchEnd { Cleared, Expired, Stopped };

// Watches the big-endian guest word at `owner` (4-byte aligned) until it reads 0, at most budgetNs; the clock and stop()
// are checked every 16 polls. *spentNs receives the time watched.
template <class Stop>
WatchEnd watchOwnerClear(const std::uint8_t* owner, std::uint64_t budgetNs, Stop stop, std::uint64_t* spentNs) noexcept {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto until = start + std::chrono::nanoseconds(budgetNs);
    WatchEnd end = WatchEnd::Expired;
    for (unsigned i = 1;; ++i) {
        if (loadBigEndian32(owner) == 0) { end = WatchEnd::Cleared; break; }
        if ((i & 15u) == 0) {
            if (stop()) { end = WatchEnd::Stopped; break; }
            if (Clock::now() >= until) break;
        }
#if defined(__aarch64__)
        asm volatile("yield" ::: "memory");
#elif defined(__x86_64__)
        __builtin_ia32_pause();
#endif
    }
    *spentNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
    return end;
}

// The watch applies only with the switch on, a nonzero guest spin byte, another thread owning the section and no queued
// waiter (FIFO), on an aligned section, with budget left in this enter.
inline bool shouldWatch(unsigned budgetUs, std::uint8_t spinByte, std::uint32_t owner, std::uint32_t self, bool queued,
                        std::uint32_t address, std::uint64_t spentNs) noexcept {
#if defined(RECOMP_MW_MUTANT_ANY_SPIN) // host-test mutant: ignores the guest's spin byte
    spinByte = 1;
#endif
    return budgetUs != 0 && spinByte != 0 && owner != 0 && owner != self && !queued && (address & 3u) == 0 &&
           spentNs < std::uint64_t{budgetUs} * 1000u;
}

// The renderer's window poll: the budget the switch files ask for (0 = off).
inline unsigned switchBudgetUs(bool spin40, bool spin100) noexcept { return spin100 ? 100u : spin40 ? 40u : 0u; }

} // namespace recomp_mw
