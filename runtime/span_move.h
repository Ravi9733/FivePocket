#pragma once
// span-move (2026-10-04, USER-APPROVED non-exact change: WHEN span validation runs and what happens to a refused span; nothing
// else): files/private-trace/diag-sm-worker moves the async capture's per-draw span planning and validation (planOf, the
// validated-span caches, the vertex / texture addSpan calls) from the render thread to the Vulkan worker.
//
// The worker must never take the registry lock R: a submission handler drains the renderer queue while it holds R (v183's commit
// hook, guestReadBarrier; see AsyncFront::drainFor's v255 note), and the registry viewer adopts or rebuilds its snapshot under R at
// every generation change (~3-4 per frame on the device). So the worker does not call the viewer. Instead the render thread hands it
// the registry's immutable validation snapshot that is current AT THE DRAW (the generation word loaded at the capture shows no open
// mutation window and the snapshot's own generation: guest_registry_concurrency.h invariant 4, the fast path's own rule), and the
// worker evaluates the locked graphics predicate on that snapshot (snapshotGraphicsView, plus the v207 adjacent-allocation opt-in the
// locked path applies). A draw whose capture finds no such snapshot (window open, this thread's snapshot stale) is planned on the
// render thread exactly as v442 does (AsyncFront::capture, unchanged).
#include <atomic>
#include <cstdint>
#include <memory>

namespace recomp::gpu::generic {
struct SnapshotSource {
    // the registry's generation word (generation << 16 | open mutation windows); nullptr: no source (every draw planned inline)
    const std::atomic<std::uint64_t>* word=nullptr;
    // (render thread, under the capture lock) the calling thread's registry validation snapshot of exactly this word (no window
    // open, same generation), else empty. Never takes R.
    std::shared_ptr<const void> (*current)(std::uint64_t word) noexcept=nullptr;
    // (any thread, no lock) the registry's graphics-data predicate (viewSerializedGraphicsDataLocked's) evaluated on `snapshot`:
    // the in-place host bytes, or nullptr
    const std::uint8_t* (*view)(const void* snapshot,std::uint32_t address,std::uint32_t bytes) noexcept=nullptr;
};
} // namespace recomp::gpu::generic

// renderer_boundary_trace.cpp (a new function only): the source for the viewer's registry, asked for by the renderer (under its capture
// lock) at the first frame end with diag-sm-worker on; false while there is none. Weak in generic_renderer.cpp (renderer-only builds).
extern "C" bool recomp_sm_snapshot_source(recomp::gpu::generic::SnapshotSource* out) noexcept;

namespace recomp::bootstrap {
class GuestImportRegistry;
// guest_registry_concurrency.inc (span-move): the two halves of SnapshotSource for a registry.
std::shared_ptr<const void> registryGraphicsSnapshotFor(const GuestImportRegistry* registry,std::uint64_t word) noexcept;
const std::uint8_t* registrySnapshotGraphicsView(const void* snapshot,std::uint32_t address,std::uint32_t bytes) noexcept;
} // namespace recomp::bootstrap
