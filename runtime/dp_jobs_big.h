#pragma once
// drive-path package (2026-10-03; switch files/private-trace/diag-dp-jobs-big, re-read every 2 s by the placement thread in
// trace_runner.cpp): the four RAGE job workers (640f5000, 64108000, 6411b000, 6412e000) pinned to the big cores {3-6} while
// the file exists. This is v196's diag-place-jobs-big (read once at boot, so no in-run A/B was possible) as a runtime switch.
// Off: the job workers' recorded placement is dropped at the same 2 s tick, so the ranking that follows re-applies their mask
// ({0-6} unless heavy) - without that the ranking would see its old entry and leave them on {3-6}.
// Why (drive-path report): while driving the frame is paced by main's loop (the render thread waits 6.4-8.2 ms per frame for
// main's kick; main waits for the render thread 0.02-0.05 ms per frame). On main's loop the job workers run 3.0-3.2 ms per
// loop (+ ~0.8 ms of wakes, ~10 completion waits per loop) vs 1.2 ms in the house; the four job threads run at ~1.88 GHz
// effective (latstat-v427-1: A715 threads 2.2-2.3, the A510 maximum is 2.02), i.e. largely on the little cores.
// Scheduling only: affinity masks. Nothing guest-visible changes (same code, same data, same synchronisation).
#include <array>
#include <atomic>
#include <cstddef>
#include "cpu_topology.h" // topology lane: "big" is the discovered mid role

namespace recomp_dp {
// topology lane: the mid role (Thor: 0x78 = cpu3-6, A715/A710; cpu7 stays the render thread's (v34), cpu0-2 are the A510s).
inline unsigned bigMask() noexcept { return recomp_topology::roles().mid.mask; }

struct JobsBigSwitch {
    std::atomic<bool> on{false};        // the file at the last 2 s poll
    std::atomic<int> applied{0};        // 1 while on, every job worker known, the last pin of each succeeded
    std::atomic<unsigned long long> polls{0}, ons{0}, offs{0}, pins{0}, failures{0};
};
inline JobsBigSwitch& jobsBig() noexcept { static JobsBigSwitch s; return s; }

// The 2 s poll (before the tick's ranking). On an on -> off transition, every job worker's entry is erased from the
// placement map (the ranking then re-applies its mask in the same tick). Returns the new state.
template <class Placed, class Tids>
bool pollJobsBig(JobsBigSwitch& s, bool fileOn, Placed& placed, const Tids& tids) noexcept {
    s.polls.fetch_add(1, std::memory_order_relaxed);
    const bool was = s.on.exchange(fileOn, std::memory_order_relaxed);
    if (fileOn && !was) s.ons.fetch_add(1, std::memory_order_relaxed);
#if !defined(RECOMP_DP_MUTANT_NO_ERASE) // test mutant: switching off leaves the ranking's stale entries
    if (was && !fileOn) {
        s.offs.fetch_add(1, std::memory_order_relaxed);
        for (const auto& t : tids)
            if (const int tid = t.load(std::memory_order_relaxed)) placed.erase(tid);
    }
#endif
    if (!fileOn) s.applied.store(0, std::memory_order_relaxed);
    return fileOn;
}

// The 250 ms pin (with v196's boot switch or this one on): every known job worker gets bigMask(). set(tid, mask) == 0 on
// success. Updates `applied` (reported only for this runtime switch).
template <class Tids, class Set>
void pinJobsBig(JobsBigSwitch& s, const Tids& tids, Set&& set) noexcept {
    bool all = true;
    for (const auto& t : tids) {
        const int tid = t.load(std::memory_order_relaxed);
        if (!tid) { all = false; continue; }
#if defined(RECOMP_DP_MUTANT_WRONG_MASK) // test mutant: the little cores stay in the mask
        const unsigned mask = recomp_topology::roles().allButPrime.mask;
#else
        const unsigned mask = bigMask();
#endif
        if (set(tid, mask) == 0) s.pins.fetch_add(1, std::memory_order_relaxed);
        else { all = false; s.failures.fetch_add(1, std::memory_order_relaxed); }
    }
    s.applied.store(all && s.on.load(std::memory_order_relaxed) ? 1 : 0, std::memory_order_relaxed);
}
} // namespace recomp_dp
