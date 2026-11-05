#pragma once
// v245 (A/B: files/private-trace/diag-v245-swap, polled by the renderer every 60 presents): the main thread (63001000) may
// use the X3 prime core (cpu7) exactly while the render thread (6408e000, cpu7 alone at nice -8) waits for main's next
// frame kick. v244-tl timelines (v243 behaviour): after each present the render thread's first park is on an object only
// main signals (a2c 20/22 outdoor, 22/26 house; b48 otherwise), and it waits there a median 11.8 ms outdoors / 8.4 ms in
// the house while cpu7 idles; main (on the saturated A715/A710 cores, ~170 ms/s runqueue wait) runs its pre-kick work in
// that window, which is on the critical path. Scheduling only: no guest-visible state changes.
//
// Protocol (one swap per frame at most):
//   arm()            render thread, at its present: the next park may swap.
//   beforePark(...)  render thread, when a wait's immediate check failed: if armed and enabled, main gets {7}.
//   release(...)     main, before it wakes the render thread; the render thread, when its wait completes: main gets the
//                    mask the placement ranking last gave it (mainMask), from main itself (tid 0) when main calls.
// A mutex orders the swap and its release, so main can never be left on {7} by an interleaving; a failed setaffinity
// (cpu7 paused by core_ctl: EINVAL) leaves nothing swapped.
// main-sched (main_sched_ms.h, switch diag-ms-prime-nice / diag-ms-all -> boostNice): after a successful swap main also runs at
// nice boostTo (-20) while it holds cpu7; every release restores the nice main had (or the one the placement thread asked for
// meanwhile: applyMainNice defers its 2 s re-application to the release while boosted). The restore follows the affinity
// change (main lands on cpu3-6 at the boosted weight, then drops back). Not with the v251 extras (they would share cpu7).
// main-waits-2 (mw2_producers.h, switches diag-mw2-scene-w / -jobs-w / -scene-r): the swap also enters the producers' W window
// (recomp_mw2::enterW, on the render thread, after a successful move of main to {7}); the render thread's release(false) leaves
// it (recomp_mw2::leaveW). Main's release(true) only calls leaveW(true), which returns at once.
// prime-exchange (prime_exchange_px.h, switches diag-px-exchange / diag-px-fixed-4 / -8; agents/prime-exchange/DESIGN.md): at its
// kick main may KEEP cpu7 and send the render thread to {3-6} instead (a "lend": one setter call on main's path, as today); the
// render thread exchanges back d ms after its wake, from its own check points, through the ordinary release(false): main leaves
// {7} first, then the render thread returns to {7}. States under m (swapped, lent): (0,0) v458's rest state; (1,0) v458's swap;
// (1,1) the hold (main {7}, render {3-6}); (0,1) only after a failed restore of main (the render thread then stays on {3-6} and
// retries). Three out-of-line calls carry it - pxPark at the top of beforePark, pxGate in place of release's swappedView test,
// pxAfterRelease at the end of release - so beforePark and release keep their control flow (and their PGO records); with the
// switches absent each returns after one to three relaxed loads and release tests swappedView exactly as before.
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include "cpu_topology.h" // topology lane: the prime seat ({7} on the Thor) and main's first home ({3-6}) are discovered roles
#include "mw2_producers.h"
#include "prime_exchange_px.h"

