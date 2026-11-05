#pragma once
// sens-jobs (attempt v535, 2026-10-11, the lead): MEASUREMENT ONLY. Extra work on the threads that publish scene items
// (the scene workers' two producer functions, through the hook main-wait put at their end), to learn whether the frame
// follows the job workers' work on the Fold. Run 32 answered that for main (yes), the drawing thread, the graphics
// worker and the sound engine (not resolved) and left the job workers unmeasured; main spends about 6 ms of every
// frame in its wait for exactly these items.
//   files/private-trace/diag-sens-jobs   text = microseconds of spinning per call of the hook (empty or no number = 15;
//                                        kept inside 1 .. 200). Polled at the renderer's 60-present window.
// The spin reads the processor's timer and nothing of the game; it changes no value the game can see, only how long a
// producer takes. Never part of a play build: with the file absent the hook makes one relaxed load more than v534.
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace recomp::sens_jobs {
inline constexpr unsigned DefaultUs = 15, MinUs = 1, MaxUs = 200;
constexpr unsigned parseUs(const char* text, std::size_t size) noexcept {
    unsigned long long value = 0;
    std::size_t digits = 0;
    while (digits < size && text[digits] >= '0' && text[digits] <= '9') {
        value = value * 10 + static_cast<unsigned>(text[digits] - '0');
#if !defined(SENS_JOBS_MUTANT_NO_MAX)
        if (value > MaxUs) value = MaxUs;
#endif
        ++digits;
    }
    if (digits == 0) return DefaultUs;
#if !defined(SENS_JOBS_MUTANT_NO_MIN)
    if (value < MinUs) value = MinUs;
#endif
    return static_cast<unsigned>(value);
}
// Timer ticks for a spin of `us` microseconds at `hz` ticks a second.
constexpr std::uint64_t ticksFor(unsigned us, std::uint64_t hz) noexcept {
#if defined(SENS_JOBS_MUTANT_MILLISECONDS)
    return static_cast<std::uint64_t>(us) * hz / 1000ull;
#else
    return static_cast<std::uint64_t>(us) * hz / 1000000ull;
#endif
}
struct State {
    std::atomic<unsigned> us{0};                 // 0 = off
    std::atomic<std::uint64_t> calls{0}, spunTicks{0};
};
State& state() noexcept;
} // namespace recomp::sens_jobs

extern "C" void recomp_sens_jobs_spin() noexcept; // out of line; called by the producers' hook
