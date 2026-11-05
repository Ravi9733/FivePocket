#pragma once
// auto-prefetch package (2026-10-03): software-pipelined host prefetches for pointer-chasing guest loops of main's frame
// (v429 device profiles: the stalls sit on dependent loads the game did not prefetch, or prefetched too late).
// ap_pass.py inserts one RECOMP_AP_* statement right after the loop-head label of each site (the back-edge target), so it
// runs once per iteration; ap_cfg.h's run-time words (diag-ap-* files) choose off / the item distance.
//
// Exactness (bit for bit): each block is one opaque asm statement that
//   - reads guest memory only through 4-byte-aligned, zero-extended 32-bit offsets from base ([base, wN, uxtw] with
//     wN & ~3): the guest space is a 4 GiB PROT_READ|PROT_WRITE mapping (guest_address_space.cpp, Size = 1 << 32; the write
//     watch only ever drops pages to PROT_READ; guest protections are recorded, never enforced with mprotect), so every
//     such load is in bounds and cannot fault, whatever value it computes from (stale, garbage, null, past the list end);
//   - writes nothing but its own scratch registers (declared outputs, never used): no guest memory, no PPCContext field,
//     no flags (add/and/rev/ldr/prfm/cbz leave NZCV alone; "cc" is listed anyway);
//   - uses PRFM hints, which never fault and change no architectural state;
//   - has no memory clobber: the compiler may schedule guest loads/stores around it freely (that only changes which values
//     the prefetch addresses are computed from, never guest semantics).
// So guest-visible state (registers, memory, CR/XER/FPSCR, call order) is identical in every mode for every input.
// PGO: the statement is straight-line IR (an inline-asm call: no branch, no select, no indirect call, no memory
// intrinsic), so the function's CFG hash is unchanged (checked per chunk with -Wbackend-plugin, ap_build.py).
// Off (word 0): adrp/add + ldr + cbz over the block.
#include <cstdint>
#include "ap_cfg.h"

#if !defined(__aarch64__)
#error "ap_prefetch.h: AArch64 inline asm"
#endif

// Prefetch / recording hooks: the unit test (ap_unit_test.py) records the 32-bit guest address instead of prefetching.
#ifndef RECOMP_AP_PF
#define RECOMP_AP_PF(kind, w) "prfm " kind ", [%[b], " w ", uxtw]\n\t"
#define RECOMP_AP_LOGOP
#define RECOMP_AP_CLOBBER
#endif
// Host test hook: counts executed statements per site (0 scene, 1 list); empty on the device.
#ifndef RECOMP_AP_COUNT
#define RECOMP_AP_COUNT(n)
#endif

// dst = big-endian guest word at (addr & ~3); addr is clobbered (aligned in place).
#define RECOMP_AP_LD(dst, addr) \
    "and " addr ", " addr ", #0xfffffffc\n\t" "ldr " dst ", [%[b], " addr ", uxtw]\n\t" "rev " dst ", " dst "\n\t"

