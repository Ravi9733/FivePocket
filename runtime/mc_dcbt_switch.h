#pragma once
// main-code package (2026-10-03): guest dcbt / dcbtst (data cache block touch / touch for store) as host prefetches,
// switchable at run time (diag-mc-dcbt*). XenonRecomp emits these instructions as comments only; dcbt_pass.py adds
// PPC_MC_DCBT / PPC_MC_DCBTST statements after each such comment.
// Exactness: on PowerPC dcbt/dcbtst are performance hints with no architectural effect (no register or memory change, no
// exception even for an invalid address). AArch64 PRFM is a hint too: it never generates a synchronous abort and changes no
// architectural state (registers, memory, flags). The statements below read only the configuration words and change no
// guest-visible state, so guest semantics are bit-for-bit unchanged for ANY prefetched address (also while the runtime
// rewrites the configuration concurrently: a torn read only picks a different, harmless prefetch address).
// PGO: straight-line code (no branch, no select, no indirect call, no memory intrinsic) -> the functions' CFG hashes are
// unchanged (checked per chunk with -Wbackend-plugin by dcbt_build.py; sites in functions whose hash still changed
// are excluded there).
// Modes (recomp_mc_dcbt_cfg, set by the runtime):
//   off      mask 0,  align ~0,   second 0  -> both prefetches hit the PPCContext (in L1 anyway)
//   line     mask ~0, align ~0,   second 0  -> the 64-byte host line holding EA (twice)
//   block    mask ~0, align ~127, second 64 -> both 64-byte halves of the 128-byte Xenon block holding EA
//   two      mask ~0, align ~0,   second 64 -> the line holding EA and the next line
#include <cstdint>
#include "mc_dcbt_cfg.h"

template <int Write>
static inline __attribute__((always_inline)) void ppc_mc_dcbt_(const PPCContext& ctx, const std::uint8_t* base,
                                                               std::uint32_t ea) {
    const std::uint32_t e = ea & __atomic_load_n(&recomp_mc_dcbt_cfg.align, __ATOMIC_RELAXED);
    const std::uintptr_t m = __atomic_load_n(&recomp_mc_dcbt_cfg.mask, __ATOMIC_RELAXED);
    const std::uintptr_t p = ((reinterpret_cast<std::uintptr_t>(base) + e) & m) |
                             (reinterpret_cast<std::uintptr_t>(&ctx) & ~m);
    const std::uintptr_t q = p + __atomic_load_n(&recomp_mc_dcbt_cfg.second, __ATOMIC_RELAXED);
#if defined(RECOMP_MC_DCBT_TEST_HOOK)
    RECOMP_MC_DCBT_TEST_HOOK(p, q, Write);
#endif
    __builtin_prefetch(reinterpret_cast<const void*>(p), Write, 3);
    __builtin_prefetch(reinterpret_cast<const void*>(q), Write, 3);
}

#define PPC_MC_DCBT(ea) ppc_mc_dcbt_<0>(ctx, base, static_cast<std::uint32_t>(ea))
#define PPC_MC_DCBTST(ea) ppc_mc_dcbt_<1>(ctx, base, static_cast<std::uint32_t>(ea))
