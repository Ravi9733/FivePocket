#pragma once
// prime-exchange ("px", 2026-10-04; runtime switches, scheduling only - no guest-visible state changes): the EXCHANGE form of the
// v245 prime swap. Today (v245): the render thread (6408e000, cpu7 alone) lends cpu7 to main (63001000) while it waits for main's
// frame kick; main goes back to its ranking mask ({3-6}) just before it wakes the render thread. With px, main KEEPS cpu7 at the
// kick and the render thread runs the first d ms of its frame on the mid cores (masks disjoint: render {3-6}, main {7}); then the
// render thread exchanges back (main -> its ranking mask first, then the render thread -> {7}). d comes from a controller fed by
// the render thread's measured residual slack (the length of its wait for the kick). Study: agents/prime-share/report.md (the one
// candidate left: unproven; model -0.5 ms, range -0.2..-0.9). Design, invariants and constants: agents/prime-exchange/DESIGN.md.
//   diag-px-exchange   controller (analyst rule P2m + a floor + a short-frame rule, below)
//   diag-px-fixed-4    fixed d = 4 ms (calibration; the floor and the short-frame rule still apply); wins over the controller
//   diag-px-fixed-8    fixed d = 8 ms; wins over fixed-4
// With all three absent nothing here acts: every px entry point returns after one to three relaxed loads, the placement thread
// keeps v458's unlocked pin, and PrimeSwap makes v458's decisions and setter calls exactly (host test: px_diff_driver).
// This header holds what does not need PrimeSwap: constants, the controller (pure, host-tested), the clock and the shared state.
// The protocol steps are in prime_swap_v245.h (pxPark / pxGate / pxAfterRelease / pxRenderPoll / pxPlacementPin).
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace recomp_px {
constexpr unsigned ModeController = 1u, ModeFixed = 2u;  // State::mode (0 = off)
// (topology merge: the exchange's two masks are discovered roles, read where they are used (prime_swap_v245.h):
// recomp_topology::roles().prime.mask - {7} on the Thor - and roles().mid.mask - {3-6}, the placement loop's own
// fallback mask for the prime owner. The literals PrimeMask = 0x80u, MidMask = 0x78u stood here.)
constexpr std::uint32_t RenderToken = 0x6408e000u;

// ---- controller constants (microseconds). Data: agents/prime-share/analyst-a/sim-records-{house,drive}.json (v455, 770 / 581
// loops), re-simulated with exactly this rule by agents/prime-exchange/sim/px_sim.py (DESIGN.md section 3 has the table).
constexpr int TargetUs = 3000;      // residual slack the controller steers min(last 4) to (analyst P2m: "target about 3 ms")
constexpr int GainDiv = 4;          // d += (min4 - target) / 4 per loop (P2m's 0.25)
constexpr unsigned Window = 4;      // min over the last 4 loops (slack has lag-1 autocorrelation 0.06: only the level is tracked)
constexpr int FloorUs = 1500;       // a kick wait shorter than this: the render thread is (nearly) the limiter -> d = 0 at once
constexpr int CapUs = 8000;         // never more than 8 ms of the frame on a mid core ...
constexpr int FrameDiv = 2;         // ... nor more than half of the render thread's last frame (kick wake -> its next kick wait)
constexpr int MinFrameUs = 6000;    // a render frame shorter than this (menus, loading, pause): d = 0
constexpr int MinLendUs = 2000;     // below this an exchange does not pay for its two extra render-thread migrations (the
                                    // reviewer-corrected model breaks even at d = 1 ms, gains 0.06-0.08 ms at 2 ms): no exchange
constexpr int SaneUs = 250000;      // a slack or frame sample above this is a stall / load, not a frame: controller reset
constexpr int RetryUs = 8000;       // main's restore failed (never seen): the render thread stays on {3-6} and retries this often

// d for the frame that starts now, from the kick wait that just ended (slackUs) and the render frame before it (frameUs, 0 =
// unknown). fixedUs > 0: the calibration switch (d = fixedUs wherever the safety rules allow an exchange at all).
struct Controller {
    std::array<int, Window> hist{};
    unsigned n = 0, at = 0;
    int d = 0;                       // the controller's own value (may sit below MinLendUs while it ramps)
    unsigned long long floorDrops = 0, shortDrops = 0; // (render thread; copied to the atomics by State)
    void reset() noexcept { n = at = 0; d = 0; }
    // A loop the render thread could not afford (its present came while it was still lent): a zero sample, so min(last 4)
    // keeps d at 0 for the next four loops, then the ramp starts again.
    void penalize() noexcept { hist[at] = 0; at = (at + 1) % Window; if (n < Window) ++n; d = 0; }
    int step(int slackUs, int frameUs, int fixedUs) noexcept {
        hist[at] = slackUs; at = (at + 1) % Window; if (n < Window) ++n;
        int m = hist[0];
        for (unsigned i = 1; i < n; ++i) m = std::min(m, hist[i]);
        int cap = CapUs;
        if (frameUs > 0) cap = std::min(cap, frameUs / FrameDiv);
        int next = fixedUs > 0 ? std::min(fixedUs, cap) : std::clamp(d + (m - TargetUs) / GainDiv, 0, cap);
#if !defined(RECOMP_PX_MUTANT_NO_FLOOR) // test mutant: the controller ignores the floor
        if (slackUs < FloorUs) { if (d > 0 || fixedUs > 0) ++floorDrops; next = 0; }
#endif
        if (frameUs > 0 && frameUs < MinFrameUs) { if (next > 0) ++shortDrops; next = 0; }
        d = fixedUs > 0 ? 0 : next;  // (fixed mode keeps no ramp state)
        return next >= MinLendUs ? next : 0;
    }
};

// Microseconds on a monotonic clock, without a syscall: the ARM generic timer (as guest_registry_concurrency.h registryTicks),
// steady_clock elsewhere. Split division: cntvct * 1e6 would overflow after ~11 days of uptime.
inline std::uint64_t nowUs() noexcept {
#if defined(__aarch64__)
    std::uint64_t t, f;
    asm volatile("mrs %0, cntvct_el0" : "=r"(t));
    asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
    if (!f) f = 19200000;
    return t / f * 1000000u + t % f * 1000000u / f;
#else
    return std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

struct alignas(64) State {
    // -- line 0: read-mostly. lentView is read by EVERY task's slow checkpoint (pxRenderPoll); nothing on this line is written
    //    while px is off, and with px on only at a lend / exchange back (twice a frame).
    std::atomic<bool> lentView{false};       // the render thread's mask is {3-6} through an exchange (the truth is `lent` under m)
    std::atomic<bool> stale{false};          // placement backstop: a hold older than two 250 ms ticks ends at the next check point
    std::atomic<unsigned> mode{0};           // the switch (set by the renderer's window poll)
    std::atomic<unsigned> fixedUs{0};        // ModeFixed: d
    std::atomic<int> renderTid{0};           // the render thread's host tid (trace_runner; 0 with diag-prime-main: px inert)
    std::uint64_t (*clock)() noexcept = &nowUs; // (never written by the build; host tests substitute a fake clock)
    // -- under PrimeSwap::m
    alignas(64) bool lent = false;
    bool placementAck = false;               // the placement thread pins the render thread under m (written by it alone, under m)
    unsigned holdSeq = 0, seenSeq = 0, staleTicks = 0; // (seenSeq / staleTicks: placement thread, under m)
    // -- written by the render thread (main reads planUs, waitOpen and parkUs once, at its kick, and zeroes deadlineUs at a lend)
    alignas(64) std::atomic<bool> waitOpen{false}; // its first not-ready wait after a present (the wait for main's kick), px on
    std::atomic<unsigned> planUs{0};         // d the controller wants for the next frame
    std::atomic<std::uint64_t> parkUs{0};    // when that wait began
    std::atomic<std::uint64_t> deadlineUs{0};// the current hold: 0 = not anchored yet, 1 = due now, else the exchange-back time
    unsigned lastMode = 0;
    std::uint64_t lastWakeUs = 0, anchorUs = 0;
    Controller ctl;
    // -- counters (cumulative; RENDER_DIAG_PX prints the deltas of a 60-present window)
    alignas(64) std::atomic<unsigned long long> exchanges{0}; // (the alignment starts the counters' own lines)
    std::atomic<unsigned long long> anchored{0}, spurious{0}, backs{0}, backsByMain{0}, rejoins{0}, dSumUs{0}, holdSumUs{0},
        lateSumUs{0}, samples{0}, slackSumUs{0}, floorDrops{0}, shortDrops{0}, resets{0}, lendFailures{0}, backFailures{0},
        homeFailures{0}, backstops{0};
    std::atomic<unsigned> slackMinUs{~0u}, lateMaxUs{0}; // (window values: the logger exchanges them back)

    // render thread: the wait for main's kick begins (the armed beforePark).
    void armedPark(unsigned md) noexcept {
        const std::uint64_t now = clock();
        if (md != lastMode || now - parkUs.load(std::memory_order_relaxed) > 2u * std::uint64_t(SaneUs)) { // switched, or px was off / stalled
            ctl.reset(); planUs.store(0, std::memory_order_relaxed); lastMode = md; lastWakeUs = 0;
        }
        parkUs.store(now, std::memory_order_relaxed);
        waitOpen.store(true, std::memory_order_relaxed);
    }
    // render thread: that wait completed: one residual-slack sample, one controller step.
    void waitDone(unsigned md) noexcept {
        if (!waitOpen.load(std::memory_order_relaxed)) return;
        waitOpen.store(false, std::memory_order_relaxed);
        if (!md) return;
        const std::uint64_t now = clock(), park = parkUs.load(std::memory_order_relaxed);
        const std::uint64_t slack = now - park, frame = lastWakeUs ? park - lastWakeUs : 0;
        lastWakeUs = now;
        if (slack > std::uint64_t(SaneUs) || frame > std::uint64_t(SaneUs)) {
            ctl.reset(); planUs.store(0, std::memory_order_relaxed); resets.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const unsigned long long f0 = ctl.floorDrops, s0 = ctl.shortDrops;
        const int d = ctl.step(int(slack), int(frame), md == ModeFixed ? int(fixedUs.load(std::memory_order_relaxed)) : 0);
        planUs.store(unsigned(d), std::memory_order_relaxed);
        if (ctl.floorDrops != f0) floorDrops.fetch_add(1, std::memory_order_relaxed);
        if (ctl.shortDrops != s0) shortDrops.fetch_add(1, std::memory_order_relaxed);
        samples.fetch_add(1, std::memory_order_relaxed);
        slackSumUs.fetch_add(slack, std::memory_order_relaxed);
        unsigned seen = slackMinUs.load(std::memory_order_relaxed);
        while (unsigned(slack) < seen && !slackMinUs.compare_exchange_weak(seen, unsigned(slack), std::memory_order_relaxed)) {}
    }
    // render thread: its present came while it was still lent (frame shorter than d, or the kick found it mid-frame): no exchange
    // until the controller has ramped again.
    void shortFrame() noexcept {
        ctl.penalize(); planUs.store(0, std::memory_order_relaxed); shortDrops.fetch_add(1, std::memory_order_relaxed);
    }
};
inline State g_state; // (constant-initialised: no guard on the checkpoint path)
inline State& state() noexcept { return g_state; }
} // namespace recomp_px
