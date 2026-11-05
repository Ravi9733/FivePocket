#pragma once
// v266 (switch files/private-trace/diag-v266-poll-sleep, re-read once a second by the Sleep(0) fast path): main's Sleep(0)
// poll (sub_8239DF60 -> SleepEx sub_824F1388) waits for a job counter the scene/job workers store (plain guest stores, no
// import), sched_yield()ing on a big core while those workers queue for a CPU (v261 outdoors: 4.3 ms/frame of poll, 6.5
// phases, p50 340 us; cpu3-6 ~90% busy; the poll ends within ~10 us of the workers parking). With the switch, a poll phase
// older than SpinNs sleeps instead of yielding: a futex wait on `word` for at most TimeoutNs, ended early by the next guest
// park anywhere (beforePark(), called when a wait's immediate check fails). The idle core can then take a queued worker.
// Scheduling only: Sleep(0) still returns STATUS_SUCCESS after the same delay handler; the guest re-checks its own
// condition after every return. A park that lands between the poller's last check and its futex wait is only seen at the
// timeout (bounded latency, never a lost wake of guest state).
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <sched.h>
#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif

namespace recomp_v266 {
struct PollSleep {
    static constexpr std::uint64_t SpinNs = 20'000, TimeoutNs = 150'000, PhaseGapNs = 50'000;
    std::atomic<bool> enabled{false};
    std::atomic<std::uint32_t> word{0};     // bumped by a park while a poller sleeps
    std::atomic<std::uint32_t> sleepers{0};
    std::atomic<unsigned long long> sleeps{0}, woken{0}, timeouts{0}, early{0}, parkWakes{0}, yields{0};
    // v270 A/B (diag-v270-clock-baseline, applied at the renderer's windows): v266-v269's clock reads on main's hot paths
    // (two steady_clock reads per Sleep(0) with the poll sleep off; telemetry nowMs through steady_clock)
    std::atomic<bool> clockBaseline{false};
};
inline PollSleep& pollSleep() { static PollSleep s; return s; }

// A guest thread is about to park: wake the sleeping pollers (one relaxed-cost load when none sleeps).
inline void beforePark() {
    auto& s = pollSleep();
    if (s.sleepers.load(std::memory_order_seq_cst) == 0) return;
    s.word.fetch_add(1, std::memory_order_seq_cst);
    s.parkWakes.fetch_add(1, std::memory_order_relaxed);
#if defined(__linux__)
    (void)::syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&s.word), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
#endif
}

// Sleep(0) in a poll phase older than SpinNs: until the next park or TimeoutNs.
inline void sleepPoll() {
    auto& s = pollSleep();
    s.sleepers.fetch_add(1, std::memory_order_seq_cst);
    const auto w = s.word.load(std::memory_order_seq_cst);
    s.sleeps.fetch_add(1, std::memory_order_relaxed);
#if defined(__linux__)
    timespec ts{0, static_cast<long>(PollSleep::TimeoutNs)};
    const long rc = ::syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&s.word), FUTEX_WAIT_PRIVATE, w, &ts, nullptr, 0);
    const int e = rc == 0 ? 0 : errno;
    if (rc == 0) s.woken.fetch_add(1, std::memory_order_relaxed);
    else if (e == ETIMEDOUT) s.timeouts.fetch_add(1, std::memory_order_relaxed);
    else s.early.fetch_add(1, std::memory_order_relaxed); // EAGAIN (bumped before the wait) / EINTR
#else
    (void)w; ::sched_yield();
#endif
    s.sleepers.fetch_sub(1, std::memory_order_seq_cst);
}
} // namespace recomp_v266
