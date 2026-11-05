#pragma once
// main-wait (lane main-wait, attempt v528, 2026-10-11; included at the end of scene_wait_sw.h): four modifiers of the watch in
// which main's Sleep(0) poll for scene items waits (scene_wait_sw.h), and a measurement of how late a watch hands main back.
//
// WHY (Fold runs 30 and 31 of 2026-10-11, v524). The poll costs main 17 % of its cycles. The 500 us watch with the idle wait
// (diag-sw-wfe) takes 18 % of main's cycles away, but the frame was 1.5 ms longer with it; the watches that spin or yield
// save nothing. What is wanted is a wait that costs neither the cycles of the poll nor lateness in handing main back.
//
// THE SWITCHES (files/private-trace/, polled by the renderer at its 60-present window; all absent = v524's code path: the
// one new instruction sequence then is the producers' hook below, a barrier and one load). Each modifies a watch; when none
// of diag-sw-watch / -500 / -wfe stands, a modifier stands for the 500 us watch with the idle wait, as diag-sw-wfe alone does.
//   diag-sw-spin-first     the watch first re-reads the two words with the processor's yield hint between the checks, with
//                          no system call and no idle wait, for N microseconds (the file's text; empty = DefaultSpinUs), so
//                          that the many short waits of a frame are answered within one check.
//   diag-sw-flag-mark      the idle wait ends promptly on a change of EITHER word. One processor holds one exclusive mark,
//                          and a second marked read moves it, so the two words (far apart in guest memory) cannot both be
//                          marked. The count stays marked as before; for the flag (and for the count as well) main
//                          registers as idle and the producer's hook sends an event (SEV) right after its store.
//   diag-sw-producer-wake  after N microseconds of a watch (the file's text; empty = DefaultSleepAfterUs) main sleeps in the
//                          kernel on a host word, in slices of SleepSliceNs up to the watch's budget, and the producer's
//                          hook wakes it right after its store. The processor is then free (off the prime core: for the
//                          producers themselves) and the kernel sees it idle.
//   diag-sw-no-yield       a watch makes the host yield its Sleep(0) stands for once, not at every check, when main does
//                          not hold the prime core (v524 calls sched_yield at every check there and never idles).
//   diag-sw-late           measurement only: the hooks stamp the first store after a watch began and main records how long
//                          after it the watch returned (RENDER_GENERIC_SWAIT late_us_p50 / late_us_p99). With no modifier
//                          the watch passes its time exactly as v524's does (same yields, idle waits and clock reads).
//
// THE PRODUCERS' HOOK (recipe pass sw-producer-hooks, chunks 301 and 34; recomp_sw_producer_stored in
// guest_import_registry.cpp). The scene workers advance the count in sub_83483EF8 with an ordered compare-and-swap and
// sub_825315B8 clears the flag with a plain store. The pass adds one call AFTER each of the two stores, never before, as
// the last statement of each function (after the count's publishing loop and the epilogue's loads; two stores after the
// flag's): the order of the guest's stores and every guest-visible value stay as they are (the hook reads and writes host
// words only), no block is added (both functions keep their profile records) and no generated code runs after the call.
// sub_83483EF8 also returns that way with nothing to publish: the hook is then called without a store, which is
// harmless (a wake or an event too many). With nobody registered the hook is a barrier and one relaxed load.
//
// NO LOST WAKE-UP (the classic store / check-sleeping against register-sleeping / re-check race). Main: M1 store
// `waiting` (registered), full barrier, then for each sleep L0 load the wake word, M2 read the two guest words, sleep only
// if they still say wait and only while the wake word equals what L0 read. Producer: S1 the guest store, full barrier, S2
// load `waiting`, and when registered B advance the wake word, W wake. Let S1 be a store after which the guest would
// proceed, and suppose main went to sleep having read the old words at M2. (1) S2 cannot have read "not registered": that
// needs M1 not yet visible at S2, so main's barrier, and with it M2, came after S2, hence after the producer's barrier,
// which made S1 visible to all: M2 would have read the new word. So S2 read "registered" and B and W follow. (2) B cannot
// be in the value L0 read: B is after the producer's barrier, so S1 was visible before B, and M2 is after L0. So the wake
// word differs from L0's value from B on: the kernel's compare (made under its queue lock) refuses the sleep if B came
// first, and otherwise main is queued before W and W wakes it. The same holds for the event form: SEV after the barrier
// sets main's event register, which a later WFE consumes at once (an exception return sets it too, so a preempted main
// does not sleep through). Both barriers are needed: without either one, both loads can run ahead of both stores. Host
// model: the lane's tests/model_test.cpp runs these very templates under every interleaving with store buffers.
// Besides: every wait is bounded (the sleep by its slice and the budget, the idle wait by the kernel's event stream), so
// a store by a path that has no hook, or an event a processor lost, costs lateness of at most 100 us, never a hang.
//
// EXACTNESS. As scene_wait_sw.h: Sleep(0) returns the same status after the same handler; nothing is written to guest
// memory (the marked read is a read); a watch holds main only while the guest's next action would be another Sleep(0) with
// no guest-visible step between; it ends when the executor needs the thread (quiet(), tested at every check: after every
// spin step, every event and every sleep slice) and at the budget.
#include "scene_wait_sw.h" // (this header is included at its end; either may come first)
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <sched.h>
#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__) // host tests on the Mac: the kernel's compare-and-wait on an address, as libc++ uses it
extern "C" int __ulock_wait(std::uint32_t operation, void* address, std::uint64_t value, std::uint32_t timeoutUs);
extern "C" int __ulock_wake(std::uint32_t operation, void* address, std::uint64_t wakeValue);
#endif

