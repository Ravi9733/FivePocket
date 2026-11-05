#pragma once
// native-imports (lane native-syscalls, attempt v526), "clock reads off the hot paths": the executor's clock reads that feed
// ONLY its accounting lines, behind the lane's switch (files/private-trace/diag-native-imports; the renderer's window poll
// sets recomp_native_clock_quiet through recomp::native_imports::window()).
//
// Which reads (guest_cooperative_executor.cpp):
//   grantLocked           Task::permitSince, the start of a permit hold. Read back only through elapsedNs(permitSince): the
//                         hold totals and maximum (RENDER_DIAG_EXEC's permit hold figures, the per-lane "held" columns) and
//                         the grant-to-running latency. One clock read at every grant and one at every release, that is two
//                         per park / resume of a waiting guest thread and per permit rotation.
//   schedLockContended    the wait for a contended scheduler lock (RENDER_DIAG_EXEC_SCHED's wait figures); the spin itself
//                         and the lock are untouched.
// With the switch on, stamp() returns the zero time point without reading the clock, and elapsedNs() of a zero stamp is 0
// without reading it (the figures above then read 0 for the intervals stamped while the switch was on; an interval stamped
// before the switch came on is still measured). With the switch off: the same reads and values as v524.
// Every clock read the executor's BEHAVIOUR depends on is untouched: deadlines and poll steps of waits and delays, the
// permit spin budget, the rotation's minimum wait (Task::queuedAt has its own read), the stall watchdog, the log cadence.
// Kept as well, although only a counter reads it: the "woken at its timeout" audit of v185 in parallelWait (one read per
// notified wake), the only monitor of v184's skipped notifies.
//
// Out of line on purpose: at each site one call replaces one call, so the calling functions keep their control flow
// (and their PGO records; checked on the device compiler by the lane's tools/device_compile.py).
#include <atomic>
#include <chrono>
#include <cstdint>

extern "C" { inline std::atomic<bool> recomp_native_clock_quiet{false}; }

namespace recomp::native_clock {
using Clock = std::chrono::steady_clock;
// The start of an accounting interval.
[[gnu::noinline]] inline Clock::time_point stamp() noexcept {
#if !defined(RECOMP_NATIVE_MUTANT_CLOCK_ALWAYS) // (mutant: the accounting stamps read the clock with the switch on)
    if (::recomp_native_clock_quiet.load(std::memory_order_relaxed)) return Clock::time_point{};
#endif
    return Clock::now();
}
// "Now" for elapsedNs(since): `since` itself for a zero stamp (0 ns, no clock read), the clock for every other.
[[gnu::noinline]] inline Clock::time_point now(Clock::time_point since) noexcept {
    return since == Clock::time_point{} ? since : Clock::now();
}
} // namespace recomp::native_clock
