#pragma once
// scene-wait (lane engine/scene-wait, attempt v482; ARCHITECTURE-PLAN C1 item 2, C6 experiment 4): main's Sleep(0) poll for
// scene items waits on the producers' own stores instead of re-entering the import path between checks.
//
// WHAT MAIN WAITS FOR (v474 corpus, read only). sub_824FD400 calls sub_8239DF60(0x8395E700). Its loop (0x8239E030) reads
// the published count at r31 = r28+0x1B584 (lwz; lwsync) and handles item r30 (sub_8239E320) while r30 < count (signed);
// with nothing new it reads the busy flag 0x83CA8254 (lis r29,-31797; lwz -32172(r29)) and, while the flag is set, calls
// Sleep(0) at 0x8239E05C (sub_82548DE0 -> b sub_824F1418 -> b SleepEx sub_824F1388 -> KeDelayExecutionThread(1, 0, &0)),
// then reloads count and flag: flag == 0 -> back to the top (one more lwsync pass, then exit); r30 >= count -> Sleep(0)
// again; else back to the top. The count is advanced by the scene workers in sub_83483EF8 (chunk 301): reserve a slot
// (lwarx/stwcx. on obj+0x1C528), write {data, size} to obj+0x1B588+8*slot, lwsync, then an ordered compare-and-swap
// count: slot -> slot+1 (lwarx/stwcx. retried until count == slot). The flag is set by sub_8227C800 and cleared by
// sub_825315B8. Both are plain guest stores / guest reservations: NO import, event or hook reports them, so the runtime
// cannot be told; its callers in chunks 302 / 15 call __imp__sub_83483EF8 / __imp__sub_825315B8 directly
// (direct_calls.py), so a C++ hook on either would not even run without re-emitting those chunks.
// THE OBSERVATION POINT used here: the producers' stores themselves. The Sleep(0) fast path recognises this one call site
// from SleepEx's frame (the guest's own saved words: back chain, LR = 0x8239E060, r30 = consumed index, r31 = &count) and
// re-reads the same two guest words the loop reads after Sleep(0) returns.
//
// THE WAIT (switch files/private-trace/diag-sw-watch, budget 50 us; diag-sw-watch-500, 500 us; polled by the renderer at
// its 60-present window). After the unchanged Sleep(0) fast path (handler, counters): when the loop will proceed already
// (an item published or the flag cleared meanwhile) the call returns at once, without the host yield step; when the guest
// would call Sleep(0) again (flag != 0 && consumed >= count), the call does not return yet: it re-reads count and flag every iteration and
// returns as soon as that predicate is false (the item is published or the flag cleared), when the executor needs the
// thread (attention word set, a domain-mate queued, children to arm: the conditions under which the next Sleep(0) or entry
// checkpoint would act), or when the budget is spent (fall-back: the guest re-checks and calls Sleep(0) again, today's
// path). Between checks the thread passes time as the old path does: sched_yield() where the old Sleep(0) yields, the CPU's
// yield hint where it skips the yield (diag-ms-prime-noyield while main holds cpu7).
// Exactness: Sleep(0) still returns STATUS_SUCCESS after the same handler; nothing is written to guest memory; the watch
// returns only while the guest's next action would have been another Sleep(0) with no guest-visible step between (two
// loads and compares), so main's sequence of guest-visible effects is the old one; it returns no later than one watch
// iteration after the guest-visible change (old: up to one whole Sleep(0) round trip), and never later than the budget.
#include <atomic>
#include <cstdint>

extern "C" std::atomic<unsigned> recomp_sw_watch_us; // strong in guest_import_registry.cpp (0 = off: v474's path)