// A: scene-item loop of 8239E320 (loop 8239E380..8239E58C; called from 8239DF60, main's scene-item consumer).
// Items are 16 bytes at r29 (item[0] = obj). Per item the game reads obj[40] = T (dcbt'd only ~40 instructions before
// use), then in 8239E598: U = T[8], V = U[12] (U[8] is stored), table[V * 8 + 4] with table = *(0x83C90474); in 8239E200:
// X = obj[16], X[0] (vtable). The game's own dcbt covers obj(i+2) only. Stages with step s (16 or 32 bytes = 1 or 2 items):
//   1: obj(i+4s) lines [0], [16], [40]     2: T(i+3s)+8 and X(i+3s)    3: U(i+2s)+8 (for store)
//   4: table[V(i+s) * 8 + 4]
// Each stage's loads read lines the previous stage prefetched one step earlier.
#define RECOMP_AP_SCENE_8239E380() { \
    RECOMP_AP_COUNT(0) \
    std::uint32_t s_, a_, o_, t_, u_; \
    __asm__ __volatile__( \
        "ldr %w[s], [%[cfg], #0]\n\t" \
        "cbz %w[s], 1f\n\t" \
        "add %w[a], %w[it], %w[s], lsl #2\n\t" RECOMP_AP_LD("%w[o]", "%w[a]") \
        RECOMP_AP_PF("pldl1keep", "%w[o]") \
        "add %w[a], %w[o], #16\n\t" RECOMP_AP_PF("pldl1keep", "%w[a]") \
        "add %w[a], %w[o], #40\n\t" RECOMP_AP_PF("pldl1keep", "%w[a]") \
        "add %w[a], %w[s], %w[s], lsl #1\n\t" "add %w[a], %w[it], %w[a]\n\t" RECOMP_AP_LD("%w[o]", "%w[a]") \
        "add %w[a], %w[o], #40\n\t" RECOMP_AP_LD("%w[t]", "%w[a]") \
        "add %w[a], %w[t], #8\n\t" RECOMP_AP_PF("pldl1keep", "%w[a]") \
        "add %w[a], %w[o], #16\n\t" RECOMP_AP_LD("%w[t]", "%w[a]") \
        RECOMP_AP_PF("pldl1keep", "%w[t]") \
        "add %w[a], %w[it], %w[s], lsl #1\n\t" RECOMP_AP_LD("%w[o]", "%w[a]") \
        "add %w[a], %w[o], #40\n\t" RECOMP_AP_LD("%w[t]", "%w[a]") \
        "add %w[a], %w[t], #8\n\t" RECOMP_AP_LD("%w[u]", "%w[a]") \
        "add %w[a], %w[u], #8\n\t" RECOMP_AP_PF("pstl1keep", "%w[a]") \
        "add %w[a], %w[it], %w[s]\n\t" RECOMP_AP_LD("%w[o]", "%w[a]") \
        "add %w[a], %w[o], #40\n\t" RECOMP_AP_LD("%w[t]", "%w[a]") \
        "add %w[a], %w[t], #8\n\t" RECOMP_AP_LD("%w[u]", "%w[a]") \
        "add %w[a], %w[u], #12\n\t" RECOMP_AP_LD("%w[u]", "%w[a]") \
        "mov %w[a], #0x0474\n\t" "movk %w[a], #0x83c9, lsl #16\n\t" RECOMP_AP_LD("%w[t]", "%w[a]") \
        "add %w[a], %w[t], %w[u], lsl #3\n\t" "add %w[a], %w[a], #4\n\t" RECOMP_AP_PF("pldl1keep", "%w[a]") \
        "1:\n\t" \
        : [s] "=&r"(s_), [a] "=&r"(a_), [o] "=&r"(o_), [t] "=&r"(t_), [u] "=&r"(u_) RECOMP_AP_LOGOP \
        : [b] "r"(base), [it] "r"(ctx.r29.u32), [cfg] "r"(&recomp_ap_cfg) \
        : "cc" RECOMP_AP_CLOBBER); }

// B: entry loop of 823AE430 (loop 823AE520..823AE6BC, head label 823AE524 = every iteration incl. the first).
// Entries are 16 bytes at E + r26 (E = *(r31 + 16)); entry[0] = obj; Q = obj[40] (game dcbt'd just before use);
// Q[0] = vtable, vtable[8] = the virtual called per entry. The game's dcbt covers obj(i+3). Stages with step s:
//   1: obj(i+4s) line [40]     2: Q(i+2s) line [0]     3: vtable(i+s) line [8]
#define RECOMP_AP_LIST_823AE524() { \
    RECOMP_AP_COUNT(1) \
    std::uint32_t s_, a_, e_, o_, t_; \
    __asm__ __volatile__( \
        "ldr %w[s], [%[cfg], #4]\n\t" \
        "cbz %w[s], 1f\n\t" \
        "add %w[a], %w[d], #16\n\t" RECOMP_AP_LD("%w[e]", "%w[a]") \
        "add %w[e], %w[e], %w[off]\n\t" \
        "add %w[a], %w[e], %w[s], lsl #2\n\t" RECOMP_AP_LD("%w[o]", "%w[a]") \
        "add %w[a], %w[o], #40\n\t" RECOMP_AP_PF("pldl1keep", "%w[a]") \
        "add %w[a], %w[e], %w[s], lsl #1\n\t" RECOMP_AP_LD("%w[o]", "%w[a]") \
        "add %w[a], %w[o], #40\n\t" RECOMP_AP_LD("%w[t]", "%w[a]") \
        RECOMP_AP_PF("pldl1keep", "%w[t]") \
        "add %w[a], %w[e], %w[s]\n\t" RECOMP_AP_LD("%w[o]", "%w[a]") \
        "add %w[a], %w[o], #40\n\t" RECOMP_AP_LD("%w[t]", "%w[a]") \
        RECOMP_AP_LD("%w[o]", "%w[t]") \
        "add %w[a], %w[o], #8\n\t" RECOMP_AP_PF("pldl1keep", "%w[a]") \
        "1:\n\t" \
        : [s] "=&r"(s_), [a] "=&r"(a_), [e] "=&r"(e_), [o] "=&r"(o_), [t] "=&r"(t_) RECOMP_AP_LOGOP \
        : [b] "r"(base), [d] "r"(ctx.r31.u32), [off] "r"(ctx.r26.u32), [cfg] "r"(&recomp_ap_cfg) \
        : "cc" RECOMP_AP_CLOBBER); }

// Considered and dropped (2026-10-03): the vcall list loops of 82390690 (head 8239070C), 8239B7D8 (8239B820) and 823983A0
// (823983EC): <= 0.04 ms per driving frame together (est., v429 drive samples on their object loads), and 82390690's index
// list never terminates on the host harness's random memory, so its body could not be validated there.
