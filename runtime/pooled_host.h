#pragma once
// v240: the next GuestCooperativeExecutor::enqueue on this thread may run its task on a pre-started, parked host thread
// (the tile-scene IRQ workers: native_tile_scene_handoff.h). Defined in guest_cooperative_executor.cpp.
namespace recomp::bootstrap {
void enqueueNextOnPooledHost() noexcept;
struct PooledHostStats {unsigned long long pooled = 0, created = 0, fallbacks = 0, stopYields = 0;}; // (v241: stopYields)
PooledHostStats pooledHostStats() noexcept;
}
