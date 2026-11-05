#pragma once
// audio-ahead (stage D; USER-APPROVED fidelity trade-off, 2026-10-03 "go ahead with the audio fix": a bounded extra audio
// output delay so the audio engine can run ahead of AAudio's bursty demand). Runtime switch, off by default.
//
// Mechanism (v427 timelines, agent audio-ahead): in the house main parks once per frame on handle 0x0E3C until the audio engine
// (guest 64141000) wakes and acks main's per-frame request; the engine wakes only when the callback worker runs the game's
// XAudio callback, and the worker delivers callbacks only on AAudio demand, which opens in 7-8 packet refill bursts after
// each 40 ms AAudio read. Main's waits begin 30-36 ms into that cycle and all end at the next burst: the house frame is
// locked to two frames per 40 ms. With the switch, while main is parked on an object the engine signals, the worker may
// deliver one more callback even though v431's demand rule says the AAudio buffer is full, as long as the frames in flight
// stay below v431's target + lead x 256. The engine then wakes (and acks main) within ~0.2 ms instead of at the next read.
//
// What does NOT change: every packet the game submits goes through the same NativeAudioRenderClient queue to AAudio once,
// in order, unmodified (no drops, duplicates, resampling or mixing changes; AAudio sees the same writes); the regular demand
// rule is v431's; no guest code changes. What changes: WHEN some callbacks are delivered (earlier), so the game renders audio
// further ahead of the speaker: the frames in flight stay below v431's target + lead x 256 (pipeline depth at most
// lead x 5.33 ms deeper); estimated mean added event -> sound delay +1.6..2.1 ms in the house, ~0 outdoors (audio-ahead's
// calibrated model, tools/iter/audio_ahead/audio_ahead_model.py; measured on the device as RENDER_DIAG_AU_AHEAD inflight_ms).
//
// Pieces: (1) AheadBackend: a pass-through NativeAudioPlaybackBackend that reports AAudio's capacity + MaxLeadPackets x 256
// to the render client, so the client's own demand ceiling admits the extra lead (the extra packets wait in the client's own
// 8-packet queue while AAudio is full; nothing is buffered or altered here). (2) decide() / step(): the worker's gate: v431's
// rule, or with the hint up to `lead` packets more. (3) The hint: the registry learns the handles the engine thread signals
// (wakeHandleWaiters) and flags main starting an NtWaitForSingleObjectEx on one of them while it is unsignalled; that wakes
// the parked worker at once (its 1 ms backpressure wait also ends on a new hint); the handle's next signal or the end of
// main's wait clears it. (4) Counters for RENDER_DIAG_AU_AHEAD and a per-packet production -> AAudio-read latency FIFO.
// The registry and the worker loop call only the out-of-line entry points at the end of this file (their PGO records hold).
// Switch files (generic_renderer.cpp polls them at its 60-present windows): files/private-trace/diag-au-ahead (lead 2),
// diag-au-ahead-1, diag-au-ahead-2, diag-au-ahead-4 (the largest present wins). Off (lead 0): the gate is exactly v431's rule
// and no hint is raised; main's waits on engine-signalled handles are still timed (the measurement runs in both arms).
// Host test: tools/audio_ahead/tests/audio_ahead_test.cpp, with mutants RECOMP_AU_AHEAD_MUTANT_{UNBOUNDED, OFF_INFLATED,
// HINT_ANY, DROP, REORDER, DUP} (each must fail it).
#include "guest_audio_backend.h"
#include "native_audio_render_client.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

namespace recomp::bootstrap {
void wakeGuestWaiters(std::uint32_t token) noexcept;        // guest_parallel_wake.h
std::uint32_t currentGuestThreadToken() noexcept;           // guest_parallel_wake.h
}
#if defined(RECOMP_AU_AHEAD_TEST_CLOCK)
std::int64_t recomp_au_ahead_test_now() noexcept; // host test: simulated steady clock (ns)
#endif