namespace recomp_sw {

// MainWait::mode (set by the renderer's window poll from the switch files).
constexpr unsigned ModeFlagMark = 1u, ModeSpinFirst = 2u, ModeProducerWake = 4u, ModeNoYield = 8u, ModeMeasure = 16u;
constexpr unsigned ModeModifiers = ModeFlagMark | ModeSpinFirst | ModeProducerWake | ModeNoYield;
// The spin: by Fold runs 30 / 31 (RENDER_DIAG_SW) a frame has about 140 watches of 42 to 44 us in the mean; a third of the
// 50 us watches expire and about 1 % of the 500 us ones: a body near an exponential of 35 us and a tail of a few waits of
// 0.5 to 10 ms a frame. A spin of S us answers 1 - exp(-S / 35) of the body at once and burns min(wait, S) of every wait:
// 20 us answers 44 % and keeps about two thirds of what the idle wait saves; 50 us answers 76 % and keeps 40 %.
constexpr unsigned DefaultSpinUs = 20u, DefaultSleepAfterUs = 150u, MaxFileUs = 500u;
constexpr std::uint64_t SleepSliceNs = 100'000; // one sleep; the idle wait's own bound is the kernel's 100 us event stream
// MainWait::waiting bits: what main asks of a producer's hook.
constexpr std::uint32_t WaitStamp = 1u, WaitIdle = 2u, WaitAsleep = 4u;
constexpr unsigned LateBuckets = 64u; // quarter octaves from 0.25 us to 16 ms
constexpr unsigned LastedBuckets = 6u; // a changed watch lasted under 10, 20, 50, 100, 200 us, or longer: what a spin of that length answers
inline unsigned lastedBucket(std::uint64_t ns) noexcept {
    return ns < 10'000 ? 0u : ns < 20'000 ? 1u : ns < 50'000 ? 2u : ns < 100'000 ? 3u : ns < 200'000 ? 4u : 5u;
}

// The number in a switch file's text: decimal microseconds, at most MaxFileUs; anything else (empty, a newline only, letters)
// gives the default. Leading blanks are skipped; the number ends at the first other character.
inline unsigned parseFileUs(const char* text, std::size_t length, unsigned fallback) noexcept {
    std::size_t i = 0;
    while (i < length && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) ++i;
    if (i == length || text[i] < '0' || text[i] > '9') return fallback;
    unsigned value = 0;
    for (; i < length && text[i] >= '0' && text[i] <= '9'; ++i) {
#if defined(RECOMP_SWM_MUTANT_PARSE_NO_CLAMP) // host-test mutant: a large number in the file is taken as it stands
        value = value * 10u + unsigned(text[i] - '0');
#else
        value = value >= MaxFileUs ? MaxFileUs : value * 10u + unsigned(text[i] - '0');
#endif
    }
#if defined(RECOMP_SWM_MUTANT_PARSE_NO_CLAMP)
    return value;
#else
    return value > MaxFileUs ? MaxFileUs : value;
#endif
}

struct MainWait {
    // configuration (the renderer's window poll writes, main reads)
    std::atomic<unsigned> mode{0};
    std::atomic<unsigned> spinUs{DefaultSpinUs};
    std::atomic<unsigned> sleepAfterUs{DefaultSleepAfterUs};
    // what the producers' hooks read at every store: on a line of its own, main writes it twice per registered watch
    alignas(64) std::atomic<std::uint32_t> waiting{0};     // WaitStamp | WaitIdle | WaitAsleep while main is in a registered watch
    std::atomic<std::uint32_t> countAddress{0};            // the count word (guest address) of that watch
    alignas(64) std::atomic<std::uint32_t> wakeWord{0};    // main sleeps while this is what it last read; a waking producer advances it
    std::atomic<std::uint64_t> firstStoreTicks{0};         // measuring: 0 = armed; else the ticks of the first store since
    // main's counters (one writer)
    alignas(64) std::atomic<unsigned long long> watches{0};
    std::atomic<unsigned long long> changed{0}, unquiet{0}, expired{0}; // how watches ended
    std::atomic<unsigned long long> spunOnly{0};     // watches that ended inside the spin
    std::atomic<unsigned long long> idled{0};        // watches with at least one idle wait
    std::atomic<unsigned long long> slept{0};        // watches with at least one sleep in the kernel
    std::atomic<unsigned long long> idleWaits{0};    // waits for an event
    std::atomic<unsigned long long> sleeps{0};       // sleeps in the kernel
    std::atomic<unsigned long long> sleepsWoken{0};  // of those, ended by a producer (woken, or the word had moved already)
    std::atomic<unsigned long long> yields{0};       // host yields made inside watches
    std::atomic<unsigned long long> byCount{0}, byFlag{0}; // changed watches: an item was published / the flag was clear
    std::atomic<unsigned long long> watchNs{0}, spinNs{0}, primeNs{0}; // time in watches; of it in the spin; in watches begun on the prime core
    std::atomic<unsigned long long> lasted[LastedBuckets]{}; // changed watches by their length (lastedBucket)
    std::atomic<unsigned long long> lateCount{0};
    std::atomic<unsigned long long> late[LateBuckets]{};
    // the producers' counters (several writers)
    alignas(64) std::atomic<unsigned long long> hookWakes{0};
    std::atomic<unsigned long long> hookEvents{0}, hookStamps{0};
    std::atomic<unsigned long long> hookOther{0};    // stores reported while main was registered that were to neither of its two words
};
inline MainWait& mainWait() noexcept { static MainWait w; return w; }

// Lateness histogram: bucket b holds [0.25 us * 2^(b/4), 0.25 us * 2^((b+1)/4)); the last bucket takes all that is longer.
inline unsigned lateBucket(std::uint64_t ns) noexcept {
    if (ns < 250) return 0;
    const std::uint64_t q = ns / 250;                       // >= 1
    const unsigned octave = 63u - unsigned(__builtin_clzll(q));
    const std::uint64_t base = std::uint64_t{250} << octave; // the octave's lower edge in ns
    // quarter of the octave: edges at 2^(1/4), 2^(2/4), 2^(3/4) = 1.189, 1.414, 1.682 (x 1024: 1218, 1448, 1722)
    const std::uint64_t scaled = ns * 1024 / base;
    const unsigned quarter = scaled < 1218 ? 0u : scaled < 1448 ? 1u : scaled < 1722 ? 2u : 3u;
    const unsigned bucket = octave * 4u + quarter;
    return bucket < LateBuckets ? bucket : LateBuckets - 1u;
}
inline double lateBucketUs(unsigned bucket) noexcept { // the bucket's geometric middle, in us
    double edge = 0.25;
    for (unsigned i = 0; i < bucket / 4u; ++i) edge *= 2.0;
    static constexpr double Quarter[4] = {1.0905, 1.2968, 1.5422, 1.8340}; // 2^((2k+1)/8)
    return edge * Quarter[bucket % 4u];
}
// The p-quantile (0 < p <= 1) of a histogram of counts; 0 when it is empty.
inline double lateQuantileUs(const unsigned long long* counts, double p) noexcept {
    unsigned long long total = 0;
    for (unsigned b = 0; b < LateBuckets; ++b) total += counts[b];
    if (total == 0) return 0.0;
    const double want = p * double(total);
    unsigned long long seen = 0;
    for (unsigned b = 0; b < LateBuckets; ++b) {
        seen += counts[b];
        if (double(seen) >= want) return lateBucketUs(b);
    }
    return lateBucketUs(LateBuckets - 1u);
}

enum class Slept : unsigned { Woken, TimedOut };

// The host's operations, as the device runs them. The tests' model replaces this type; the registry adds what it alone
// knows (the clock, whether main holds the prime core, the count word's host address).
struct HostOps {
    static std::uint32_t load(const std::atomic<std::uint32_t>& a) noexcept { return a.load(std::memory_order_relaxed); }
    static void store(std::atomic<std::uint32_t>& a, std::uint32_t v) noexcept { a.store(v, std::memory_order_relaxed); }
    static void advance(std::atomic<std::uint32_t>& a) noexcept { a.fetch_add(1, std::memory_order_seq_cst); }
    // Every store this thread made is visible to all before any later load of this thread is made (AArch64: DMB ISH).
    static void fence() noexcept { std::atomic_thread_fence(std::memory_order_seq_cst); }
    static void spinHint() noexcept {
#if defined(__aarch64__)
        asm volatile("yield" ::: "memory");
#elif defined(__x86_64__)
        __builtin_ia32_pause();
#endif
    }
    static void yieldCpu() noexcept { ::sched_yield(); }
    // An event for every processor that waits for one. The store before it must be visible first: DSB, as the kernel's own
    // unlock-and-wake sequence has it (a DMB orders memory accesses among themselves, not a store before SEV).
    static void sendEvent() noexcept {
#if defined(__aarch64__)
        asm volatile("dsb ishst\n\tsev" ::: "memory");
#endif
    }
    // v511's idle wait: mark the count word for exclusive access (LDXR: a read) and wait for an event unless the word no
    // longer holds what the watch last read. A store to the word by another processor clears the mark, which is an event.
    static bool markedIdle(const std::uint8_t* countWord, std::uint32_t lastCount) noexcept {
#if defined(__aarch64__)
        std::uint32_t marked;
        asm volatile("ldxr %w0, [%1]" : "=&r"(marked) : "r"(countWord) : "memory");
        if (__builtin_bswap32(marked) != lastCount) return false;
        asm volatile("wfe" ::: "memory");
        return true;
#else
        (void)countWord; (void)lastCount;
        return false;
#endif
    }
    // Sleep in the kernel while `word` still holds `expected`, for at most `ns`.
    static Slept sleep(std::atomic<std::uint32_t>& word, std::uint32_t expected, std::uint64_t ns) noexcept {
#if defined(__linux__)
        struct timespec ts;
        ts.tv_sec = static_cast<time_t>(ns / 1'000'000'000ull);
        ts.tv_nsec = static_cast<long>(ns % 1'000'000'000ull);
        const long rc = ::syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&word), FUTEX_WAIT_PRIVATE, expected, &ts, nullptr, 0);
        return rc != 0 && errno == ETIMEDOUT ? Slept::TimedOut : Slept::Woken; // woken, the word had moved, or a signal: re-check
#elif defined(__APPLE__)
        const std::uint64_t us = ns / 1000u;
        const int rc = ::__ulock_wait(1u /* UL_COMPARE_AND_WAIT */, &word, expected, us ? static_cast<std::uint32_t>(us) : 1u);
        return rc < 0 && errno == ETIMEDOUT ? Slept::TimedOut : Slept::Woken;
#else
        (void)word; (void)expected; (void)ns;
        ::sched_yield();
        return Slept::TimedOut;
#endif
    }
    static void wake(std::atomic<std::uint32_t>& word) noexcept {
#if defined(__linux__)
        ::syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&word), FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
#elif defined(__APPLE__)
        ::__ulock_wake(1u /* UL_COMPARE_AND_WAIT */, &word, 0);
#else
        (void)word;
#endif
    }
};

