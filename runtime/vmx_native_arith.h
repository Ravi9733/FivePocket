// vmx_native_arith.h (package vmx-arith, 2026-10-02): exact native NEON forms of the SIMDe vector-FP operations that the
// NDK clang SCALARIZES under -ffp-model=strict. A vector FP compare is a constrained fcmps on <4 x float> there, which the
// AArch64 backend unrolls into four scalar fcmp(e) + csetm + lane extracts/inserts (~21 instructions instead of one).
// tools/gta5_codegen/vmx_native_arith.py rewrites the generated calls simde_mm_cmpgt_ps( ... to ppc_native_cmpgt_ps( ...;
// every function here returns the same bits as the SIMDe call it replaces, for every input and FPCR state (host test:
// tools/gta5_codegen/vmx_native_arith_test.cpp).
//
//  replaced call               SIMDe on AArch64 (no SIMDE_FAST_NANS / _CONVERSION_RANGE)        native form
//  simde_mm_cmpgt_ps(a, b)     vcgtq_f32(a, b)                                                  fcmgt a, b
//  simde_mm_cmpge_ps(a, b)     vcgeq_f32(a, b)                                                  fcmge a, b
//  simde_mm_cmpeq_ps(a, b)     vceqq_f32(a, b)                                                  fcmeq a, b
//  simde_mm_cmplt_ps(a, b)     vcltq_f32(a, b)                                                  fcmgt b, a
//  simde_mm_max_ps(a, b)       vbslq_f32(vcgtq_f32(a, b), a, b)                                 fcmgt a, b; bsl
//  simde_mm_min_ps(a, b)       (a & (a < b)) | (b & ~(a < b))                                   fcmgt b, a; bsl
//  simde_mm_cmpunord_ps(a, b)  ~(vceqq_f32(a, a) & vceqq_f32(b, b))                             fcmeq x2; and; mvn
//  simde_mm_vctsxs(x)          ppc_context.h: cvttps (fcvtzs + range/NaN fix-up to INT_MIN),    fcvtzs (saturating,
//                              INT_MIN -> INT_MAX for non-negative inputs, NaN -> 0             NaN -> 0, truncating)
//  simde_mm_vctuxs(x)          ppc_context.h: max(x, 0), 2^31 split, cvttps, re-add, saturate   fcvtzu (saturating,
//                                                                                               NaN -> 0, truncating)
// FPCR: the compares honour FPCR.FZ exactly as the scalar fcmp/fcmpe of the current code do (both flush single-precision
// denormal inputs when FZ = 1; FPCR.AH is never set). fcvtzs/fcvtzu truncate regardless of FPCR.RMode, and a denormal
// truncates to 0 with or without FZ, so the conversions do not depend on FPCR at all.
// Ordering: the compares depend on FPCR.FZ, which the generated code changes with `msr fpcr` (ppc_context.h setcsr(), a
// volatile asm without a memory clobber, i.e. "may read/write any memory" for clang/LLVM) when it switches between VMX and
// FPU instructions, and which any call may change. The original constrained compares are never moved across such a point.
// PPC_NATIVE_ARITH_ORDER selects how the native compare is kept in place:
//   1 (default, the exact pass): a plain asm with an unused memory INPUT operand, the hidden global ppc_native_fpcr_token.
//     The asm then only reads memory: it cannot be moved across, CSE'd across or hoisted out of a loop past anything that
//     may write memory - every msr fpcr asm and every call - yet it is not a barrier for the ctx loads/stores around it.
//   2: volatile asm (also exact; an implicit memory barrier: the compiler reloads/re-stores ctx registers around it).
//   0: plain asm without the token (DIAGNOSTIC ONLY, not exact in general: could be hoisted across an FPCR write).
// The FPCR-independent conversions are plain asm. Host FPSR exception flags differ (unobservable: nothing reads FPSR).
// On a non-AArch64 host the functions fall back to the SIMDe calls themselves.
#pragma once
#include "ppc_context.h"

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>

#ifndef PPC_NATIVE_ARITH_ORDER
#define PPC_NATIVE_ARITH_ORDER 1
#endif

// a byte nothing writes or reads: its address only ties the compares to the "may write memory" points (see above)
inline char ppc_native_fpcr_token;

