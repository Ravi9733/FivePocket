#pragma once
// imports-fast (agent package F "imports", 2026-10-03): exact cuts on the guest import path, behind the in-run switch
// files/private-trace/diag-imports-fast -> extern "C" std::atomic<bool> recomp_imports_fast (strong definition in
// guest_import_registry.cpp; the renderer's window poll sets it; default off = v414 behaviour). Guest-visible state,
// return values, wake and wait outcomes are unchanged in both settings. With the switch on:
//  A. The hot imports' dispatch section is a RegistryUniqueQuiet (guest_registry_concurrency.h): the same R acquisition
//     and windows without the hold accounting (two cntvct reads and the per-site batch per section), which feeds only the
//     5 s RENDER_DIAG_REGISTRY_HOLD* lines (contended waits are still accounted: registryLockContended is unchanged).
//     Existing section code is untouched (it keeps its PGO profile: the renderer's validated views inline it).
//  B. The saves gate (guest_saves_dispatch.inc: savesStateFor's static mutex + std::map lookup, under R) is skipped for
//     the hot N-imports, which that include never handles, whenever the call could not create the saves state (not
//     eligible, or already created for this registry): for them the gate then has no effect at all.
//  C. The v160 stack diagnostic under R (findThreadObject, two maps, logs) is skipped for the hot imports (diagnostic).
//  D. Per-thread node caches in front of criticalSections_.find (the v197 CS fast path, under the address's stripe lock)
//     and kernelHandles_.find (event / semaphore / wait handlers, under R). A cached iterator is used only while its key's
//     erase generation is unchanged. EVERY erase / clear of the two maps goes through eraseKey / eraseIterator / clearAll
//     below (tools/gta5_codegen/imports/source_check.py enforces it), always, whatever the switch: they bump the
//     generation of the erased key's bucket (clear: all buckets) while the caller holds the map's lock (R for
//     kernelHandles_; CsAll = all stripes for criticalSections_). Argument: std::map iterators stay valid until their
//     own element is erased (inserts and value writes never invalidate them; keys are unique and const). A reader holds
//     the lock the eraser held, so any erase of key k that precedes the reader's acquisition happened-before it and its
//     bump is visible; none can run concurrently. Hence "entry (map, k, g) and bucket(k) still reads g" implies the
//     cached iterator is the live element of key k, i.e. exactly map.find(k). A destroyed registry's map cannot be hit:
//     a registry at a reused address dispatches only after initialize(), which clears both maps through clearAll.
//     Generations are 64-bit (no wrap). Misses (and the switch off) run map.find as before; only hits skip the walk.
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

extern "C" std::atomic<bool> recomp_imports_fast;