// A scene producer's store to guest word `address` is complete (called AFTER the store, never before; see the header).
// ticks() is the clock main's watch uses. With nobody registered: the barrier, one relaxed load, return.
template <class Ops, class Ticks>
inline void producerStored(MainWait& w, Ops& ops, std::uint32_t address, Ticks ticks) noexcept {
#if !defined(RECOMP_SWM_MUTANT_HOOK_NO_FENCE) // host-test mutant: the load below may run ahead of the producer's store
    ops.fence();
#endif
    const std::uint32_t waiting = ops.load(w.waiting);
    if (waiting == 0) return;
    // Is this one of the two words main waits on? Only the stamp and a counter follow the answer. The wake and the event
    // are sent for any store: one for another word costs main one more check, one withheld by a wrong comparison (the
    // count word's address is what main's frame says it is, not something this hook can verify) would cost it a slice.
    const bool watched = address == FlagAddress || address == ops.load(w.countAddress);
    if (!watched) w.hookOther.fetch_add(1, std::memory_order_relaxed);
    if ((waiting & WaitStamp) != 0 && watched) { // measuring: the first store since main armed the stamp
        std::uint64_t armed = 0;
        if (w.firstStoreTicks.compare_exchange_strong(armed, ticks() | 1u, std::memory_order_relaxed))
            w.hookStamps.fetch_add(1, std::memory_order_relaxed);
    }
    if (waiting & WaitIdle) {
#if defined(RECOMP_SWM_MUTANT_FLAG_NO_EVENT) // host-test mutant: a cleared flag sends no event (seen at the next tick, as in v524)
        if (address != FlagAddress)
#endif
        { ops.sendEvent(); w.hookEvents.fetch_add(1, std::memory_order_relaxed); }
    }
    if (waiting & WaitAsleep) {
#if !defined(RECOMP_SWM_MUTANT_NO_ADVANCE) // host-test mutant: the wake leaves the word as main read it (a sleep after the wake is not refused)
        ops.advance(w.wakeWord);
#endif
        ops.wake(w.wakeWord);
        w.hookWakes.fetch_add(1, std::memory_order_relaxed);
    }
}

