#pragma once
// auto-prefetch package (2026-10-03): run-time configuration of the inserted host prefetch chains (ap_prefetch.h in the
// generated chunks). Shared by the chunks (readers, inside their asm blocks) and the runtime (generic_renderer.cpp:
// definition + the diag-ap-* poll at the 60-present window boundary). Each word is the item step in bytes of one site
// (0 = off: the asm block branches over everything). A torn or stale read only selects another prefetch distance: the
// blocks never write guest-visible state.
#include <cstdint>

extern "C" {
struct alignas(64) RecompApCfg {
    std::uint32_t scene;  // 8239E320 scene-item loop (16-byte items): 16 (diag-ap-scene) or 32 (diag-ap-scene2), 0 off
    std::uint32_t list;   // 823AE430 entry loop (16-byte entries): 16 (diag-ap-list) or 32 (diag-ap-list2), 0 off
    std::uint32_t pad[14];
};
// hidden: defined in the same library (generic_renderer.cpp), so the chunks address it with adrp+add instead of a GOT load
__attribute__((visibility("hidden"))) extern RecompApCfg recomp_ap_cfg;
}

// Mode -> words: 0 off, 1 = one-item stage spacing, 2 = two-item spacing.
inline void recomp_ap_set(unsigned scene, unsigned list) noexcept {
    __atomic_store_n(&recomp_ap_cfg.scene, 16u * scene, __ATOMIC_RELAXED);
    __atomic_store_n(&recomp_ap_cfg.list, 16u * list, __ATOMIC_RELAXED);
}
