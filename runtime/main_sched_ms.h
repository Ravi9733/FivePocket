#pragma once
// main-sched (2026-10-04; runtime switches, scheduling only - no guest-visible state changes): main while it holds the X3 prime
// core (cpu7) through the v245 swap (the render thread waits for main's kick; main's mask is {7}).
// Evidence (milestone-v429-1 timelines + profiles, v404 swap trigger): on main's loop critical path main is off-CPU 1.15 ms per
// driving loop (32%) while it holds cpu7 (house 0.46 / 19%, outdoor 0.28 / 15%), of 1.75 ms of on-path off-CPU in all; ~0.9 ms
// of it in multi-ms holes that start at main's own Sleep(0) sched_yield (p50 ~3-4 ms). Nothing of ours can run on cpu7 then
// (the render thread is parked, every other thread is ranked to {3-6} / {0-6} / {0-2}), so the yield hands cpu7 to other
// processes' nice-0 threads (dual-screen assistant, kworkers, ...) until their slice / the next tick.
//   diag-ms-prime-noyield  (ModeNoYield) main's Sleep(0) fast path skips the host sched_yield while main holds cpu7
//                          (guest_import_registry.cpp -> sleepZeroYield). The guest call, its counters and the executor's
//                          slice bookkeeping are unchanged; only the host yield is skipped.
//   diag-ms-prime-nice     (prime_swap_v245.h: PrimeSwap::boostNice) main runs at nice PrimeNice while it holds cpu7; the
//                          release restores the nice the placement thread wants for main (its 2 s re-application is deferred to
//                          the release while boosted).
//   diag-ms-all            both.
// Neither acts while the v251 extras (scene workers 0-1 on cpu7) are enabled: they could then queue on cpu7 behind main.
#include <atomic>
#include <cstdint>

namespace recomp_ms {
constexpr unsigned ModeNoYield = 1u;
constexpr int PrimeNice = -20; // the app's RLIMIT_NICE allows it (trace_runner's v336 render thread uses it)
struct State {
    std::atomic<unsigned> mode{0};                       // ModeNoYield bit (set by the renderer's window poll)
    std::atomic<unsigned long long> skipped{0};          // Sleep(0) host yields skipped (main on cpu7)
    std::atomic<unsigned long long> yielded{0};          // Sleep(0) host yields made through the registry's fast path
};
inline State& state() noexcept { static State s; return s; }

// The decision (host-tested): skip the host sched_yield of this Sleep(0)?
//   mode      the switch bits; swapped: main currently holds cpu7 (v245 swappedView); extras: the v251 extras are enabled;
//   isMain    the calling guest thread is main (0x63001000).
inline bool skipYield(unsigned mode, bool swapped, bool extras, bool isMain) noexcept {
#if defined(RECOMP_MS_MUTANT_ANY_THREAD) // test mutant: every thread skips while main holds cpu7
    (void)isMain; return (mode & ModeNoYield) && swapped && !extras;
#elif defined(RECOMP_MS_MUTANT_NO_SWAP_CHECK) // test mutant: main skips its yields wherever it runs
    (void)swapped; return (mode & ModeNoYield) && !extras && isMain;
#else
    return (mode & ModeNoYield) && swapped && !extras && isMain;
#endif
}
} // namespace recomp_ms