namespace recomp::imports_fast {

inline bool on() noexcept { return ::recomp_imports_fast.load(std::memory_order_relaxed); }

// ---- hot imports (by the v393 driving profile: >99% of the import-path samples on main and the job workers)
enum Hot : unsigned {
    HotNone = 0, HotNtWait = 1u << 0, HotNtSet = 1u << 1, HotNtRelease = 1u << 2, HotNtClear = 1u << 3,
    HotCsEnter = 1u << 4, HotCsLeave = 1u << 5, HotTlsGet = 1u << 6, HotDelay = 1u << 7,
    HotHandleImports = HotNtWait | HotNtSet | HotNtRelease | HotNtClear, // the N-imports: the saves include handles none
};
inline unsigned hotImport(std::string_view n) noexcept {
    switch (n.size()) {
    case 17: return n == "__imp__NtSetEvent" ? HotNtSet : HotNone;
    case 19: return n == "__imp__NtClearEvent" ? HotNtClear : HotNone;
    case 20: return n == "__imp__KeTlsGetValue" ? HotTlsGet : HotNone;
    case 25: return n == "__imp__NtReleaseSemaphore" ? HotNtRelease : HotNone;
    case 29: return n == "__imp__KeDelayExecutionThread" ? HotDelay : HotNone;
    case 30: return n == "__imp__NtWaitForSingleObjectEx" ? HotNtWait
                  : n == "__imp__RtlEnterCriticalSection" ? HotCsEnter
                  : n == "__imp__RtlLeaveCriticalSection" ? HotCsLeave : HotNone;
    default: return HotNone;
    }
}
static_assert(std::string_view("__imp__NtSetEvent").size() == 17 && std::string_view("__imp__NtClearEvent").size() == 19 &&
              std::string_view("__imp__KeTlsGetValue").size() == 20 && std::string_view("__imp__NtReleaseSemaphore").size() == 25 &&
              std::string_view("__imp__KeDelayExecutionThread").size() == 29 &&
              std::string_view("__imp__NtWaitForSingleObjectEx").size() == 30 &&
              std::string_view("__imp__RtlEnterCriticalSection").size() == 30 &&
              std::string_view("__imp__RtlLeaveCriticalSection").size() == 30);

// ---- erase generations and node caches (D)
constexpr unsigned BucketBits = 8, Buckets = 1u << BucketBits, CacheSlots = 32;
struct EraseGenerations { std::array<std::atomic<std::uint64_t>, Buckets> bucket{}; };
inline unsigned bucketOf(std::uint32_t key) noexcept { return ((key >> 2) * 0x9E3779B1u) >> (32 - BucketBits); }
inline unsigned slotOf(std::uint32_t key) noexcept { return ((key >> 2) * 0x85EBCA77u) >> 27; } // 32 slots
static_assert(CacheSlots == 32);
inline EraseGenerations& handleGenerations() noexcept { static EraseGenerations g; return g; }
inline EraseGenerations& csGenerations() noexcept { static EraseGenerations g; return g; }

struct CacheEntry { const void* map; std::uint64_t generation; std::uint64_t iterator; std::uint32_t key; };
struct NodeCache { CacheEntry entries[CacheSlots]; };
static_assert(std::is_trivially_default_constructible_v<NodeCache> && std::is_trivially_destructible_v<NodeCache>);
// Trivial thread_locals: one TLS access each, no guard (constant zero initialization).
inline NodeCache& handleCache() noexcept { static thread_local NodeCache c; return c; }
inline NodeCache& csCache() noexcept { static thread_local NodeCache c; return c; }

// Counters for the host tests (relaxed).
inline std::atomic<std::uint64_t>& cacheHits() noexcept { static std::atomic<std::uint64_t> n{0}; return n; }
inline std::atomic<std::uint64_t>& cacheMisses() noexcept { static std::atomic<std::uint64_t> n{0}; return n; }

// The caller holds the map's lock.
template<class Map>
typename Map::iterator find(Map& map, std::uint32_t key, EraseGenerations& generations, NodeCache& cache) {
    using It = typename Map::iterator;
    static_assert(sizeof(It) == sizeof(std::uint64_t) && std::is_trivially_copyable_v<It>);
    auto& e = cache.entries[slotOf(key)];
    const auto generation = generations.bucket[bucketOf(key)].load(std::memory_order_acquire);
#if !defined(RECOMP_IMPORTS_FAST_MUTANT_NO_GENERATION_CHECK)
    const bool current = e.generation == generation;
#else
    const bool current = true; // (mutant: a cached iterator survives the erase of its element)
#endif
#if !defined(RECOMP_IMPORTS_FAST_MUTANT_NO_KEY_CHECK)
    if (e.map == &map && e.key == key && current) {
#else
    if (e.map == &map && current) { // (mutant: any key of the slot)
#endif
        It it;
        std::memcpy(&it, &e.iterator, sizeof it);
#if defined(RECOMP_IMPORTS_FAST_COUNTERS)
        cacheHits().fetch_add(1, std::memory_order_relaxed);
#endif
        return it;
    }
    const auto it = map.find(key);
    if (it != map.end()) {
        e.map = &map; e.key = key; e.generation = generation;
        std::memcpy(&e.iterator, &it, sizeof it);
    }
#if defined(RECOMP_IMPORTS_FAST_COUNTERS)
    cacheMisses().fetch_add(1, std::memory_order_relaxed);
#endif
    return it;
}

// Erase / clear (always, switch on or off). The bump precedes the erase; both happen under the map's lock.
template<class Map>
auto eraseKey(Map& map, std::uint32_t key, EraseGenerations& generations) {
#if !defined(RECOMP_IMPORTS_FAST_MUTANT_NO_BUMP)
    generations.bucket[bucketOf(key)].fetch_add(1, std::memory_order_acq_rel);
#endif
    return map.erase(key);
}
template<class Map>
typename Map::iterator eraseIterator(Map& map, typename Map::iterator it, EraseGenerations& generations) {
#if !defined(RECOMP_IMPORTS_FAST_MUTANT_NO_BUMP)
    generations.bucket[bucketOf(it->first)].fetch_add(1, std::memory_order_acq_rel);
#endif
    return map.erase(it);
}
template<class Map>
void clearAll(Map& map, EraseGenerations& generations) {
    for (auto& b : generations.bucket) b.fetch_add(1, std::memory_order_acq_rel);
    map.clear();
}

} // namespace recomp::imports_fast