#if PPC_NATIVE_ARITH_ORDER == 1
#define PPC_NATIVE_CMP(insn, r, a, b) __asm__(insn " %0.4s, %1.4s, %2.4s" : "=w"(r) : "w"(a), "w"(b), "m"(ppc_native_fpcr_token))
#elif PPC_NATIVE_ARITH_ORDER == 2
#define PPC_NATIVE_CMP(insn, r, a, b) __asm__ __volatile__(insn " %0.4s, %1.4s, %2.4s" : "=w"(r) : "w"(a), "w"(b))
#else
#define PPC_NATIVE_CMP(insn, r, a, b) __asm__(insn " %0.4s, %1.4s, %2.4s" : "=w"(r) : "w"(a), "w"(b))
#endif

#define PPC_NATIVE_INLINE static inline __attribute__((always_inline))

PPC_NATIVE_INLINE uint32x4_t ppc_native_fcmgt(float32x4_t a, float32x4_t b)
{
    uint32x4_t r;
    PPC_NATIVE_CMP("fcmgt", r, a, b);
    return r;
}

PPC_NATIVE_INLINE uint32x4_t ppc_native_fcmge(float32x4_t a, float32x4_t b)
{
    uint32x4_t r;
    PPC_NATIVE_CMP("fcmge", r, a, b);
    return r;
}

PPC_NATIVE_INLINE uint32x4_t ppc_native_fcmeq(float32x4_t a, float32x4_t b)
{
    uint32x4_t r;
    PPC_NATIVE_CMP("fcmeq", r, a, b);
    return r;
}

PPC_NATIVE_INLINE simde__m128 ppc_native_cmpgt_ps(simde__m128 a, simde__m128 b)
{
    return vreinterpretq_f32_u32(ppc_native_fcmgt(a, b));
}

PPC_NATIVE_INLINE simde__m128 ppc_native_cmpge_ps(simde__m128 a, simde__m128 b)
{
    return vreinterpretq_f32_u32(ppc_native_fcmge(a, b));
}

PPC_NATIVE_INLINE simde__m128 ppc_native_cmpeq_ps(simde__m128 a, simde__m128 b)
{
    return vreinterpretq_f32_u32(ppc_native_fcmeq(a, b));
}

PPC_NATIVE_INLINE simde__m128 ppc_native_cmplt_ps(simde__m128 a, simde__m128 b)
{
    return vreinterpretq_f32_u32(ppc_native_fcmgt(b, a));
}

PPC_NATIVE_INLINE simde__m128 ppc_native_max_ps(simde__m128 a, simde__m128 b)
{
    return vbslq_f32(ppc_native_fcmgt(a, b), a, b);
}

PPC_NATIVE_INLINE simde__m128 ppc_native_min_ps(simde__m128 a, simde__m128 b)
{
    return vbslq_f32(ppc_native_fcmgt(b, a), a, b);
}

PPC_NATIVE_INLINE simde__m128 ppc_native_cmpunord_ps(simde__m128 a, simde__m128 b)
{
    return vreinterpretq_f32_u32(vmvnq_u32(vandq_u32(ppc_native_fcmeq(a, a), ppc_native_fcmeq(b, b))));
}

PPC_NATIVE_INLINE simde__m128i ppc_native_vctsxs(simde__m128 x)
{
    int32x4_t r;
    __asm__("fcvtzs %0.4s, %1.4s" : "=w"(r) : "w"(x));
    return vreinterpretq_s64_s32(r);
}

PPC_NATIVE_INLINE simde__m128i ppc_native_vctuxs(simde__m128 x)
{
    uint32x4_t r;
    __asm__("fcvtzu %0.4s, %1.4s" : "=w"(r) : "w"(x));
    return vreinterpretq_s64_u32(r);
}

#undef PPC_NATIVE_INLINE
#else
#define ppc_native_cmpgt_ps simde_mm_cmpgt_ps
#define ppc_native_cmpge_ps simde_mm_cmpge_ps
#define ppc_native_cmpeq_ps simde_mm_cmpeq_ps
#define ppc_native_cmplt_ps simde_mm_cmplt_ps
#define ppc_native_max_ps simde_mm_max_ps
#define ppc_native_min_ps simde_mm_min_ps
#define ppc_native_cmpunord_ps simde_mm_cmpunord_ps
#define ppc_native_vctsxs simde_mm_vctsxs
#define ppc_native_vctuxs simde_mm_vctuxs
#endif
