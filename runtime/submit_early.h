#pragma once
// submit-early package (Q, 2026-10-03): pure helpers of the renderer's early-submission switches and of the chunk trace
// (tests/submit_early_test.cpp). Every caller is off unless files/private-trace/diag-se-big / diag-se-idle exist (polled with
// the renderer's 60-present switch window; RENDER_GENERIC_V210Y se=<bits held for the whole window>) or the v159 timeline is
// armed (the trace events below), so a run without them executes v419's code plus a few plain loads.
//
// Why: the frame's FIRST vkQueueSubmit comes only after its heaviest pass (the G-buffer, ~400-1100 draws in one render pass)
// has been recorded: v337 submits at a pass begin once >= 192 items are pending, and the passes before the G-buffer
// (reflection faces etc., ~1 ms of GPU) hold fewer. Meanwhile the GPU is idle (it finished the previous frame ~7 ms earlier),
// and from that first submission to the presenter's end it never waits for a submission again (v343/v419 pass profiles): the
// GPU block [first submission, GPU done] is ~15-17 ms of back-to-back work. ModeBig submits the pending commands right before
// the predicted heaviest pass begins, so that pre-pass work runs during the heaviest pass's recording instead of inside the
// block. Exact: only where the command stream is split into command buffers changes (between passes, as v337's chunks).
#include "diag_timeline.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>