struct MainConfig {
    unsigned mode = 0;                          // Mode* bits
    bool idleWait = false;                      // the watch's idle wait (diag-sw-wfe)
    std::uint64_t budgetNs = 0;                 // the watch's budget
    std::uint64_t spinNs = 0;                   // ModeSpinFirst: length of the spin
    std::uint64_t sleepAfterNs = 0;             // ModeProducerWake: no sleep before this much of the watch has passed
    std::uint64_t sleepSliceNs = SleepSliceNs;  // one sleep (the tests lengthen it to make a lost wake-up visible)
};
struct MainResult {
    std::uint64_t spentNs = 0, spinNs = 0;
    unsigned checks = 0, idleWaits = 0, sleeps = 0, sleepsWoken = 0, yields = 0;
    bool spunOnly = false, byFlag = false;
    std::uint32_t registered = 0;               // the waiting bits main had set when the watch ended (cleared again by then)
};

// The watch. read32 / quiet / now as scene_wait_sw.h's watch (now() in ns). ops: HostOps's operations plus
//   bool skipYield()                 the old path would skip the host yield now (main holds the prime core, diag-ms-prime-noyield)
//   bool idle(std::uint32_t count)   the marked idle wait for the count the watch last read; false = it did not wait
// Passing of time between two checks, in this order:
//   1. ModeSpinFirst, for cfg.spinNs: the yield hint only.
//   2. ModeProducerWake, from cfg.sleepAfterNs on: a sleep in the kernel of at most one slice and never past the budget.
//   3. main does not hold the prime core: the host yield, at every check as v524 does, or once per watch (ModeNoYield).
//   4. else the idle wait (cfg.idleWait), else the yield hint.
// With ModeFlagMark main registers before its first idle wait, with ModeProducerWake before its first sleep, and the checks
// that decide a wait are made after the registration's barrier. The registration is withdrawn on every way out.
// With no modifier bit (ModeMeasure alone) steps 3 and 4 and the clock reads (every 16th check) are v524's.
template <class Ops, class Read32, class Quiet, class Now>
End watchMain(MainWait& w, Ops& ops, const PollFrame& f, const MainConfig& cfg, Read32 read32, Quiet quiet, Now now,
              MainResult& r) noexcept {
    const std::uint64_t start = now();
    const bool tightClock = (cfg.mode & ModeModifiers) != 0; // v524 reads the clock every 16th check, also around idle waits
    const bool measuring = (cfg.mode & ModeMeasure) != 0;
    End end = End::Expired;
    std::uint32_t registered = 0;
#if defined(RECOMP_SWM_MUTANT_SPIN_WITHOUT_SWITCH) // host-test mutant: every watch begins with the spin
    bool spinning = cfg.spinNs != 0;
#else
    bool spinning = (cfg.mode & ModeSpinFirst) != 0 && cfg.spinNs != 0;
#endif
    bool waited = false, yielded = false;
    bool skip = ops.skipYield();
    if (measuring) { // arm the stamp: the first store from here on is the one this watch waits for
        w.firstStoreTicks.store(0, std::memory_order_relaxed);
        registered = WaitStamp;
        ops.store(w.countAddress, f.countAddress);
        ops.store(w.waiting, registered);
    }
    for (unsigned i = 1;; ++i) {
        // A sleep is refused when the wake word has moved since this load, which precedes the check below.
        const std::uint32_t wakeSeen = (registered & WaitAsleep) ? ops.load(w.wakeWord) : 0u;
        std::uint32_t count = 0, flag = 0;
        if (!read32(f.countAddress, count) || !read32(FlagAddress, flag)) { end = End::Unreadable; break; }
        if (!sleepsAgain(count, flag, f.consumed)) { end = End::Changed; r.byFlag = flag == 0; break; }
        r.checks = i;
#if defined(RECOMP_SWM_MUTANT_QUIET_IGNORED) // host-test mutant: the executor's need for main does not end a spin or a sleep
        if (!spinning && !(registered & WaitAsleep) && !quiet()) { end = End::Unquiet; break; }
#else
        if (!quiet()) { end = End::Unquiet; break; }
#endif
        std::uint64_t elapsed = 0;
        if (spinning || (i & 15u) == 0 || (tightClock && waited) || (cfg.mode & ModeProducerWake)) {
            elapsed = now() - start;
#if defined(RECOMP_SWM_MUTANT_BUDGET_IGNORED) // host-test mutant: only a watch that neither idles nor sleeps ever expires
            if (elapsed >= cfg.budgetNs && !waited) break;
#else
            if (elapsed >= cfg.budgetNs) break;
#endif
        }
        if ((i & 15u) == 0) skip = ops.skipYield();
        if (spinning) { // 1. the spin
#if defined(RECOMP_SWM_MUTANT_SPIN_NEVER_ENDS) // host-test mutant: the spin lasts as long as the watch
            if (elapsed < cfg.budgetNs) { ops.spinHint(); continue; }
#else
            if (elapsed < cfg.spinNs) { ops.spinHint(); continue; }
#endif
            spinning = false;
            r.spinNs = elapsed;
        }
#if defined(RECOMP_SWM_MUTANT_SLEEP_WITHOUT_SWITCH) // host-test mutant: main sleeps in the kernel whatever the switch says
        elapsed = now() - start;
        const bool sleepNow = elapsed >= cfg.sleepAfterNs;
#else
        const bool sleepNow = (cfg.mode & ModeProducerWake) != 0 && elapsed >= cfg.sleepAfterNs;
#endif
        if (sleepNow) { // 2. the sleep
            if (!(registered & WaitAsleep)) { // register, then check again from the top: after the barrier
                registered |= WaitAsleep;
                ops.store(w.countAddress, f.countAddress);
                ops.store(w.waiting, registered);
#if !defined(RECOMP_SWM_MUTANT_MAIN_NO_FENCE) // host-test mutant: the checks below may run ahead of the registration
                ops.fence();
#endif
#if !defined(RECOMP_SWM_MUTANT_NO_RECHECK) // host-test mutant: main sleeps on the words it read before it registered
                continue;
#endif
            }
            const std::uint64_t left = elapsed < cfg.budgetNs ? cfg.budgetNs - elapsed : 1u; // (the budget test above passed on this clock read)
            const std::uint64_t slice = left < cfg.sleepSliceNs ? left : cfg.sleepSliceNs;
#if defined(RECOMP_SWM_MUTANT_STALE_WAKE_WORD) // host-test mutant: the wake word is read after the check, just before the sleep
            const Slept how = ops.sleep(w.wakeWord, ops.load(w.wakeWord), slice);
            (void)wakeSeen;
#else
            const Slept how = ops.sleep(w.wakeWord, wakeSeen, slice);
#endif
            ++r.sleeps;
            if (how == Slept::Woken) ++r.sleepsWoken;
            waited = true;
            continue;
        }
        if (!skip) { // 3. off the prime core: the host yield
#if defined(RECOMP_SWM_MUTANT_YIELD_EVERY_CHECK) // host-test mutant: diag-sw-no-yield changes nothing
            { ops.yieldCpu(); ++r.yields; continue; }
#else
            if (!(cfg.mode & ModeNoYield)) { ops.yieldCpu(); ++r.yields; continue; }
#endif
#if defined(RECOMP_SWM_MUTANT_NO_YIELD_AT_ALL) // host-test mutant: under diag-sw-no-yield a watch never yields the processor
            yielded = true;
#endif
            if (!yielded) { ops.yieldCpu(); ++r.yields; yielded = true; continue; }
        }
        if (cfg.idleWait) { // 4. the idle wait
#if !defined(RECOMP_SWM_MUTANT_IDLE_NO_REGISTER) // host-test mutant: diag-sw-flag-mark registers nothing (no event for the flag)
            if ((cfg.mode & ModeFlagMark) != 0 && !(registered & WaitIdle)) { // register, then check again: after the barrier
                registered |= WaitIdle;
                ops.store(w.countAddress, f.countAddress);
                ops.store(w.waiting, registered);
                ops.fence();
#if !defined(RECOMP_SWM_MUTANT_IDLE_NO_RECHECK) // host-test mutant: main waits for an event on the words it read before it registered
                continue;
#endif
            }
#endif
            if (ops.idle(count)) { ++r.idleWaits; waited = true; }
            continue;
        }
        ops.spinHint();
    }
#if !defined(RECOMP_SWM_MUTANT_WAITING_NOT_CLEARED) // host-test mutant: the registration outlives the watch (producers keep waking)
    if (registered != 0) ops.store(w.waiting, 0u);
#endif
    r.registered = registered;
    r.spentNs = now() - start;
    r.spunOnly = spinning;
    return end;
}

