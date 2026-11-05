#pragma once
// park-watch (attempt v531, 2026-10-11, the lead): main's watch of its own wake sequence before it parks, made longer and
// able to wait without work. Scheduling only: which of two exact paths main takes to the same predicate. The game sees
// the same values; guest memory is not read or written here.
//
// What was there (v346 / v347): after releasing its permit and before blocking in cv.wait, main re-reads its wake
// sequence for 40 us (files/private-trace/diag-v346-main-spin) or 100 us (diag-v347-spin100) with the yield hint. A wake
// in that time finds main not in cv.wait, so the waker skips its futex call (v184) and main goes on without a sleep and
// a wake-up. Fold run 43 (2026-10-11): main parks about 540 times a second on the settled street, 58 % of the parks end
// inside 100 us; with the 100 us watch main is put on a core 22 % less often and the frame read -1.4 ms (+-2.0), for
// 5.8 % more of main's cycles (the watch spins).
//
// What this adds, behind two switch files polled at the renderer's 60-present window:
//   diag-park-watch       the watch's length is the file's text in microseconds (empty or not a number = 300; kept inside
//                         20 .. 2000). It wins over the two older files.
//   diag-park-watch-idle  between two checks the watch waits for an event instead of spinning: the wake sequence is
//                         marked for exclusive access (LDXR, a read) and the processor waits (WFE) unless the word has
//                         already moved. The waker's store to that word, made under the scheduler lock as before, clears
//                         the mark, which is an event: no instruction is added on the waker's side. The kernel's event
//                         stream (every 100 us) bounds a wait whose event was lost and lets the watch end on time. The
//                         scheduler epoch (the second word of the predicate, moved by wake-all only) has no mark of its
//                         own: a change of it alone is seen at the next event, at most one stream tick later, and the
//                         park that would follow re-reads both words under the lock exactly as before.
// With neither file the stored length is the older files' (0, 40 or 100) and the loop is v530's: the yield hint.
//
// The loop in parallelWait keeps its shape (it carries a profile record): its statement `asm volatile("yield")` became
// `k |= recomp_park_watch_pause(&t.wakeSeq, seq)`. The loop reads the clock when (++k & 15) == 0. pause() returns
// CheckEveryTime (15) after an idle wait, so the next pass reads the clock (an idle wait can last a stream tick, and
// sixteen of them would overrun the watch by 1.6 ms), and 0 after a yield hint (the clock every sixteenth pass, as before).
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace recomp::park_watch {
inline constexpr unsigned DefaultUs = 300, MinUs = 20, MaxUs = 2000;
inline constexpr unsigned ClockMask = 15;      // parallelWait's own mask: the clock is read when (++k & ClockMask) == 0
inline constexpr unsigned CheckEveryTime = 15; // or-ed into k after an idle wait: the next pass reads the clock

// The file's text as a watch length: decimal digits (anything after them is ignored), kept inside MinUs .. MaxUs; no digit
// at all = DefaultUs.
constexpr unsigned parseUs(const char* text, std::size_t size) noexcept {
    unsigned long long value = 0;
    std::size_t digits = 0;
    while (digits < size && text[digits] >= '0' && text[digits] <= '9') {
        value = value * 10 + static_cast<unsigned>(text[digits] - '0');
#if !defined(PARK_WATCH_MUTANT_NO_MAX)
        if (value > MaxUs) value = MaxUs; // also keeps a long row of digits from overflowing
#endif
        ++digits;
    }
#if defined(PARK_WATCH_MUTANT_DEFAULT_ZERO)
    if (digits == 0) return 0;
#else
    if (digits == 0) return DefaultUs;
#endif
#if !defined(PARK_WATCH_MUTANT_NO_MIN)
    if (value < MinUs) value = MinUs;
#endif
    return static_cast<unsigned>(value);
}
// What recomp_v346_main_spin_us takes: this file's length when the file stands, else the older files' value.
constexpr unsigned chosenUs(bool filePresent, unsigned parsedUs, unsigned olderUs) noexcept {
#if defined(PARK_WATCH_MUTANT_FILE_IGNORED)
    (void)filePresent; (void)parsedUs;
    return olderUs;
#else
    return filePresent ? parsedUs : olderUs;
#endif
}
// What pause() hands back for the loop's counter.
constexpr unsigned pauseBits(bool idled) noexcept {
#if defined(PARK_WATCH_MUTANT_NO_CLOCK_AFTER_IDLE)
    (void)idled;
    return 0;
#else
    return idled ? CheckEveryTime : 0u;
#endif
}
// Does the idle form wait at all for this reading? Only while the word still holds what the watch saw.
constexpr bool waitsFor(std::uint64_t marked, std::uint64_t seen) noexcept {
#if defined(PARK_WATCH_MUTANT_WAIT_ON_MOVED)
    (void)marked; (void)seen;
    return true;
#else
    return marked == seen;
#endif
}
struct Counters {
    std::atomic<std::uint64_t> idleWaits{0}, movedAtMark{0}, yields{0};
};
Counters& counters() noexcept;
} // namespace recomp::park_watch

extern "C" {
extern std::atomic<bool> recomp_park_watch_idle; // diag-park-watch-idle (strong in guest_cooperative_executor.cpp)
// One pause of the watch (out of line, no lock held): the yield hint, or the marked idle wait when the switch stands.
unsigned recomp_park_watch_pause(const std::uint64_t* wakeSeq, std::uint64_t seen) noexcept;
}