namespace recomp_au_ahead {
constexpr std::uint32_t MainToken = 0x63001000u;   // the title's main thread (= recomp_v245::MainToken)
constexpr std::uint32_t EngineToken = 0x64141000u; // the audio engine's update thread (v346's audio thread)
constexpr unsigned MaxLeadPackets = 4;
constexpr std::uint32_t FramesPerPacket = 256;
constexpr std::uint32_t ExtraFrames = MaxLeadPackets * FramesPerPacket;
constexpr std::size_t HandleSlots = 8;

struct State {
    std::atomic<unsigned> lead{0};                // extra packets allowed while main waits on the engine (0 = off)
    std::atomic<std::uint32_t> workerToken{0};    // the XAudio callback worker (woken by a hint)
    std::array<std::atomic<std::uint32_t>, HandleSlots> engineHandles{}; // handles the engine thread signalled (learned)
    std::atomic<std::uint32_t> mainWaiting{0};    // engine-signalled handle main is parked on now (0 = none)
    std::atomic<std::uint32_t> hintSeq{0};        // +1 per main wait that raised a hint
    std::atomic<std::uint32_t> mainWaitHandle{0}; // the handle of main's timed wait in progress (both arms)
    std::atomic<std::int64_t> mainWaitStartNs{0};
    // Monotone counters (the renderer logs window deltas).
    std::atomic<unsigned long long> callbacks{0}, early{0}, refused{0}, hints{0}, wakes{0}, leadSumFrames{0};
    std::atomic<unsigned long long> latSumUs{0}, latCount{0}, latOverflow{0}, mainWaits{0}, mainWaitUs{0}, drops{0};
    // Time-weighted frames in flight (trapezoids between the worker's polls): its window mean / 48 is the pipeline delay in
    // ms; ON minus OFF = the mean added delay from a game event to its sound (the device's read schedule is the same).
    std::atomic<unsigned long long> inFlightFrameUs{0}, inFlightUs{0};
    // Window extremes (the renderer exchanges them back to these initial values).
    std::atomic<long long> leadMaxFrames{-1}, leadMinFrames{1LL << 40}, latMaxUs{-1};
};
inline State& state() noexcept { static State s; return s; }
inline std::int64_t nowNs() noexcept {
#if defined(RECOMP_AU_AHEAD_TEST_CLOCK)
    return recomp_au_ahead_test_now();
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}
inline void atomicMax(std::atomic<long long>& a, long long v) noexcept {
    long long c = a.load(std::memory_order_relaxed);
    while (v > c && !a.compare_exchange_weak(c, v, std::memory_order_relaxed)) {}
}
inline void atomicMin(std::atomic<long long>& a, long long v) noexcept {
    long long c = a.load(std::memory_order_relaxed);
    while (v < c && !a.compare_exchange_weak(c, v, std::memory_order_relaxed)) {}
}

// v431's demand target: the real AAudio capacity rounded up to whole packets (NativeAudioRenderClient::pollLocked).
inline std::uint64_t baseTarget(const recomp::bootstrap::NativeAudioConfiguration& real) noexcept {
    return (std::uint64_t{real.bufferCapacityFrames} + FramesPerPacket - 1) / FramesPerPacket * FramesPerPacket;
}

enum class Gate { Hold, Regular, Early };
// clientCallback: the client's own poll said Callback (its ceiling is baseTarget + ExtraFrames behind AheadBackend);
// accepted packets, frames AAudio read, packets still in the client's queue, v431's target, the switch's lead, the hint.
inline Gate decide(bool clientCallback, std::uint64_t acceptedPackets, std::uint64_t readFrames, std::size_t pendingPackets,
                   std::uint64_t base, unsigned lead, bool hint) noexcept {
    if (!clientCallback) return Gate::Hold;
    const std::uint64_t accepted = acceptedPackets * FramesPerPacket;
    const std::uint64_t inFlight = accepted > readFrames ? accepted - readFrames : 0;
    const bool room = pendingPackets < recomp::audio::NativeAudioQueue::CapacityPackets;
#if defined(RECOMP_AU_AHEAD_MUTANT_OFF_INFLATED)
    if (inFlight < base + ExtraFrames && room) return Gate::Regular; // mutant: the inflated ceiling leaks into the regular rule
#endif
    if (inFlight < base && room) return Gate::Regular; // v431's rule, unchanged
    const unsigned l = std::min(lead, MaxLeadPackets);
#if defined(RECOMP_AU_AHEAD_MUTANT_UNBOUNDED)
    if (l && hint && room) return Gate::Early; // mutant: no lead bound (only the client's ceiling)
#endif
    if (l && hint && room && inFlight < base + std::uint64_t{l} * FramesPerPacket) return Gate::Early;
    return Gate::Hold;
}

// The client's backend: AAudio unchanged except the reported capacity (+ExtraFrames, constant for the stream's life, so the
// client's per-poll configuration check holds). The DROP / REORDER / DUP mutants model a faulty intermediate buffer.
class AheadBackend final : public recomp::audio::NativeAudioPlaybackBackend {
public:
    explicit AheadBackend(std::shared_ptr<recomp::audio::NativeAudioPlaybackBackend> inner) noexcept : inner_(std::move(inner)) {}
    std::optional<recomp::bootstrap::NativeAudioConfiguration> configuration() const noexcept override {
        auto c = inner_ ? inner_->configuration() : std::nullopt;
        if (!c || c->bufferCapacityFrames > 48000u - ExtraFrames) return std::nullopt;
        c->bufferCapacityFrames += ExtraFrames;
        return recomp::bootstrap::validNativeAudioConfiguration(*c) ? c : std::nullopt;
    }
    // The real stream's configuration (v431's target is derived from it).
    std::optional<recomp::bootstrap::NativeAudioConfiguration> realConfiguration() const noexcept {
        return inner_ ? inner_->configuration() : std::nullopt;
    }
    bool requestStart() noexcept override { return inner_->requestStart(); }
    bool started() const noexcept override { return inner_->started(); }
    std::optional<std::uint64_t> framesWritten() const noexcept override {
        const auto w = inner_->framesWritten();
#if defined(RECOMP_AU_AHEAD_MUTANT_DROP) || defined(RECOMP_AU_AHEAD_MUTANT_DUP) || defined(RECOMP_AU_AHEAD_MUTANT_REORDER)
        if (w) return static_cast<std::uint64_t>(static_cast<std::int64_t>(*w) + delta_); // the faulty buffer hides its error
#endif
        return w;
    }
    std::optional<std::uint64_t> framesRead() const noexcept override { return inner_->framesRead(); }
    std::int32_t writeStereo(const float* interleaved, std::uint32_t frames) noexcept override {
#if defined(RECOMP_AU_AHEAD_MUTANT_DROP) || defined(RECOMP_AU_AHEAD_MUTANT_DUP) || defined(RECOMP_AU_AHEAD_MUTANT_REORDER)
        if (frames == FramesPerPacket) {
            ++writes_;
#if defined(RECOMP_AU_AHEAD_MUTANT_DROP)
            if (writes_ == 300) { delta_ += frames; return static_cast<std::int32_t>(frames); } // acknowledged, never written
#elif defined(RECOMP_AU_AHEAD_MUTANT_DUP)
            if (writes_ == 300) {
                const auto a = inner_->writeStereo(interleaved, frames);
                if (a == static_cast<std::int32_t>(frames) && inner_->writeStereo(interleaved, frames) > 0) delta_ -= frames;
                return a;
            }
#else
            if (writes_ == 300) { std::copy(interleaved, interleaved + 2 * frames, held_.begin()); delta_ += frames;
                                  return static_cast<std::int32_t>(frames); } // held back ...
            if (writes_ == 301) { // ... and written after the next packet
                const auto a = inner_->writeStereo(interleaved, frames);
                if (a == static_cast<std::int32_t>(frames) &&
                    inner_->writeStereo(held_.data(), frames) == static_cast<std::int32_t>(frames)) delta_ -= frames;
                return a;
            }
#endif
        }
#endif
        return inner_->writeStereo(interleaved, frames);
    }
private:
    std::shared_ptr<recomp::audio::NativeAudioPlaybackBackend> inner_;
#if defined(RECOMP_AU_AHEAD_MUTANT_DROP) || defined(RECOMP_AU_AHEAD_MUTANT_DUP) || defined(RECOMP_AU_AHEAD_MUTANT_REORDER)
    std::uint64_t writes_{0};
    std::int64_t delta_{0};
    std::array<float, 2 * FramesPerPacket> held_{};
#endif
};

// ---- the hint (reached from the registry through the entry points below; atomics only, safe under the registry mutex) ----
inline bool engineHandle(const State& s, std::uint32_t handle) noexcept {
    if (!handle) return false;
    for (const auto& slot : s.engineHandles)
        if (slot.load(std::memory_order_relaxed) == handle) return true;
    return false;
}
// A handle changed state (wakeHandleWaiters); `signaller` is the calling guest thread's token. A signal of the handle main
// waits on ends the hint (the producer ran); a handle the engine signals is learned (the first HandleSlots distinct ones).
inline void noteHandleSignal(std::uint32_t handle, std::uint32_t signaller) noexcept {
    auto& s = state();
    if (!handle) return;
    if (s.mainWaiting.load(std::memory_order_relaxed) == handle) {
        std::uint32_t expected = handle;
        s.mainWaiting.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
    }
    if (signaller != EngineToken || engineHandle(s, handle)) return;
    for (auto& slot : s.engineHandles) {
        std::uint32_t v = 0;
        if (slot.compare_exchange_strong(v, handle, std::memory_order_acq_rel) || v == handle) return;
    }
}
// A handle wait found its object unavailable and keeps waiting (first = the first time for this wait call).
inline void noteWaitUnavailable(std::uint32_t handle, std::uint32_t waiter, bool first) noexcept {
    auto& s = state();
#if defined(RECOMP_AU_AHEAD_MUTANT_HINT_ANY)
    if (!engineHandle(s, handle)) return; // mutant: any waiter raises the hint
#else
    if (waiter != MainToken || !engineHandle(s, handle)) return;
#endif
    if (first && waiter == MainToken) { // timing (both arms)
        s.mainWaitHandle.store(handle, std::memory_order_relaxed);
        s.mainWaitStartNs.store(nowNs(), std::memory_order_relaxed);
        s.mainWaits.fetch_add(1, std::memory_order_relaxed);
    }
    if (!s.lead.load(std::memory_order_relaxed)) return;
    if (s.mainWaiting.load(std::memory_order_relaxed) != handle) s.mainWaiting.store(handle, std::memory_order_release);
    if (!first) return;
    s.hints.fetch_add(1, std::memory_order_relaxed);
    s.hintSeq.fetch_add(1, std::memory_order_acq_rel);
    if (const auto w = s.workerToken.load(std::memory_order_acquire)) {
        recomp::bootstrap::wakeGuestWaiters(w); // the scheduler lock is a leaf (guest_parallel_wake.h)
        s.wakes.fetch_add(1, std::memory_order_relaxed);
    }
}
// The handle wait returned (signalled, timed out or rejected).
inline void noteWaitDone(std::uint32_t handle, std::uint32_t waiter) noexcept {
    auto& s = state();
    if (waiter != MainToken || !handle) return;
    std::uint32_t expected = handle;
    s.mainWaiting.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
    if (s.mainWaitHandle.load(std::memory_order_relaxed) != handle) return;
    s.mainWaitHandle.store(0, std::memory_order_relaxed);
    const auto d = nowNs() - s.mainWaitStartNs.load(std::memory_order_relaxed);
    if (d > 0) s.mainWaitUs.fetch_add(static_cast<unsigned long long>(d / 1000), std::memory_order_relaxed);
}

// ---- the worker's bookkeeping (the worker thread only, except the published atomics) ----
// Production -> AAudio-read latency of every packet: the packet ending at frame `end` was submitted at `ns`; it is counted
// when the worker first sees AAudio's read position at or past `end` (the worker polls every <= 1 ms while it has no demand,
// after each callback otherwise: the same observation bias in both arms).
struct LatencyFifo {
    struct Entry { std::uint64_t end; std::int64_t ns; };
    static constexpr std::size_t Capacity = 64; // > the client's ceiling in packets (20) plus its queue
    std::array<Entry, Capacity> e{};
    std::size_t head = 0, count = 0;
    std::int64_t lastNs = 0;          // the previous poll (time-weighted frames in flight)
    std::uint64_t lastInFlight = 0;
    void produced(std::uint64_t end, std::int64_t ns) noexcept {
        if (count == Capacity) { head = (head + 1) % Capacity; --count; state().latOverflow.fetch_add(1, std::memory_order_relaxed); }
        e[(head + count) % Capacity] = {end, ns};
        ++count;
    }
    void observe(std::uint64_t readFrames, std::int64_t ns) noexcept {
        auto& s = state();
        while (count && e[head].end <= readFrames) {
            const auto us = (ns - e[head].ns) / 1000;
            if (us >= 0) {
                s.latSumUs.fetch_add(static_cast<unsigned long long>(us), std::memory_order_relaxed);
                s.latCount.fetch_add(1, std::memory_order_relaxed);
                atomicMax(s.latMaxUs, us);
            }
            head = (head + 1) % Capacity;
            --count;
        }
    }
};

// One worker iteration after the client's poll (trace_runner.cpp's callback worker and the host test both call this).
// Hold: wait <= 1 ms, ending early when hintSeq != seen. Regular / Early: deliver one callback now.
struct Step { Gate gate; std::uint32_t seen; std::uint64_t inFlight; };
inline Step step(bool clientCallback, const recomp::audio::NativeAudioRenderClient::Statistics& st, std::uint64_t base,
                 LatencyFifo& fifo, std::uint32_t& lastRefused, std::int64_t now) noexcept {
    auto& s = state();
    const auto seen = s.hintSeq.load(std::memory_order_acquire); // before the decision: a later hint ends the wait
    fifo.observe(st.serviceReadFrames, now);
    if (st.callbacksCompleted != st.acceptedPackets) // every completed callback submitted exactly one packet (must stay 0)
        s.drops.store(st.callbacksCompleted > st.acceptedPackets ? st.callbacksCompleted - st.acceptedPackets
                                                                 : st.acceptedPackets - st.callbacksCompleted,
                      std::memory_order_relaxed);
    const unsigned lead = s.lead.load(std::memory_order_relaxed);
    const bool hint = s.mainWaiting.load(std::memory_order_acquire) != 0;
    const auto g = decide(clientCallback, st.acceptedPackets, st.serviceReadFrames, st.pendingPackets, base, lead, hint);
    const std::uint64_t accepted = st.acceptedPackets * FramesPerPacket;
    const std::uint64_t inFlight = accepted > st.serviceReadFrames ? accepted - st.serviceReadFrames : 0;
    if (fifo.lastNs && now > fifo.lastNs) {
        const auto us = static_cast<unsigned long long>((now - fifo.lastNs) / 1000);
        s.inFlightFrameUs.fetch_add((fifo.lastInFlight + inFlight) / 2 * us, std::memory_order_relaxed);
        s.inFlightUs.fetch_add(us, std::memory_order_relaxed);
    }
    fifo.lastNs = now;
    fifo.lastInFlight = inFlight;
    if (g == Gate::Hold && clientCallback && lead && hint && seen != lastRefused) { // at the lead bound
        s.refused.fetch_add(1, std::memory_order_relaxed);
        lastRefused = seen;
    }
    if (g != Gate::Hold) {
        auto& c = s;
        c.callbacks.fetch_add(1, std::memory_order_relaxed);
        if (g == Gate::Early) c.early.fetch_add(1, std::memory_order_relaxed);
        c.leadSumFrames.fetch_add(inFlight, std::memory_order_relaxed);
        atomicMax(c.leadMaxFrames, static_cast<long long>(inFlight));
        atomicMin(c.leadMinFrames, static_cast<long long>(inFlight));
    }
    return {g, seen, inFlight};
}
inline bool hintArrived(std::uint32_t seen) noexcept {
    return state().hintSeq.load(std::memory_order_acquire) != seen;
}

// ---- out-of-line entry points for the profiled call sites ----
// The call sites (the registry's dispatcher, its wait lambda and wakeHandleWaiters; the callback worker's loop) get one
// plain call each and no new branch or local object, so their control flow, and with it their PGO records
// (round5-v422.profdata: IR-level CFG hashes), stay those of v431.
struct Worker { // the callback worker's own bookkeeping (that thread only)
    std::uint64_t base = 0;        // v431's demand target (frames)
    std::uint32_t seen = 0;        // hintSeq at the last decision (a later hint ends the Hold wait)
    std::uint32_t refusedSeq = 0;
    LatencyFifo fifo;
};
inline Worker& worker() noexcept { static Worker w; return w; }
[[gnu::noinline]] inline void onHandleSignal(std::uint32_t handle) noexcept {
    noteHandleSignal(handle, recomp::bootstrap::currentGuestThreadToken());
}
// NtWaitForSingleObjectEx, under the registry mutex before it unlocks to wait: `signalCount` / `mutant` of the waited object
// (an event with no signal will park the caller unless a signal lands in the next microseconds: such a race costs one
// unneeded early packet at most, inside the bound).
[[gnu::noinline]] inline void onWaitBegin(std::uint32_t handle, std::uint32_t waiter, bool first, std::uint32_t signalCount,
                                          bool mutant) noexcept {
    if (first && !mutant && signalCount == 0) noteWaitUnavailable(handle, waiter, true);
}
[[gnu::noinline]] inline void onWaitReturned(std::uint32_t handle, std::uint32_t waiter, bool completed, bool resolved) noexcept {
    if (!completed || resolved) noteWaitDone(handle, waiter);
}
// At the worker's start: its token (for the hint's wake) and v431's target from the real stream's capacity (from the
// reported one if the real one is unavailable; with neither the gate holds and the client fails at its own next poll).
[[gnu::noinline]] inline void workerStart(std::uint32_t token, const AheadBackend* backend) noexcept {
    auto& w = worker();
    const auto real = backend ? backend->realConfiguration() : std::nullopt;
    const auto shown = backend ? backend->configuration() : std::nullopt;
    if (real) w.base = baseTarget(*real);
    else if (shown && shown->bufferCapacityFrames > ExtraFrames)
        w.base = baseTarget({shown->sampleRate, shown->channels, shown->bufferCapacityFrames - ExtraFrames, shown->framesPerBurst});
    state().workerToken.store(token, std::memory_order_release);
}
// After the client's poll: Hold (wait <= 1 ms or until a new hint) or deliver a callback (Regular / Early).
[[gnu::noinline]] inline Gate workerStep(bool clientCallback, const recomp::audio::NativeAudioRenderClient& client) noexcept {
    auto& w = worker();
    const auto s = step(clientCallback, client.statistics(), w.base, w.fifo, w.refusedSeq, nowNs());
    w.seen = s.seen;
    return s.gate;
}
// The worker loop's form: the client's poll result in, the demand the loop acts on out (Failed and Wait unchanged; a
// Callback the gate holds becomes Wait), so the loop keeps v431's two tests of `demand`.
[[gnu::noinline]] inline recomp::audio::NativeAudioRenderClient::Demand gateDemand(
        recomp::audio::NativeAudioRenderClient::Demand demand, const recomp::audio::NativeAudioRenderClient& client) noexcept {
    using Demand = recomp::audio::NativeAudioRenderClient::Demand;
    if (demand == Demand::Failed) return demand;
    return workerStep(demand == Demand::Callback, client) == Gate::Hold ? Demand::Wait : Demand::Callback;
}
// The Hold wait's predicate: its 1 ms deadline (v431's backpressure wait) or a hint raised after the decision.
[[gnu::noinline]] inline bool holdOver(std::chrono::steady_clock::time_point deadline) noexcept {
    return std::chrono::steady_clock::now() >= deadline || hintArrived(worker().seen);
}
// After finishCallback: the packet just submitted ends at frame acceptedPackets x 256.
[[gnu::noinline]] inline void workerProduced(const recomp::audio::NativeAudioRenderClient& client) noexcept {
    worker().fifo.produced(client.statistics().acceptedPackets * FramesPerPacket, nowNs());
}
}
