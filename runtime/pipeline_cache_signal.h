// boot-stall (lane boot-stall, 2026-10-06): what the generic renderer tells the persistent pipeline cache (pipeline_cache_store.h;
// started by device.cpp). Three atomics and two functions that store into them: no file, no lock, no system call, no log line.
//   noteCreated()      the renderer made a pipeline through the device's VkPipelineCache (called by pwNoteCreation, the counter
//                      function recordRecipe runs right after pipeline()'s create, so: while recipes are on; a driver cache
//                      hit counts too: the renderer cannot tell them apart). The creating thread.
//   warmupFinished(n)  the warm-up (the v139 prewarm thread) ended, or there is none in this run; n = recipes it compiled.
// The cache's saver thread reads them every few seconds and decides by itself whether the cache is worth writing: nothing here
// can wait for it. Nothing reads them back into the renderer: what is drawn does not depend on them.
#pragma once
#include <atomic>
#include <cstdint>

namespace recomp::pcache {
struct Signals {
    std::atomic<std::uint64_t> created{0};        // pipelines the renderer created (compiles and cache hits)
    std::atomic<std::uint64_t> warmupCompiled{0}; // the warm-up's count when it ended (for the save's log line)
    std::atomic<bool> warmupDone{false};
};
inline Signals signals; // (constant-initialised: no guard on the draw path)
#if defined(RECOMP_PCACHE_MUTANT_INLINE_SAVE) // host-test mutant: the saver's work is done by whoever raises a signal (the frame path)
inline std::atomic<void(*)()> mutantInlineSave{nullptr};
#endif
inline void noteCreated() noexcept {
    signals.created.fetch_add(1, std::memory_order_relaxed);
#if defined(RECOMP_PCACHE_MUTANT_INLINE_SAVE)
    if (const auto f = mutantInlineSave.load()) f();
#endif
}
inline void warmupFinished(std::uint64_t compiled) noexcept {
    signals.warmupCompiled.store(compiled, std::memory_order_relaxed);
    signals.warmupDone.store(true, std::memory_order_release);
}
} // namespace recomp::pcache