// Main's bookkeeping after a watch (one writer). nowTicks / startTicks / ticksToNs: the clock the hooks stamp with.
template <class TicksToNs>
inline void recordWatch(MainWait& w, End end, const MainResult& r, bool onPrime, bool measuring, std::uint64_t startTicks,
                        std::uint64_t nowTicks, TicksToNs ticksToNs) noexcept {
    const auto add = [](std::atomic<unsigned long long>& c, unsigned long long n) noexcept { c.fetch_add(n, std::memory_order_relaxed); };
    add(w.watches, 1);
    add(end == End::Changed ? w.changed : end == End::Expired ? w.expired : w.unquiet, 1);
    if (end == End::Changed) { add(r.byFlag ? w.byFlag : w.byCount, 1); add(w.lasted[lastedBucket(r.spentNs)], 1); }
    if (r.spunOnly) add(w.spunOnly, 1);
    if (r.idleWaits) { add(w.idled, 1); add(w.idleWaits, r.idleWaits); }
    if (r.sleeps) { add(w.slept, 1); add(w.sleeps, r.sleeps); add(w.sleepsWoken, r.sleepsWoken); }
    if (r.yields) add(w.yields, r.yields);
    add(w.watchNs, r.spentNs);
    add(w.spinNs, r.spunOnly ? r.spentNs : r.spinNs);
    if (onPrime) add(w.primeNs, r.spentNs);
    if (measuring && end == End::Changed) { // how long after the first store since the watch began did the watch return
        const std::uint64_t stamp = w.firstStoreTicks.load(std::memory_order_relaxed);
        if (stamp != 0 && stamp >= startTicks && nowTicks >= stamp) {
            add(w.late[lateBucket(ticksToNs(nowTicks - stamp))], 1);
            add(w.lateCount, 1);
        }
    }
}

// The renderer's window poll: mode bits from the switch files' presence.
inline unsigned switchMode(bool flagMark, bool spinFirst, bool producerWake, bool noYield, bool late) noexcept {
    return (flagMark ? ModeFlagMark : 0u) | (spinFirst ? ModeSpinFirst : 0u) | (producerWake ? ModeProducerWake : 0u) |
           (noYield ? ModeNoYield : 0u) | (late ? ModeMeasure : 0u);
}
// The watch word (recomp_sw_watch_us) for a window: the base files' value when one of them stands; else a modifier
// stands for the 500 us watch with the idle wait; the measuring switch alone starts no watch.
inline unsigned switchWatchWord(unsigned baseWord, unsigned mode) noexcept {
    if ((baseWord & ~WfeBit) != 0) return baseWord;
    return (mode & ModeModifiers) != 0 ? (LongBudgetUs | WfeBit) : 0u;
}

} // namespace recomp_sw
