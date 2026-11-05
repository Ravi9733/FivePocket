#pragma once
// main-waits-2 (2026-10-04; runtime switches, scheduling only - no guest-visible state changes): the nice of main's nice-0
// producers while main holds the X3 prime core (cpu7) through the v245 swap, i.e. while the render thread waits for main's
// kick (frame phase W) - the window in which main Sleep(0)-polls for scene items and parks on job completions.
// Evidence (perf-audit/prof-v442-1 timelines + fp profiles, pooled sample estimates; agents/main-waits-2): main's Sleep(0) polls
// are 2.87 ms per outdoor loop (W 2.39; house 2.48 / W 2.07), all waiting on the four scene workers (the job workers are parked
// 99% of that time). Scene workers 0-1 (nice -4 since v238) are on-CPU for all of their runnable time inside those polls;
// scene workers 2-3 (nice 0) are runnable but NOT running for 49% / 47% of it (house 31% / 30%): 2.7 thread-ms per outdoor
// frame in W. The job workers (nice 0) run 76% of their runnable time while main waits for them (W off-CPU 0.12-0.23 ms per
// worker per frame). v249 (job workers + scene workers 2-3 at nice -4 for the whole frame) cost the render thread +2.0 ms
// outdoors (R phase): these switches act only in the W window (or only outside it, scene-r), never on the job workers in R.
//   diag-mw2-scene-w  (ModeSceneW) scene workers 2-3 (6404c000, 6405f000) at min(base, Boost) while main holds cpu7.
//   diag-mw2-jobs-w   (ModeJobsW)  the four RAGE job workers (640f5000, 64108000, 6411b000, 6412e000) the same, W only.
//   diag-mw2-scene-r  (ModeSceneR) scene workers 2-3 at min(base, Boost) OUTSIDE that window (with scene-w: the whole frame).
// "base" is what the placement thread wants (v249: -4 with diag-v249-jobs-nice, else 0), applied every 2 s through
// applyBase(); with every switch off applyBase makes exactly v451's call (setpriority(tid, base), every 2 s) and the phase
// transitions make none. Transitions (both on the render thread): enterW() from PrimeSwap::beforePark after a successful swap,
// leaveW() from the render thread's PrimeSwap::release(false) when that wait completes (main's own release at the kick makes no
// extra syscall). A transition applies only values that differ from the last applied one. The placement thread's re-application
// inside a window applies the window's effective value (not the base), so it never undoes a boost mid-window. Also measured in
// every state: the W window's length (beforePark's swap -> the render thread's release), RENDER_DIAG_MW2 w_ms.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace recomp_mw2 {
constexpr unsigned ModeSceneW = 1u, ModeJobsW = 2u, ModeSceneR = 4u;
constexpr int Boost = -4;                   // scene workers 0-1's level (v238)
constexpr unsigned Jobs = 4, Slots = 6;     // slots 0-3: job workers (JobWorkerTokens order), 4-5: scene workers 2-3
using SetNice = int (*)(int tid, int nice); // 0 = success (trace_runner: setpriority)

// The nice a producer slot should have (host-tested): base, or min(base, Boost) when its group is boosted in this phase.
inline int effective(unsigned slot, int base, bool inW, unsigned mode) noexcept {
    const bool scene = slot >= Jobs;
#if defined(RECOMP_MW2_MUTANT_WRONG_GROUP) // test mutant: scene-w also boosts the job workers
    const bool boosted = scene ? (inW ? (mode & ModeSceneW) != 0 : (mode & ModeSceneR) != 0)
                               : (inW && (mode & (ModeJobsW | ModeSceneW)) != 0);
#else
    const bool boosted = scene ? (inW ? (mode & ModeSceneW) != 0 : (mode & ModeSceneR) != 0) : (inW && (mode & ModeJobsW) != 0);
#endif
    return boosted ? std::min(base, Boost) : base;
}

struct Producers {
    struct Slot { int tid = 0; int base = 0; int applied = 0; bool known = false, appliedKnown = false; };
    std::atomic<unsigned> mode{0};                // the switch bits (set by the renderer's window poll)
    std::atomic<bool> inWView{false};             // (render thread's fast check; the truth is inW under m)
    std::atomic<bool> boostedView{false};         // some slot holds a value != its base
    SetNice set = nullptr;
    std::atomic<unsigned long long> enters{0}, leaves{0}, sets{0}, setFailures{0}, baseCalls{0}, wNs{0}, wCount{0};
    std::uint64_t wStart = 0;                     // render thread only
#if defined(RECOMP_MW2_MUTANT_NO_LOCK)
    struct NoLock { void lock() {} void unlock() {} } m;
#else
    std::mutex m;
#endif
    std::array<Slot, Slots> slots{};
    bool inW = false;

