#pragma once
// main-code package (2026-10-03): run-time configuration of the guest dcbt/dcbtst prefetches (mc_dcbt_switch.h).
// Shared by the generated chunks (readers) and the runtime (generic_renderer.cpp: definition + the diag-mc-dcbt* poll).
// Modes: 0 off (both prefetches hit the PPCContext), 1 line (the 64-byte host line holding EA), 2 block (both 64-byte halves
// of the 128-byte Xenon block holding EA = what dcbt touches on the Xbox 360), 3 two (the line holding EA and the next one).
// Any mix of old and new words read during a change only selects another prefetch address: guest state is never affected.
#include <cstdint>

extern "C" {
struct RecompMcDcbtCfg {
    std::uint64_t mask;   // 0 = off (prefetch &ctx), ~0 = prefetch guest memory
    std::uint32_t align;  // EA mask: ~0 (line, two) or ~127 (block)
    std::uint32_t second; // offset of the second prefetch: 0 (off, line) or 64 (block, two)
};
extern RecompMcDcbtCfg recomp_mc_dcbt_cfg;
}

inline void recomp_mc_dcbt_set_mode(unsigned mode) noexcept {
    const std::uint64_t mask = mode >= 1 && mode <= 3 ? ~std::uint64_t{0} : 0;
    const std::uint32_t align = mode == 2 ? ~127u : ~0u;
    const std::uint32_t second = mode == 2 || mode == 3 ? 64u : 0u;
    __atomic_store_n(&recomp_mc_dcbt_cfg.align, align, __ATOMIC_RELAXED);
    __atomic_store_n(&recomp_mc_dcbt_cfg.second, second, __ATOMIC_RELAXED);
    __atomic_store_n(&recomp_mc_dcbt_cfg.mask, mask, __ATOMIC_RELAXED);
}
