#pragma once
// job-path (agent package J, 2026-10-03; switch files/private-trace/diag-jp-park-poll, polled by the renderer every 60
// presents, applied at the next wait call of each task): the executor's park poll for the guest tasks that are NOT the
// frame's critical threads (main 63001000, render 6408e000, tile 640b1000, audio 64141000) starts at 2 ms instead of 250 us.
//
// Why (v414 milestone, local-analysis/agents/job-path): a parked wait re-checks its readiness every time its poll timer fires,
// with or without a notification. The poll is a safety net for a readiness change that sends no notification (the
// executor's "late" counter: ~25/s on the device). RENDER_DIAG_EXEC: 11.4 k poll-timeout wakes/s against 8.7 k notified
// wakes/s and 27 late completions/s, so > 99.7% of poll wakes are futile (the task re-parks on the same object at once).
// Timelines (tools/iter/timeline_poll_steps.py): 11.2-12.1 k futile wakes/s, 7.3 k/s of them on the 250 us / 500 us / 1 ms
// steps of non-critical tasks (job workers 2.0-2.3 k/s, scene workers 2.1 k/s, other guest threads 3.4 k/s). Each is a
// timer interrupt, two context switches, a permit acquisition and a registry readiness check on the woken thread
// (3-10 us of its CPU, tools/iter/timeline_futile_cost.py) plus the switch of the thread it preempts on the saturated big
// cores (job workers are off-CPU ~1/3 of the time they are not parked while driving).
//
// Scheduling only, not guest-visible: what a wait returns, and when it CAN complete, are unchanged. A notification ends the
// park at once (the poll is not involved); a guest deadline still bounds the park exactly (the executor parks until
// min(deadline, now + poll)); only the re-check cadence of a wait that has had no notification changes (2, 4, 8, 8 ... ms
// instead of 0.25, 0.5, 1, 2, 4, 8, 8 ... ms; the 8 ms maximum is kept). Cost: a readiness change without any notification
// (a "late" completion) of a non-critical task is noticed up to 2 ms after it happened instead of 0.25 ms on the first step.
#include <atomic>
#include <chrono>
#include <cstdint>

namespace recomp_jp {
constexpr std::uint32_t MainToken = 0x63001000u, RenderToken = 0x6408e000u, TileToken = 0x640b1000u, AudioToken = 0x64141000u;
constexpr std::chrono::microseconds ShortInitial{250}, LongInitial{2000}, Maximum{8000};

// The frame's critical threads keep the original schedule (main and render: the R/W chain; tile: the render thread's
// IRQ/drain partner; audio: the house frame's audio handshake).
inline bool critical(std::uint32_t token) noexcept {
#if defined(RECOMP_JP_MUTANT_ALL) // test mutant: the long poll for every task
    (void)token;
    return false;
#else
    return token == MainToken || token == RenderToken || token == TileToken || token == AudioToken;
#endif
}

// The first poll step of a wait (and the step after each notification).
inline std::chrono::microseconds initialPoll(std::uint32_t token, bool longPoll) noexcept {
#if defined(RECOMP_JP_MUTANT_NO_POLL) // test mutant: no safety-net poll for non-critical tasks (a non-notified change is lost)
    return longPoll && !critical(token) ? std::chrono::microseconds(std::chrono::hours(1)) : ShortInitial;
#else
    return longPoll && !critical(token) ? LongInitial : ShortInitial;
#endif
}

// The next step after a poll timeout (the executor's unchanged rule: double, capped at the maximum).
inline std::chrono::microseconds nextPoll(std::chrono::microseconds current) noexcept {
    return current * 2 < Maximum ? current * 2 : Maximum;
}
} // namespace recomp_jp
