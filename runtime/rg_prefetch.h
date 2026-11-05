#pragma once
// render-guest package (2026-10-04): host prefetch blocks for the render thread's per-draw D3D-layer guest code (chunk 18).
// v429 device profiles (render thread, drive + outdoor): the guest code there is load-latency bound; the most concentrated
// stalls are first touches of per-draw objects the game reaches by pointer chains and never prefetches:
//   T  parameter binding (823B8650 pixel / 823B8EC8 vertex; inlined into 823B63C8, 823B9230, 823B9668): per parameter
//      entry = E + idx*8; a sampler (entry byte 0 == 0) binds d = (*(entry[4]))[40] and SetTexture (823B8B70) then reads
//      d's fetch-constant words d[28..51]; a constant (byte 0 != 0) copies from entry[4]. One block at function entry walks
//      all parameters (at most cfg.params) and issues every chain at once (the loads of different parameters overlap),
//      then prefetches d[28], d[48] and the constant sources.
//   G  pass loop of 823B9668 (head 823B970C): before the pass's parameter binding (823B9230, long), the geometry object
//      g = (r26[4])[r29] that the pass then draws (vtable call -> 823B9888: g[0], g[55], g[68..75]) and its two buffer objects
//      b5 = g[12 + 4*(g[55]&1 ? G : 0)] (read at b5[4], b5[28]) and b6 = g[28 + 4*(g[55]&2 ? G : 0)] (read at b6[12]),
//      G = *(0x83BFAEB8) (823B9888's selection, recomputed here).
// rg_pass.py inserts the statements; rg_cfg.h's words (diag-rg-* files) switch them.
//
// Exactness (bit for bit), as auto-prefetch's ap_prefetch.h: each block is one opaque asm statement that
//   - reads guest memory only through zero-extended 32-bit offsets from base ([base, wN, uxtw]; words at wN & ~3, halfwords
//     at wN & ~1, bytes anywhere): the guest space is a 4 GiB PROT_READ|PROT_WRITE mapping, so every such load is in bounds
//     and cannot fault, whatever value it computes from (stale, garbage, null, past a list end);
//   - writes nothing but its own scratch registers (declared outputs, never used) and NZCV (declared "cc"): no guest
//     memory, no PPCContext field, no FPCR;
//   - prefetches with PRFM hints (never fault, no architectural state);
//   - has no memory clobber (the compiler may move guest loads/stores around it; that only changes which values the
//     prefetch addresses are computed from), and a bounded loop (T: at most cfg.params <= 16 iterations).
// PGO: the statement is straight-line IR (an inline-asm call: its branches are inside the asm), so the function's CFG
// hash is unchanged (checked per chunk with -Wbackend-plugin, rg_build.py). Off (word 0): adrp/add + ldr + cbz.
#include <cstdint>
#include "rg_cfg.h"

#if !defined(__aarch64__)
#error "rg_prefetch.h: AArch64 inline asm"
#endif

// Prefetch / recording hooks: the unit test (rg_unit_test.py) records the 32-bit guest address instead of prefetching.
#ifndef RECOMP_RG_PF
#define RECOMP_RG_PF(w) "prfm pldl1keep, [%[b], " w ", uxtw]\n\t"
#define RECOMP_RG_LOGOP
#define RECOMP_RG_CLOBBER
#endif
// Host test hook: counts executed statements per block (0 T, 1 G); empty on the device.
#ifndef RECOMP_RG_COUNT
#define RECOMP_RG_COUNT(n)
#endif

// dst = big-endian guest word at (addr & ~3); addr is clobbered (aligned in place); dst may equal addr.
#define RECOMP_RG_LD(dst, addr) \
    "and " addr ", " addr ", #0xfffffffc\n\t" "ldr " dst ", [%[b], " addr ", uxtw]\n\t" "rev " dst ", " dst "\n\t"