namespace recomp_sw {

constexpr std::uint32_t PollReturnLr = 0x8239E060u; // return address of the loop's Sleep(0) call (bl 0x82548de0 at 0x8239E05C)
constexpr std::uint32_t SleepExFrame = 112u;        // sub_824F1388: stwu r1,-112(r1); LR at +104, r30 at +88, r31 at +96
constexpr std::uint32_t FlagAddress = 0x83CA8254u;  // lis r29,-31797 ; lwz r11,-32172(r29)
constexpr unsigned BudgetUs = 50u, LongBudgetUs = 500u;

// idle-wait (attempt v511, 2026-10-10; switch files/private-trace/diag-sw-wfe, with either watch switch or alone = with the
// 500 us budget): between two checks of a watch main does not spin. It marks the count word for exclusive access (LDXR: no
// write) and, when the word still holds the value the watch last read, waits for an event (WFE). A store to that word by a
// scene worker clears the mark, which is an event: main wakes at once. The flag word is not marked: its clearing is seen at
// the next event stream tick (the kernel's 10 kHz stream) or the next interrupt. Scheduling only: the watch reads what it
// read before and ends on the same conditions. The top bit of recomp_sw_watch_us carries the switch to the registry.
constexpr unsigned WfeBit = 0x80000000u;
constexpr std::uint64_t WfeSlowNs = 200'000;
// The renderer's window poll: the budget the switch files ask for (0 = off; the longer one wins).
inline unsigned switchBudgetUs(bool watch, bool watch500) noexcept { return watch500 ? LongBudgetUs : watch ? BudgetUs : 0u; }

struct PollFrame {
    std::uint32_t countAddress; // the loop's r31 (its object + 0x1B584)
    std::int32_t consumed;      // the loop's r30 (items handled so far)
};

// Is the Sleep(0) whose SleepEx frame starts at guest r1 = sp the scene-item poll's? read32(address, value) reads one
// big-endian guest word and returns false when the address is not mapped. Reads only; the words are the guest's own saves.
template <class Read32>
bool identify(std::uint32_t sp, Read32 read32, PollFrame& out) noexcept {
    if (sp == 0 || (sp & 7u) != 0 || sp > 0xFFFFFFFFu - SleepExFrame) return false;
    const std::uint32_t caller = sp + SleepExFrame;
    std::uint32_t chain = 0, lr = 0, r30 = 0, r31 = 0;
#if !defined(RECOMP_SW_MUTANT_NO_IDENTITY) // host-test mutant: every zero-delay Sleep(0) is taken for the scene poll
    if (!read32(sp, chain) || chain != caller) return false;         // the back chain stwu stored
    if (!read32(caller - 8u, lr) || lr != PollReturnLr) return false; // stw r12(LR),-8(r1) in SleepEx's prologue
#else
    (void)chain; (void)lr;
#endif
    // std r30,-24(r1) / std r31,-16(r1): 64-bit big-endian saves; the loop uses the low words (cmpw / lwz 0(r31)).
    if (!read32(caller - 24u + 4u, r30) || !read32(caller - 16u + 4u, r31) || (r31 & 3u) != 0) return false;
    out = PollFrame{r31, static_cast<std::int32_t>(r30)};
    return true;
}

// The loop's decision after Sleep(0) returns (it loads count, then the flag): true = it calls Sleep(0) again at once.
inline bool sleepsAgain(std::uint32_t count, std::uint32_t flag, std::int32_t consumed) noexcept {
    return flag != 0 && consumed >= static_cast<std::int32_t>(count);
}

enum class End : unsigned { Changed, Unquiet, Expired, Unreadable };

// The watch. read32 as above; quiet() = the executor would not act at the next Sleep(0) / checkpoint; now() = monotonic
// ticks; pause(i) passes the time between two checks (i = iteration). The budget clock is read every 16 iterations.
template <class Read32, class Quiet, class Now, class Pause>
End watch(const PollFrame& f, std::uint64_t budgetTicks, Read32 read32, Quiet quiet, Now now, Pause pause,
          std::uint64_t* spentTicks) noexcept {
    const std::uint64_t start = now();
    End end = End::Expired;
#if defined(RECOMP_SW_MUTANT_NO_REREAD) // host-test mutant: the producers' stores are no longer observed after entry
    std::uint32_t cached[2] = {0, 0};
    bool haveCached = false;
#endif
    for (unsigned i = 1;; ++i) {
        std::uint32_t count = 0, flag = 0;
#if defined(RECOMP_SW_MUTANT_NO_REREAD)
        if (!haveCached) {
            if (!read32(f.countAddress, cached[0]) || !read32(FlagAddress, cached[1])) { end = End::Unreadable; break; }
            haveCached = true;
        }
        count = cached[0]; flag = cached[1];
#elif defined(RECOMP_SW_MUTANT_FLAG_BLIND) // host-test mutant: the flag is never re-read (the count's word stands in for it)
        if (!read32(f.countAddress, count) || !read32(f.countAddress, flag)) { end = End::Unreadable; break; }
#else
        if (!read32(f.countAddress, count) || !read32(FlagAddress, flag)) { end = End::Unreadable; break; }
#endif
        if (!sleepsAgain(count, flag, f.consumed)) { end = End::Changed; break; }
#if !defined(RECOMP_SW_MUTANT_NO_QUIET) // host-test mutant: a stop / queued domain-mate does not end the watch
        if (!quiet()) { end = End::Unquiet; break; }
#endif
#if !defined(RECOMP_SW_MUTANT_NO_TIMEOUT) // host-test mutant: no fall-back to the guest's own re-check
#if defined(RECOMP_SW_MUTANT_EARLY_EXPIRE) // host-test mutant: every watch ends at its 16th iteration (budget test inverted)
        if ((i & 15u) == 0 && now() - start < budgetTicks) break;
#else
        if ((i & 15u) == 0 && now() - start >= budgetTicks) break;
#endif
#endif
        pause(i);
    }
    *spentTicks = now() - start;
    return end;
}

struct Counters {
    std::atomic<unsigned long long> identified{0}; // zero-delay Sleep(0)s recognised as the scene poll's (switch on)
    std::atomic<unsigned long long> ready{0};      // of those, the loop proceeds already (returned without the yield step)
    std::atomic<unsigned long long> watches{0};    // of those, watched (the guest would sleep again and nothing needed main)
    std::atomic<unsigned long long> changed{0};    // watches ended by a published item or the flag (the loop proceeds)
    std::atomic<unsigned long long> unquiet{0};    // watches ended because the executor needed the thread
    std::atomic<unsigned long long> expired{0};    // watches that spent the budget (the guest re-checks and sleeps again)
    std::atomic<unsigned long long> watchNs{0};    // time inside watches
    std::atomic<unsigned long long> maxNs{0};      // longest watch
    std::atomic<unsigned long long> wfe{0};        // idle-wait (diag-sw-wfe): waits for an event inside watches
    std::atomic<unsigned long long> wfeSlow{0};    // of those, longer than WfeSlowNs (no store and no event stream tick woke it sooner)
};
inline Counters& counters() noexcept { static Counters c; return c; }

} // namespace recomp_sw

#include "scene_wait_main.h" // main-wait (attempt v528): diag-sw-spin-first / -flag-mark / -producer-wake / -no-yield / -late