namespace recomp_v245 {
using SetAffinity = int (*)(int tid, unsigned mask); // 0 = success (tid 0 = the calling thread)
using SetNice = int (*)(int tid, int nice);          // main-sched: 0 = success
using GetNice = int (*)(int tid, int* nice);         // main-sched: 0 = success
struct PrimeSwap;
// prime-exchange (defined below the struct; noinline + noexcept: direct calls in existing blocks of their callers).
[[gnu::noinline]] inline void pxPark(PrimeSwap& s) noexcept;                      // render thread: top of beforePark
// pxGate returns `unsigned` (0 / 1), NOT `bool`, on purpose: release's `if (!pxGate(...)) return;` must reach the profile-use pass
// as `br (x == 0), return, continue` - the successor order v458's `if (!swappedView.load(...)) return;` has. A `bool` call folds to
// `br call, continue, return`, the CFG hash of release changes and its profile record (203,598 counts in round7-v456) is dropped;
// LLVM does not warn about that for an inline (COMDAT) function by default. tools/px_devcc.sh checks it (sensitivity case c).
[[gnu::noinline]] inline unsigned pxGate(PrimeSwap& s, bool byMain) noexcept;     // release: "run v458's release now?" (0 = no)
[[gnu::noinline]] inline void pxAfterRelease(PrimeSwap& s, bool byMain) noexcept; // release: main has just left {7} (under m)
struct PrimeSwap {
    std::atomic<bool> enabled{false};  // the A/B switch (set by the renderer's switch poll)
    std::atomic<bool> armed{false};    // a present happened since the last swap attempt
    std::atomic<int> mainTid{0};       // main's host thread
    // topology lane: a constant here, not the role itself: a member initialised from the discovered roles puts PrimeSwap's
    // construction code into every function that inlines state() (measured: their profile records stopped matching).
    // trace_runner.cpp's start-up initialiser stores the mid role ({3-6} on the Thor, the literal this had) before any thread runs.
    // (Until then - and in a PrimeSwap that a host test constructs for itself - a release pins main nowhere: NoPinning.)
    std::atomic<unsigned> mainMask{recomp_topology::NoPinning}; // the ranking's current mask for main (mid unless it said otherwise)
    std::atomic<bool> swappedView{false}; // (lock-free fast check; the truth is `swapped` under m)
    std::atomic<unsigned long long> swaps{0}, releasesByMain{0}, releasesByRender{0}, failures{0};
    // v251 (switch diag-v251-scene-prime): scene workers 0-1 may also use cpu7 during the same window (their ranking mask plus
    // cpu7), restored at the release; main keeps {7}.
    std::atomic<bool> extrasEnabled{false};
    std::array<std::atomic<int>, 2> extraTid{};
    std::array<std::atomic<unsigned>, 2> extraMask{recomp_topology::NoPinning, recomp_topology::NoPinning}; // (topology lane: as mainMask)
    bool extrasSwapped = false;
    std::atomic<unsigned long long> extraSwaps{0};
    SetAffinity set = nullptr;
    // main-sched (diag-ms-prime-nice): main's nice while it holds cpu7.
    std::atomic<bool> boostNice{false};
    int boostTo = -20;
    SetNice setNice = nullptr;
    GetNice getNice = nullptr;
    bool boosted = false; int restoreNice = 0; // (under m)
    std::atomic<unsigned long long> boosts{0}, boostFailures{0}, unboosts{0}, unboostFailures{0}, deferred{0};
#if defined(RECOMP_V245_MUTANT_NO_LOCK) // host-test mutant: the swap and its release unordered
    struct NoLock { void lock() {} void unlock() {} } m;
#else
    std::mutex m;
#endif
    bool swapped = false;
    int releaseRc = 0; // prime-exchange (under m): the setter result of release's last move of main (pxAfterRelease reads it)

