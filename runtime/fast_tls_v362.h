#pragma once
// v362 (exact): thread_local accesses of the hook layer without a TLS resolver call on one thread.
//
// This library is dlopen'ed, so every thread_local access is a TLSDESC call into bionic's tlsdesc_resolver_dynamic
// (the initial-exec model is refused for dlopen'ed libraries). The v349 render-thread profiles put that resolver at
// 1.6-1.9% of the render thread (outdoor, house), called from liveTask (currentTask + GuestExecutionGuard::isActive, both
// for every lock gate and every ownsNativeRendering admission check), the registry's per-view thread state, the draw
// hooks' own thread_locals (gate tick, owner counter, draw scratch, device-shadow cache, shader fallback pool, ...).
//
// Mechanism: ONE host thread (the render thread, claimed by the executor when its task starts) keeps, per thread_local
// variable, the variable's own address in a slot of fastThread that only that thread writes and reads. Its accesses go through
// that pointer: the same object as the thread_local access names, so every value, every state transition and every
// side effect is the thread_local's own; only the address computation differs. The identity test is the thread pointer
// register (one mrs) against the claimed identity. Any other thread, the claimed thread with the switch off and every
// thread before the claim or after the release take the ordinary thread_local access. Because the storage is the
// same either way, the switch may flip at any instant (even in the middle of a hook) without changing any value.
//
// Claim is once per process: the identity word goes 0 -> claimed thread -> Released (never claimed again). The claimed
// thread releases it from a thread_local destructor at its exit, before its thread control block (and so its thread
// pointer value) can be reused by a new thread; a later thread with the same thread pointer therefore never matches.
// The cached pointers stay valid for the claimed thread's whole life (its TLS block lives as long as the thread).
//
// Switch: diag-v362-render-trims (generic_renderer.cpp's switch poll), reported as v362_render_trims in RENDER_GENERIC_V210X.
#include <atomic>
#include <cstdint>
#include <pthread.h>

namespace recomp::v362 {

// The claimed thread's cached addresses, one slot per thread_local variable, in this object's own two cache lines (written once
// per slot by the claimed thread, then only read; no other data shares the lines, so the fast path's reads stay core-local).
enum class Slot : unsigned {
    CurrentTask, GuardActive, RegistryState, GateCalls, OwnerTick, DrawScratch, ShadowView, ShaderFallback, PendingInline,
    PreparedIndexed, ResolveDevice, TestA, TestB, TestC, Count
};
struct alignas(64) FastThread {
    std::atomic<std::uintptr_t> identity{0}; // 0 = not claimed yet, Released = claimed once and released, else the thread
    std::atomic<bool> on{false};             // diag-v362-render-trims (every v362 trim reads it)
    void* slots[unsigned(Slot::Count)]{};    // (the claimed thread's only; see tls())
};
static_assert(sizeof(FastThread) == 128, "two cache lines");
inline constexpr std::uintptr_t Released = 1; // (no thread pointer / pthread_t is 1)
[[gnu::visibility("hidden")]] inline FastThread fastThread;

[[gnu::always_inline]] inline std::uintptr_t threadIdentity() noexcept {
#if defined(__aarch64__) && defined(__ANDROID__)
    return reinterpret_cast<std::uintptr_t>(__builtin_thread_pointer()); // TPIDR_EL0: bionic's per-thread TLS block
#else
    return reinterpret_cast<std::uintptr_t>(pthread_self()); // host tests
#endif
}
[[gnu::always_inline]] inline bool trimsOn() noexcept { return fastThread.on.load(std::memory_order_relaxed); }
// True only on the claimed thread while the switch is on.
[[gnu::always_inline]] inline bool onFastThread() noexcept {
    return fastThread.on.load(std::memory_order_relaxed) &&
           fastThread.identity.load(std::memory_order_relaxed) == threadIdentity();
}
// The claimed thread's cached address in `slot` (nullptr until its first fast access). Only the claimed thread calls these.
[[gnu::always_inline]] inline void* slotAddress(Slot slot) noexcept { return fastThread.slots[unsigned(slot)]; }
[[gnu::always_inline]] inline void setSlotAddress(Slot slot, void* address) noexcept { fastThread.slots[unsigned(slot)] = address; }
// `slow` names the thread_local (returns a reference to it); `slot` is that variable's own slot (one variable per slot).
template <Slot S, class T, class F>
[[gnu::always_inline]] inline T& tls(F&& slow) noexcept {
#if defined(RECOMP_V362_MUTANT_SHARED_CACHE) // test mutant: the cached address answers on every thread (no identity test)
    if (trimsOn()) {
#else
    if (onFastThread()) {
#endif
        if (void* cached = slotAddress(S)) [[likely]] return *static_cast<T*>(cached);
        T& object = slow();
        setSlotAddress(S, const_cast<void*>(static_cast<const volatile void*>(&object)));
        return object;
    }
    return slow();
}

namespace detail {
struct ReleaseAtExit {
    bool armed = false;
    ~ReleaseAtExit() {
        if (!armed) return;
        auto expected = threadIdentity();
        fastThread.identity.compare_exchange_strong(expected, Released, std::memory_order_acq_rel);
    }
};
inline thread_local ReleaseAtExit releaseAtExit; // (touched only by claim(): its TLS guard is off the hot paths)
}
// Called by the thread that wants the fast path (the executor: the render task's host thread, before the task runs).
// Returns true when this thread holds the claim afterwards.
inline bool claim() noexcept {
    const auto self = threadIdentity();
    std::uintptr_t expected = 0;
    if (fastThread.identity.compare_exchange_strong(expected, self, std::memory_order_acq_rel)) {
        detail::releaseAtExit.armed = true;
        return true;
    }
    return expected == self;
}
inline bool claimedBySelf() noexcept { return fastThread.identity.load(std::memory_order_relaxed) == threadIdentity(); }

} // namespace recomp::v362