// T: OFF = the config word's byte offset ("0" pixel = cfg.ps, "4" vertex = cfg.vs), P3 = the binding object (E = P3[0]:
// parameter entries), P4 = the parameter list (count = halfword P4[12], index list X = P4[8]); the values the function
// received in r3 / r4.
#define RECOMP_RG_PARAMS(OFF, P3, P4) { \
    RECOMP_RG_COUNT(0) \
    std::uint32_t s_, n_, e_, x_, a_, t_, q_; \
    __asm__ __volatile__( \
        "ldr %w[s], [%[cfg], #" OFF "]\n\t" \
        "cbz %w[s], 9f\n\t" \
        "add %w[a], %w[p4], #12\n\t" "and %w[a], %w[a], #0xfffffffe\n\t" \
        "ldrh %w[n], [%[b], %w[a], uxtw]\n\t" "rev16 %w[n], %w[n]\n\t" \
        "cbz %w[n], 9f\n\t" \
        "cmp %w[n], %w[s]\n\t" "csel %w[n], %w[n], %w[s], ls\n\t" \
        "mov %w[a], %w[p3]\n\t" RECOMP_RG_LD("%w[e]", "%w[a]") \
        "add %w[a], %w[p4], #8\n\t" RECOMP_RG_LD("%w[x]", "%w[a]") \
        "8:\n\t" \
        "mov %w[a], %w[x]\n\t" RECOMP_RG_LD("%w[t]", "%w[a]") \
        "add %w[a], %w[e], %w[t], lsl #3\n\t" \
        "ldrb %w[t], [%[b], %w[a], uxtw]\n\t" \
        "add %w[q], %w[a], #4\n\t" RECOMP_RG_LD("%w[q]", "%w[q]") \
        "cbz %w[t], 2f\n\t" \
        RECOMP_RG_PF("%w[q]") "add %w[a], %w[q], #64\n\t" RECOMP_RG_PF("%w[a]") \
        "b 3f\n\t" \
        "2:\n\t" \
        "cbz %w[q], 3f\n\t" RECOMP_RG_LD("%w[q]", "%w[q]") \
        "cbz %w[q], 3f\n\t" "add %w[q], %w[q], #40\n\t" RECOMP_RG_LD("%w[q]", "%w[q]") \
        "cbz %w[q], 3f\n\t" \
        "add %w[a], %w[q], #28\n\t" RECOMP_RG_PF("%w[a]") "add %w[a], %w[q], #48\n\t" RECOMP_RG_PF("%w[a]") \
        "3:\n\t" \
        "add %w[x], %w[x], #4\n\t" "subs %w[n], %w[n], #1\n\t" "b.ne 8b\n\t" \
        "9:\n\t" \
        : [s] "=&r"(s_), [n] "=&r"(n_), [e] "=&r"(e_), [x] "=&r"(x_), [a] "=&r"(a_), [t] "=&r"(t_), [q] "=&r"(q_) \
          RECOMP_RG_LOGOP \
        : [b] "r"(base), [p3] "r"(P3), [p4] "r"(P4), [cfg] "r"(&recomp_rg_cfg) \
        : "cc" RECOMP_RG_CLOBBER); }

// G: 823B9668's pass-loop head (loc_823B970C): R26 = r26 (the drawable), R29 = r29 (its geometry index * 4).
#define RECOMP_RG_GEOM(R26, R29) { \
    RECOMP_RG_COUNT(1) \
    std::uint32_t s_, a_, g_, f_, c_, t_; \
    __asm__ __volatile__( \
        "ldr %w[s], [%[cfg], #8]\n\t" \
        "cbz %w[s], 9f\n\t" \
        "add %w[a], %w[r26], #4\n\t" RECOMP_RG_LD("%w[t]", "%w[a]") \
        "add %w[a], %w[t], %w[r29]\n\t" RECOMP_RG_LD("%w[g]", "%w[a]") \
        "cbz %w[g], 9f\n\t" \
        RECOMP_RG_PF("%w[g]") "add %w[a], %w[g], #38\n\t" RECOMP_RG_PF("%w[a]") \
        "add %w[a], %w[g], #75\n\t" RECOMP_RG_PF("%w[a]") \
        "add %w[a], %w[g], #55\n\t" "ldrb %w[f], [%[b], %w[a], uxtw]\n\t" \
        "mov %w[a], #0xaeb8\n\t" "movk %w[a], #0x83bf, lsl #16\n\t" RECOMP_RG_LD("%w[c]", "%w[a]") \
        "tst %w[f], #1\n\t" "csel %w[t], %w[c], wzr, ne\n\t" "add %w[t], %w[t], #3\n\t" \
        "add %w[a], %w[g], %w[t], lsl #2\n\t" RECOMP_RG_LD("%w[t]", "%w[a]") \
        "cbz %w[t], 1f\n\t" \
        "add %w[a], %w[t], #4\n\t" RECOMP_RG_PF("%w[a]") "add %w[a], %w[t], #28\n\t" RECOMP_RG_PF("%w[a]") \
        "1:\n\t" \
        "tst %w[f], #2\n\t" "csel %w[t], %w[c], wzr, ne\n\t" "add %w[t], %w[t], #7\n\t" \
        "add %w[a], %w[g], %w[t], lsl #2\n\t" RECOMP_RG_LD("%w[t]", "%w[a]") \
        "cbz %w[t], 9f\n\t" \
        "add %w[a], %w[t], #12\n\t" RECOMP_RG_PF("%w[a]") \
        "9:\n\t" \
        : [s] "=&r"(s_), [a] "=&r"(a_), [g] "=&r"(g_), [f] "=&r"(f_), [c] "=&r"(c_), [t] "=&r"(t_) RECOMP_RG_LOGOP \
        : [b] "r"(base), [r26] "r"(R26), [r29] "r"(R29), [cfg] "r"(&recomp_rg_cfg) \
        : "cc" RECOMP_RG_CLOBBER); }