    static std::uint64_t now() noexcept {
        return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    // Apply slot k's effective value if it differs from the last applied one (under m).
    void transitionLocked(unsigned k, unsigned md) noexcept {
        Slot& s = slots[k];
        if (!s.known || !set) return;
        const int want = effective(k, s.base, inW, md);
        if (s.appliedKnown && s.applied == want) return;
        if (set(s.tid, want) == 0) { s.applied = want; s.appliedKnown = true; sets.fetch_add(1, std::memory_order_relaxed); }
        else { s.appliedKnown = false; setFailures.fetch_add(1, std::memory_order_relaxed); } // retried at the next transition
    }
    void refreshBoostedLocked() noexcept {
        bool any = false;
        for (const auto& s : slots) any = any || (s.known && (!s.appliedKnown || s.applied != s.base));
        boostedView.store(any, std::memory_order_relaxed);
    }
    // Render thread: the v245 swap just gave main cpu7.
    [[gnu::noinline]] void enterW() noexcept {
        wStart = now();
        inWView.store(true, std::memory_order_relaxed);
        enters.fetch_add(1, std::memory_order_relaxed);
        const unsigned md = mode.load(std::memory_order_relaxed);
        if (!md && !boostedView.load(std::memory_order_relaxed)) { std::lock_guard<decltype(m)> l(m); inW = true; return; }
        std::lock_guard<decltype(m)> l(m);
        inW = true;
        for (unsigned k = 0; k < Slots; ++k) transitionLocked(k, md);
        refreshBoostedLocked();
    }
    // PrimeSwap::release: byMain = main at its kick (nothing to do here); the render thread when its wait completed.
    [[gnu::noinline]] void leaveW(bool byMain) noexcept {
        if (byMain || !inWView.load(std::memory_order_relaxed)) return;
        inWView.store(false, std::memory_order_relaxed);
        wNs.fetch_add(now() - wStart, std::memory_order_relaxed);
        wCount.fetch_add(1, std::memory_order_relaxed);
        leaves.fetch_add(1, std::memory_order_relaxed);
        const unsigned md = mode.load(std::memory_order_relaxed);
        std::lock_guard<decltype(m)> l(m);
        inW = false;
        if (!md && !boostedView.load(std::memory_order_relaxed)) return;
#if !defined(RECOMP_MW2_MUTANT_NO_RESTORE) // test mutant: the window's boost stays after the window
        for (unsigned k = 0; k < Slots; ++k) transitionLocked(k, md);
#endif
        refreshBoostedLocked();
    }
    // Placement thread (every 2 s, v249 block): the base nice of slot k (thread tid). Always makes one setter call, as v451's
    // setpriority did; the value is the slot's effective one in the current phase. Returns the setter's result.
    int applyBase(unsigned k, int tid, int base) noexcept {
        if (k >= Slots || !set) return -1;
        baseCalls.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<decltype(m)> l(m);
        Slot& s = slots[k];
        s.tid = tid; s.base = base; s.known = true;
#if defined(RECOMP_MW2_MUTANT_BASE_CLOBBER) // test mutant: the 2 s re-application undoes a boost inside the window
        const int want = base;
#else
        const int want = effective(k, base, inW, mode.load(std::memory_order_relaxed));
#endif
        const int rc = set(tid, want);
        if (rc == 0) { s.applied = want; s.appliedKnown = true; }
        else s.appliedKnown = false;
        refreshBoostedLocked();
        return rc;
    }
};
inline Producers& producers() noexcept { static Producers p; return p; }
// The calls PrimeSwap makes (out of line: one direct call in an existing block of beforePark / at the top of release, no new
// branch or static-init guard in the callers, so their CFG - and the PGO records of the functions they inline into - stay).
[[gnu::noinline]] inline void enterW() noexcept { producers().enterW(); }
[[gnu::noinline]] inline void leaveW(bool byMain) noexcept { producers().leaveW(byMain); }
} // namespace recomp_mw2