    void arm() noexcept { armed.store(true, std::memory_order_relaxed); }
    // The render thread is about to park (its wait was not ready).
    void beforePark() noexcept {
        pxPark(*this); // prime-exchange: the wait for main's kick begins (slack clock); a hold still on rejoins here. Off: three loads.
        if (!armed.exchange(false, std::memory_order_relaxed) || !enabled.load(std::memory_order_relaxed)) return;
        const int tid = mainTid.load(std::memory_order_relaxed);
        if (!tid || !set) return;
        std::lock_guard<decltype(m)> lock(m);
        if (swapped) return;
        if (set(tid, recomp_topology::roles().prime.mask) == 0) { swapped = true; swappedView.store(true, std::memory_order_relaxed); swaps.fetch_add(1, std::memory_order_relaxed); recomp_mw2::enterW(); }
        else { failures.fetch_add(1, std::memory_order_relaxed); return; }
        if (boostNice.load(std::memory_order_relaxed) && !extrasEnabled.load(std::memory_order_relaxed) && setNice && getNice) { // main-sched
            int current = 0;
            if (getNice(tid, &current) == 0 && setNice(tid, boostTo) == 0) { boosted = true; restoreNice = current; boosts.fetch_add(1, std::memory_order_relaxed); }
            else boostFailures.fetch_add(1, std::memory_order_relaxed);
        }
        if (extrasEnabled.load(std::memory_order_relaxed)) { // v251
            for (unsigned i = 0; i < extraTid.size(); ++i)
                if (const int x = extraTid[i].load(std::memory_order_relaxed)) (void)set(x, extraMask[i].load(std::memory_order_relaxed) | recomp_topology::roles().prime.mask);
            extrasSwapped = true; extraSwaps.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // byMain: the caller is main (it moves itself); otherwise the render thread moves main.
    void release(bool byMain) noexcept {
        recomp_mw2::leaveW(byMain); // main-waits-2 (render thread only; returns at once for main or outside a window)
        if (!pxGate(*this, byMain)) return; // prime-exchange; with the px switches absent exactly: swappedView.load(relaxed)
        std::lock_guard<decltype(m)> lock(m);
        if (!swapped) return;
        const int tid = byMain ? 0 : mainTid.load(std::memory_order_relaxed);
        if (set && (releaseRc = set(tid, mainMask.load(std::memory_order_relaxed))) == 0) {
            (byMain ? releasesByMain : releasesByRender).fetch_add(1, std::memory_order_relaxed);
        } else failures.fetch_add(1, std::memory_order_relaxed); // (main keeps {7} until the ranking's next mask: counted)
        if (extrasSwapped) { // v251
            for (unsigned i = 0; i < extraTid.size(); ++i)
                if (const int x = extraTid[i].load(std::memory_order_relaxed)) (void)set(x, extraMask[i].load(std::memory_order_relaxed));
            extrasSwapped = false;
        }
        if (boosted) { // main-sched: back to the nice main had / the placement thread asked for meanwhile
#if !defined(RECOMP_MS_MUTANT_NO_RESTORE) // test mutant: main keeps the boost after the release
            const int t = mainTid.load(std::memory_order_relaxed);
            if (setNice && t && setNice(t, restoreNice) == 0) unboosts.fetch_add(1, std::memory_order_relaxed);
            else unboostFailures.fetch_add(1, std::memory_order_relaxed);
#endif
            boosted = false;
        }
        swapped = false; swappedView.store(false, std::memory_order_relaxed);
        pxAfterRelease(*this, byMain); // prime-exchange: a lent render thread goes home to {7} now that main has left it. Off: one load.
    }
    // main-sched: the placement thread's (re-)application of main's nice (v238 / v246, every 2 s). While main is boosted the
    // value becomes the release's restore value instead (deferred); otherwise it is applied now, exactly as before.
    int applyMainNice(int tid, int nice) noexcept {
        std::lock_guard<decltype(m)> lock(m);
#if !defined(RECOMP_MS_MUTANT_NO_DEFER) // test mutant: the placement thread un-boosts main in the middle of its cpu7 window
        if (boosted && tid == mainTid.load(std::memory_order_relaxed)) { restoreNice = nice; deferred.fetch_add(1, std::memory_order_relaxed); return 0; }
#endif
        return setNice ? setNice(tid, nice) : -1;
    }
};
inline PrimeSwap& state() noexcept { static PrimeSwap s; return s; }

// ---------------------------------------------------------------------------------------------------------------------------
// prime-exchange: the protocol steps (all noinline + noexcept). x = the px state; everything marked "under m" holds PrimeSwap::m.
// Masks: main {7} <=> swapped (as v458; a failed restore excepted, as v458); the render thread {3-6} <=> x.lent.
//   lend           main, pxGate at its first wake of the render thread after a swap: set(render, {3-6}); main keeps {7}.   (1,0)->(1,1)
//   anchor         render thread, pxGate when that wait completes: deadline = now + d (d = the controller's value, which
//                  already contains the slack sample of the wait that just ended).
//   exchange back  render thread, at a check point past the deadline (pxRenderPoll in the executor's slow checkpoint, pxGate at
//                  its later wait completions): pxGate returns true -> v458's release(false) moves main to its ranking mask,
//                  restores nice / extras -> pxAfterRelease moves the render thread to {7}.                              (1,1)->(0,0)
//   rejoin         render thread, pxPark when its present came while it was still lent: it returns to {7}, main keeps cpu7 for
//                  the kick wait (v458's swap state), W is entered.                                                      (1,1)->(1,0)
// ---------------------------------------------------------------------------------------------------------------------------
// The render thread goes home (under m; main is off {7}, or stays there only as v458's swap state at a rejoin).
inline void pxHomeLocked(PrimeSwap& s, recomp_px::State& x, bool timed) noexcept {
    if (!s.set || s.set(x.renderTid.load(std::memory_order_relaxed), recomp_topology::roles().prime.mask) != 0)
        x.homeFailures.fetch_add(1, std::memory_order_relaxed); // (cpu7 paused: the placement tick re-pins / falls back, as for any owner)
    x.lent = false;
    x.lentView.store(false, std::memory_order_relaxed);
    x.stale.store(false, std::memory_order_relaxed);
    x.backs.fetch_add(1, std::memory_order_relaxed);
    if (!timed) { x.backsByMain.fetch_add(1, std::memory_order_relaxed); return; } // (main: no clock read on its path)
    const std::uint64_t now = x.clock(), dl = x.deadlineUs.load(std::memory_order_relaxed);
    if (dl > 1 && now >= x.anchorUs) x.holdSumUs.fetch_add(now - x.anchorUs, std::memory_order_relaxed); // (anchored holds with d > 0)
    if (dl > 1 && now >= dl) {
        const std::uint64_t late = now - dl;
        x.lateSumUs.fetch_add(late, std::memory_order_relaxed);
        if (late > x.lateMaxUs.load(std::memory_order_relaxed)) x.lateMaxUs.store(unsigned(std::min<std::uint64_t>(late, ~0u)), std::memory_order_relaxed);
    }
}
// Render thread past its deadline (or px switched off, or the backstop): exchange back through the release path; in the
// failed-restore state (lent, not swapped) retry main's restore first.
[[gnu::noinline]] inline void pxRenderCheck(PrimeSwap& s) noexcept {
    auto& x = recomp_px::state();
    const std::uint64_t dl = x.deadlineUs.load(std::memory_order_relaxed);
    if (dl > 1 && x.mode.load(std::memory_order_relaxed) && !x.stale.load(std::memory_order_relaxed) &&
        s.enabled.load(std::memory_order_relaxed) && x.clock() < dl) return; // the hold continues (no lock taken)
    if (s.swappedView.load(std::memory_order_relaxed)) { s.release(false); return; }
    std::lock_guard<decltype(s.m)> lock(s.m);
    if (!x.lent || s.swapped) return;
    const int rc = s.set ? s.set(s.mainTid.load(std::memory_order_relaxed), s.mainMask.load(std::memory_order_relaxed)) : -1;
    if (rc != 0) {
        x.backFailures.fetch_add(1, std::memory_order_relaxed);
        x.deadlineUs.store(x.clock() + recomp_px::RetryUs, std::memory_order_relaxed);
        return;
    }
    pxHomeLocked(s, x, true);
}
// Render thread: its present came while it was still lent (its frame was shorter than d, or the kick found it mid-frame).
[[gnu::noinline]] inline void pxParkLent(PrimeSwap& s) noexcept {
    auto& x = recomp_px::state();
    {
        std::lock_guard<decltype(s.m)> lock(s.m);
        if (!x.lent) return;
        x.shortFrame();
        if (s.swapped && s.enabled.load(std::memory_order_relaxed)) { // rejoin: v458's swap state for this wait
            pxHomeLocked(s, x, true);
            x.rejoins.fetch_add(1, std::memory_order_relaxed);
            recomp_mw2::enterW();                                      // (as beforePark's swap: under m, on the render thread)
            return;
        }
        x.deadlineUs.store(1, std::memory_order_relaxed);              // v245 switched off meanwhile, or main's restore pending
    }
    pxRenderCheck(s);
}
// Render thread, top of beforePark (every not-ready wait of the render thread; `armed` = the first one after a present).
[[gnu::noinline]] inline void pxPark(PrimeSwap& s) noexcept {
    auto& x = recomp_px::state();
    const unsigned md = x.mode.load(std::memory_order_relaxed);
    const bool lent = x.lentView.load(std::memory_order_relaxed);
    if (!md && !lent) { // px off: nothing
        if (x.waitOpen.load(std::memory_order_relaxed)) x.waitOpen.store(false, std::memory_order_relaxed); // (once, after a switch-off)
        return;
    }
    if (!s.armed.load(std::memory_order_relaxed)) return;              // (only this thread sets and consumes `armed`)
    if (lent) pxParkLent(s);
    if (md) x.armedPark(md);                                           // the wait for main's kick begins: the slack clock
}
// PrimeSwap::release, in place of v458's `swappedView.load(relaxed)`: 1 (true) = run v458's release now, 0 (false) = return.
// (`unsigned`, not `bool`: see the declaration - release's profile shape depends on it.)
[[gnu::noinline]] inline unsigned pxGate(PrimeSwap& s, bool byMain) noexcept {
    auto& x = recomp_px::state();
    const bool sw = s.swappedView.load(std::memory_order_relaxed);
    const unsigned md = x.mode.load(std::memory_order_relaxed);
    if (!md && !x.lentView.load(std::memory_order_relaxed)) return sw; // px off: v458's test and nothing else
    if (!byMain) x.waitDone(md);                                       // render thread: a wait completed (slack sample, controller)
    if (!sw) return false;
    // Main's later wakes of the render thread inside a hold (its command stream, ~1.4 per frame): it keeps cpu7, and takes no
    // lock for that (v458 returns here too: by then its swappedView is false). A hold the placement thread marked stale goes on.
    if (byMain && x.lentView.load(std::memory_order_relaxed) && !x.stale.load(std::memory_order_relaxed)) return false;
#if defined(RECOMP_PX_MUTANT_NO_LOCK) // test mutant: the exchange decided and made without the protocol lock
    struct { void unlock() {} } lock;
#else
    std::unique_lock<decltype(s.m)> lock(s.m);
#endif
    if (!s.swapped) return false;
    if (x.lent) { // the hold (main {7}, render {3-6})
        if (byMain) return x.stale.load(std::memory_order_relaxed);    // (only a stale hold gets here: main ends it, see pxPlacementPin)
        const std::uint64_t now = x.clock();
        std::uint64_t dl = x.deadlineUs.load(std::memory_order_relaxed);
        if (!dl) { // anchor: this is the completion of the wait main's lend woke
            const unsigned d = x.planUs.load(std::memory_order_relaxed);
            dl = d ? now + d : 1;
            x.deadlineUs.store(dl, std::memory_order_relaxed);
            x.anchorUs = now;
            if (d) { x.anchored.fetch_add(1, std::memory_order_relaxed); x.dSumUs.fetch_add(d, std::memory_order_relaxed); }
            else x.spurious.fetch_add(1, std::memory_order_relaxed);   // the newest slack sample took d to 0: back at once
        }
        if (now < dl && md && s.enabled.load(std::memory_order_relaxed) && !x.stale.load(std::memory_order_relaxed)) return false;
#if defined(RECOMP_PX_MUTANT_HOME_FIRST) // test mutant: the render thread takes {7} back before main has left it
        (void)s.set(x.renderTid.load(std::memory_order_relaxed), recomp_topology::roles().prime.mask);
#endif
#if defined(RECOMP_PX_MUTANT_NO_BACK) // test mutant: no exchange-back (the hold never ends from the render thread)
        return false;
#else
        return true; // exchange back: v458's release(false) (main leaves {7}), then pxAfterRelease (the render thread returns)
#endif
    }
    // v458's swap state. Main's first wake of the render thread: lend instead of releasing?
    if (byMain && md && s.set && !s.extrasSwapped && !s.extrasEnabled.load(std::memory_order_relaxed)
#if !defined(RECOMP_PX_MUTANT_NO_ACK) // test mutant: lends before the placement thread pins under the lock
        && x.placementAck
#endif
        ) {
        const int rt = x.renderTid.load(std::memory_order_relaxed);
        // The controller wants an exchange, and THIS loop's kick wait (the residual slack that pays for it) reached the floor:
        // a kick that finds the render thread late or barely waiting never lends. One clock read on main's path (no syscall).
        if (rt && x.planUs.load(std::memory_order_relaxed) >= unsigned(recomp_px::MinLendUs) && x.waitOpen.load(std::memory_order_relaxed) &&
            x.clock() - x.parkUs.load(std::memory_order_relaxed) >= std::uint64_t(recomp_px::FloorUs)) {
            if (s.set(rt, recomp_topology::roles().mid.mask) == 0) {
                x.deadlineUs.store(0, std::memory_order_relaxed);
                ++x.holdSeq;
                x.lent = true;
                x.lentView.store(true, std::memory_order_relaxed);
                x.exchanges.fetch_add(1, std::memory_order_relaxed);
                return false; // main keeps {7}; nothing is released
            }
            x.lendFailures.fetch_add(1, std::memory_order_relaxed);   // (then v458's release, below)
        }
    }
    lock.unlock();
    return true;
}
// PrimeSwap::release, last statement under m: swapped has just become false.
[[gnu::noinline]] inline void pxAfterRelease(PrimeSwap& s, bool byMain) noexcept {
    auto& x = recomp_px::state();
    if (!x.lentView.load(std::memory_order_relaxed)) return; // no hold: v458's release and nothing else
    if (!x.lent) return;
    if (!s.set || s.releaseRc != 0) { // main may still be on {7} (v458 counts it in `failures`): the render thread stays on {3-6}
        x.backFailures.fetch_add(1, std::memory_order_relaxed);
        x.deadlineUs.store(x.clock() + recomp_px::RetryUs, std::memory_order_relaxed);
        return;
    }
    pxHomeLocked(s, x, !byMain);
}
// Every task's slow checkpoint (guest_cooperative_executor.cpp parallelCheckpoint, <= 1024 guest function entries apart): the
// render thread's check point inside its frame. No hold: one relaxed load of a line nothing writes.
[[gnu::noinline]] inline void pxRenderPoll(std::uint32_t token) noexcept {
    if (!recomp_px::state().lentView.load(std::memory_order_relaxed)) return;
    if (token != recomp_px::RenderToken) return;
    pxRenderCheck(state());
}
// Placement thread, every 250 ms tick, for the prime owner `tid` (trace_runner.cpp): -1 = make v458's own unlocked pin call;
// otherwise the pin was handled here under m: 0 = pinned to {7}, or deliberately skipped because the render thread is lent
// (it must not come back to {7} while main holds it), > 0 = the setter's errno (the caller falls back to {3-6} as v458).
// placementAck: main lends only once this thread pins under m, and this thread returns to the unlocked call only after it
// cleared the flag under m with no hold on - so no pin can land inside a hold. Backstop: a hold seen by two ticks in a row
// (>= 250 ms; d is <= 8 ms) is marked stale and ends at the render thread's or main's next step.
[[gnu::noinline]] inline int pxPlacementPin(PrimeSwap& s, int tid) noexcept {
    auto& x = recomp_px::state();
    if (tid != x.renderTid.load(std::memory_order_relaxed)) return -1; // (diag-prime-main: main is the owner; px inert)
    if (!x.placementAck && !x.mode.load(std::memory_order_relaxed)) return -1; // px never on, or off and acknowledged
    std::lock_guard<decltype(s.m)> lock(s.m);
#if !defined(RECOMP_PX_MUTANT_PIN_ANYWAY) // test mutant: the 250 ms re-pin ignores the hold
    if (x.lent) {
        if (x.seenSeq == x.holdSeq) {
            if (++x.staleTicks >= 2 && !x.stale.exchange(true, std::memory_order_relaxed)) x.backstops.fetch_add(1, std::memory_order_relaxed);
        } else { x.seenSeq = x.holdSeq; x.staleTicks = 1; }
        return 0;
    }
#endif
    x.placementAck = x.mode.load(std::memory_order_relaxed) != 0;
    return s.set ? s.set(tid, recomp_topology::roles().prime.mask) : -1;
}
// v407 (switch diag-v407-swap-delay): lend cpu7 only once a render-thread wait has lasted SwapDelay. v404 A/B (fast-v404-swap):
// the swap point - the render thread's first not-ready wait after a present - is a short poll-completed wait (~0.4 ms), so the
// render thread woke with main on its only CPU (2.4 ms/frame of run-queue wait) and then took cpu7 back for the rest of main's
// window. With the delay, short waits never swap; the long wait for main's kick swaps at its first poll re-check past 1 ms.
constexpr long long SwapDelayUs = 1000;
inline bool v407SwapNow(bool renderThread, bool delayOn, long long waitedUs) noexcept {
#if defined(RECOMP_V407_MUTANT_NO_DELAY) // test mutant: swap at the first re-check whatever the wait's age
    return renderThread && delayOn;
#else
    return renderThread && delayOn && waitedUs >= SwapDelayUs;
#endif
}
constexpr std::uint32_t RenderToken = 0x6408e000u, MainToken = 0x63001000u;
} // namespace recomp_v245