namespace se {
// The trace's timeline kinds (tools/submit_early/chunk_trace.py), measurement only, recorded while armed. They are values of
// diag_timeline.h's Kind (uint16_t) kept here so that header is unchanged (48-52: 40-47 left free for other packages). extra =
// frameItems(the renderer's frame number, an item count).
namespace tlk {
using K=recomp::diag::timeline::Kind;
constexpr K ChunkSubmitted=K(48); // a frame command buffer was submitted (Vulkan worker; arg: submitArg(reason, chunk index);
                                  // items = the draws/clears/resolves in it)
constexpr K ChunkGpuBegin=K(49);  // that chunk's GPU start / end timestamps on the steady clock (the presenter watcher's offset:
constexpr K ChunkGpuEnd=K(50);    // needs diag-lat-gpu-done; arg: chunk index; recorded when the slot's fence is waited)
constexpr K WorkerIdle=K(51);     // the Vulkan worker ran out of items while recording a frame (arg: bit 0 a pass was open, bit 1
                                  // diag-se-idle submitted; items = pending draws/clears/resolves)
constexpr K FrameTag=K(52);       // a present tag (extra) and its renderer frame number (arg, low 16 bits), at its final submit
}
constexpr unsigned ModeBig=1;   // diag-se-big: submit the pending commands before the predicted heaviest pass of the frame
constexpr unsigned ModeIdle=2;  // diag-se-idle: submit the pending commands when the Vulkan worker runs out of items, if no pass is open
#if defined(RECOMP_SE_TEST_BIG_MIN)      // host harness: its scenes draw ~36 times per frame
constexpr std::uint32_t BigMinDraws=RECOMP_SE_TEST_BIG_MIN;
#else
constexpr std::uint32_t BigMinDraws=64;  // the previous frame's heaviest pass must have had at least this many draws
#endif
#if defined(RECOMP_SE_TEST_MIN_PENDING)  // host harness: every pending item counts
constexpr unsigned MinPending=RECOMP_SE_TEST_MIN_PENDING;
#else
constexpr unsigned MinPending=16;        // draws/clears/resolves pending, at least, for an early submission (~0.1-0.3 ms of worker CPU each)
#endif

// The heaviest-pass predictor. During a frame every ended pass reports (its framebuffer key, the draws recorded in it); at the
// frame's end the key of the pass with the most draws (>= BigMinDraws) becomes the next frame's prediction. begins(key), asked
// at every pass begin, is true once per frame: at the first begin of the predicted key.
struct BigPass {
    std::uint64_t predicted=0;bool havePrediction=false;
    std::uint64_t bestKey=0;std::uint32_t bestDraws=0;
    bool fired=false;
    void passEnded(std::uint64_t key,std::uint32_t draws) noexcept {
#if defined(RECOMP_SE_MUTANT_FIRST_PASS) // test mutant: the first pass of the frame is taken as the heaviest
        if(!bestDraws && draws){bestDraws=draws;bestKey=key;}
#elif defined(RECOMP_SE_TEST_PREFER_FEWEST) // host harness: the pass with the FEWEST draws is predicted (its scene's heaviest
        if(draws && (!bestDraws || draws<bestDraws)){bestDraws=draws;bestKey=key;} // pass is the frame's first, so nothing is pending there)
#else
        if(draws>bestDraws){bestDraws=draws;bestKey=key;}
#endif
    }
    void frameEnd() noexcept {havePrediction=bestDraws>=BigMinDraws;predicted=bestKey;bestDraws=0;bestKey=0;fired=false;}
    bool begins(std::uint64_t key) noexcept {
#if defined(RECOMP_SE_MUTANT_EVERY_BEGIN) // test mutant: fires at every begin of the predicted key, not once per frame
        if(!havePrediction || key!=predicted)return false;fired=true;return true;
#else
        if(fired || !havePrediction || key!=predicted)return false;
        fired=true;return true;
#endif
    }
    void reset() noexcept {*this=BigPass{};}
};

// Trace (armed timeline only): why a renderer command buffer of a frame was submitted.
enum Reason : unsigned {Threshold=0,Big=1,Idle=2,Final=3,Pending=4};
// SeChunkSubmitted's arg: the reason (bits 0-3) and the chunk's index in its frame (bits 4-15, saturating at 4095).
constexpr std::uint16_t submitArg(unsigned reason,unsigned chunk) noexcept {
    return std::uint16_t((reason&15u)|(std::min(chunk,4095u)<<4));
}
// Extra of the trace events: the renderer's frame number (low 16 bits) and an item count (high 16 bits, saturating).
constexpr std::uint32_t frameItems(std::uint64_t frame,std::uint64_t items) noexcept {
    return std::uint32_t(frame&0xFFFFu)|(std::uint32_t(std::min<std::uint64_t>(items,65535u))<<16);
}

// GPU timestamps -> steady_clock ns for the trace. The presenter's GPU-done watcher (diag-lat-gpu-done) sees each presenter
// fence signal at cpuNs and reads the pass's end timestamp gpuNs (ticks x period): cpuNs - gpuNs is the clock offset plus the
// watcher's wake-up delay (>= 0), so the minimum over a window of frames is the best estimate. One writer (the watcher), any
// number of readers.
struct ClockOffset {
    static constexpr std::int64_t None=std::numeric_limits<std::int64_t>::min();
    static constexpr unsigned Window=16;
    std::atomic<std::int64_t> published{None};
    std::int64_t windowMin=std::numeric_limits<std::int64_t>::max();unsigned n=0;
    void sample(std::int64_t cpuNs,std::int64_t gpuNs) noexcept {
        const std::int64_t o=cpuNs-gpuNs;
#if defined(RECOMP_SE_MUTANT_OFFSET_MAX) // test mutant: the window's maximum (the latest wake-up) instead of its minimum
        if(windowMin==std::numeric_limits<std::int64_t>::max() || o>windowMin)windowMin=o;
#else
        if(o<windowMin)windowMin=o;
#endif
        if(++n>=Window){published.store(windowMin,std::memory_order_release);windowMin=std::numeric_limits<std::int64_t>::max();n=0;}
    }
    bool get(std::int64_t& o) const noexcept {o=published.load(std::memory_order_acquire);return o!=None;}
};
inline ClockOffset gpuClock;
// The mapped time of a raw GPU timestamp (ticks & mask, x period ns) on the steady clock.
inline std::uint64_t gpuToCpu(std::uint64_t ticks,std::uint64_t mask,double periodNs,std::int64_t offset) noexcept {
    return std::uint64_t(std::int64_t(double(ticks&mask)*periodNs)+offset);
}
// The worker's idle hook is wanted (diag-se-idle, or the timeline armed: the idle trace); set by the renderer's switch poll.
inline std::atomic<bool> idleWanted{false};
}
