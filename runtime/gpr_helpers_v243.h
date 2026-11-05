#pragma once

// v243: the GPR save/restore helpers (__savegprlr_14..31 / __restgprlr_14..31) check their whole stack block once instead of
// every access. With RECOMP_GUEST_SPECIAL_CONST_WINDOWS the per-access fast predicate is "[address, last] meets none of the
// three windows"; the helper's accesses tile [r1 - 8*(32-N) - 8, r1 - 4) and the block is far smaller than the gaps between
// the windows, so the block test equals every access's test. When it holds and r1 is 8-aligned, each access is the same
// storeGuestScalarRaw / loadGuestScalarRaw (same alignment rule, memory order, value, address and order) the per-access
// fast path performs; otherwise the generated code below runs unchanged. v239 outdoor profile: the helpers 4.4% of the
// render thread, 6.1% of main, 3.7% of audio, 10.2% of 64249000. A/B: diag-v243-baseline (recomp_v243_baseline).
#include <atomic>
#include <cstdint>
#if !(defined(RECOMP_GUEST_SPECIAL_CONST_WINDOWS) && RECOMP_GUEST_SPECIAL_CONST_WINDOWS && defined(RECOMP_GUEST_SPECIAL_CONST_WINDOWS_TREE) && RECOMP_GUEST_SPECIAL_CONST_WINDOWS_TREE)
#error "v243: the block test equals the per-access test only with the constant-window tree predicate"
#endif
extern "C" __attribute__((weak)) std::atomic<bool> recomp_v243_baseline;
namespace recomp_v243 {
inline bool on() { return !(&recomp_v243_baseline && recomp_v243_baseline.load(std::memory_order_relaxed)); }
inline bool fastBlock(std::uint32_t address, std::uint32_t bytes) { // v229's tree predicate for a block of `bytes`
    const std::uint64_t last = std::uint64_t{address} + bytes - 1;
    using namespace recomp::bootstrap;
    if (last < 0x7FC80000u) return last < guestSpecialWindowB || address >= guestSpecialWindowB + guestSpecialWindowBytes;
    if (address < 0x7FEB0000u) return false;
    return last < guestSpecialWindowA || (address >= guestSpecialWindowA + guestSpecialWindowBytes && last <= 0xFFFFFFFFu);
}
inline constexpr PPCRegister PPCContext::* Gpr[18] = {&PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17,
    &PPCContext::r18, &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23, &PPCContext::r24,
    &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28, &PPCContext::r29, &PPCContext::r30, &PPCContext::r31};
template<int N> inline bool save(PPCContext& ctx, std::uint8_t* base) {
    const std::uint32_t sp = ctx.r1.u32, bytes = 8u * (32 - N) + 4u, lo = sp - (8u * (32 - N) + 8u);
    if (!on() || (sp & 7u) || !fastBlock(lo, bytes)) return false;
    for (int k = N; k <= 31; ++k)
#if defined(V243_MUTANT_OFFSET) // test mutant: every register one slot off
        recomp::bootstrap::storeGuestScalarRaw<std::uint64_t>(base + lo + 8u * (k - N) + 8u, (ctx.*Gpr[k - 14]).u64);
#else
        recomp::bootstrap::storeGuestScalarRaw<std::uint64_t>(base + lo + 8u * (k - N), (ctx.*Gpr[k - 14]).u64);
#endif
    recomp::bootstrap::storeGuestScalarRaw<std::uint32_t>(base + sp - 8u, ctx.r12.u32);
    return true;
}
template<int N> inline bool restore(PPCContext& ctx, std::uint8_t* base) {
    const std::uint32_t sp = ctx.r1.u32, bytes = 8u * (32 - N) + 4u, lo = sp - (8u * (32 - N) + 8u);
    if (!on() || (sp & 7u) || !fastBlock(lo, bytes)) return false;
    for (int k = N; k <= 31; ++k)
        (ctx.*Gpr[k - 14]).u64 = recomp::bootstrap::loadGuestScalarRaw<std::uint64_t>(base + lo + 8u * (k - N));
    ctx.r12.u64 = recomp::bootstrap::loadGuestScalarRaw<std::uint32_t>(base + sp - 8u);
    ctx.lr = ctx.r12.u64;
    return true;
}
}
