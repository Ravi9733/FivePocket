#pragma once
// render-guest package (2026-10-04): run-time configuration of the host prefetch blocks rg_prefetch.h inserts into the render
// thread's D3D-layer guest functions (generated chunk 18). Shared by the chunk (readers, inside the asm blocks) and the
// runtime (generic_renderer.cpp: the definition + the diag-rg-* poll at the 60-present window boundary). Each word is read
// once per block execution; 0 = off (the block branches over everything). A torn or stale read only switches a block on or
// off for one execution: the blocks never write guest-visible state.
#include <cstdint>

extern "C" {
struct alignas(64) RecompRgCfg {
    std::uint32_t ps;    // 823B8650 pixel-parameter binding: max parameters walked (16 = diag-rg-tex), 0 off
    std::uint32_t vs;    // 823B8EC8 vertex-parameter binding: max parameters walked (16 = diag-rg-vs), 0 off
    std::uint32_t geom;  // 823B9668 pass loop: geometry object + its two buffer objects (1 = diag-rg-geom), 0 off
    std::uint32_t pad[13];
};
// hidden: defined in the same library (generic_renderer.cpp), so the chunk addresses it with adrp+add instead of a GOT load
__attribute__((visibility("hidden"))) extern RecompRgCfg recomp_rg_cfg;
}

// files -> words (each 0 / 1).
inline void recomp_rg_set(unsigned ps, unsigned vs, unsigned geom) noexcept {
    __atomic_store_n(&recomp_rg_cfg.ps, ps ? 16u : 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&recomp_rg_cfg.vs, vs ? 16u : 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&recomp_rg_cfg.geom, geom ? 1u : 0u, __ATOMIC_RELAXED);
}
