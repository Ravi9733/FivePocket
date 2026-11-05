#pragma once
// Attempt v42: the registry mutex R (GuestImportRegistry::mutex_) as typed sections, renderer read
// validation against an immutable generation-published snapshot (no R on a hit), and per-site R
// accounting. Included by guest_import_registry.cpp and guest_native_initialization.cpp AFTER every
// other include: the last line poisons the member name `mutex_`, so any acquisition of R in those
// translation units that does not go through a section below fails to compile.
//
// Invariant (the only thing the snapshot readers rely on)
// ------------------------------------------------------
// V = the state the renderer read predicates consult (copySerializedGuestHeap/viewSerializedGuestHeap
// and copySerializedGraphicsData/viewSerializedGraphicsData): addressSpace_, heapBase_, heapSize_,
// allocations_, virtualCommitBits_, nativeEventPool_, nativeXamAddress_, nativeTitleAddress_,
// threadObjectTypeAddress_, threadObjects_ (key set), threadStorage_, threadStorageConfig_,
// threadTlsStorage_, physicalMemoryArenaBound_, physicalArenaBase_, physicalArenaSize_,
// physicalAddressOffset_, physicalAllocations_, graphicsInitialized_, graphicsBackend_ (pointer and
// every non-const call through it, since those can change ready()).
// 1. Every write to V goes through RECOMP_V(member) / RECOMP_V_BACKEND() / RECOMP_V_BACKEND_SHARED().
//    This is checked by the compiler: tests/check_validation_mutations.py compiles both TUs against a
//    copy of the header in which exactly these members are const (the backend as a pointer to a const
//    backend); any other write is a compile error there.
// 2. RECOMP_V opens a mutation window on the innermost registry section of the calling thread
//    (window count += 1 in the slot word, an acq_rel RMW) BEFORE returning the writable reference,
//    so the window's opening is sequenced before the write. The window closes (generation += 1,
//    window count -= 1, one release RMW) when that section releases R (end of section or a
//    RECOMP_REGISTRY_UNLOCK), i.e. after the write. RECOMP_V_BACKEND_SHARED (a backend pointer copied
//    for a call made after R is released) pins the window until the section ends. A write without
//    R, or outside any section, opens a window that never closes: the fast path is then off for that
//    registry (fail closed).
// 3. A snapshot for generation g is built only by a thread holding R that observed window count 0
//    and generation g. While it holds R no window can open (opening needs R), so V is stable and
//    equals the snapshot.
// 4. A reader that loads the slot word (acquire) and sees window count 0 and generation g uses the
//    snapshot of generation g. Every write W to V lies inside a window [open, close]. If the load
//    reads a value after `close` in modification order it reads a generation > the one before W, so
//    the snapshot was built after W (all changes of the word are RMWs, so the load synchronizes with
//    `close` through its release sequence). If it reads a value before `open`, the read linearizes
//    before W; nothing the reader knows can have come from W (if `open` happened-before the load,
//    coherence forbids reading an earlier value). A value between them has a nonzero window count
//    and the reader falls back to the locked path. Hence: fast acceptance => the locked predicate
//    held at the load, which lies inside the call, exactly the guarantee v33+ gives (validated under
//    R at one instant, bytes read after R was released).
// 5. The fast path only ACCEPTS. Any doubt (window open, no slot, stale or missing snapshot, a
//    range that touches the thread arena whose answer depends on the calling thread, a snapshot
//    rejection) runs the unchanged locked predicate.
// 6. Under R, overlapsThreadTlsStorage uses the published snapshot's TLS ranges only when
//    registryCurrentSnapshotLocked proves it equal to V (this thread's section holds R and opened no
//    window, no window is open anywhere, the published generation is current).
//
// Lock order: sections take only R. A snapshot refresh is an ordinary R acquisition by a reader
// that holds no other registry lock (the renderer's callers hold at most the gate/L_gpu above R).
// Slot claims use a leaf mutex taken with or without R and never while waiting for anything.
// Logging runs after R is released.
#include "guest_import_registry.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include "imports_fast.h" // imports-fast (agent F): recomp_imports_fast, node caches

namespace recomp::bootstrap {

// Copy of V taken under R (captureValidationSnapshotLocked). Immutable once published.
struct GuestRegistryValidationSnapshot {
    std::uint64_t generation{0};
    GuestAddressSpace* addressSpace{nullptr};
    std::uint32_t heapBase{0}, heapSize{0};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> heapAllocations;       // base -> size, ascending
    std::vector<std::uint64_t> commitBits;                                        // virtualCommitBits_
    std::vector<std::pair<std::uint64_t, std::uint64_t>> protectedRanges;         // [begin,end), disjoint, ascending
    std::vector<std::pair<std::uint64_t, std::uint64_t>> tlsRanges;               // TLS blocks only, same form
    bool threadArena{false};
    std::uint64_t threadArenaBegin{0}, threadArenaEnd{0};
    bool physicalBound{false};
    std::uint32_t physicalBase{0}, physicalSize{0}, physicalOffset{0};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> physicalAllocations;   // base -> size, ascending
    bool graphicsReadable{false};
};

// ---- time base: the ARM generic timer on the device (no syscall), steady_clock elsewhere.
inline std::uint64_t registryTicks() noexcept {
#if defined(__aarch64__)
    std::uint64_t value;
    asm volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
#else
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}
std::uint64_t registryTicksToNs(std::uint64_t ticks) noexcept;

// ---- per call site R accounting (one static object per section site; dispatch uses one per import).
// v110: roles of the waiting host thread, by the guest token it runs (RENDER_DIAG_REGISTRY_WAIT).
enum RegistryRole : unsigned { RegistryRoleMain, RegistryRoleRender, RegistryRoleTile, RegistryRoleAudio,
    RegistryRoleOther, RegistryRoleHost, RegistryRoles };
struct RegistryLockSite {
    const char* function; // __func__, or the import name for dispatch sites (line 0)
    int line;
    std::atomic<std::uint64_t> acquisitions{0}, contended{0}, waitTicks{0}, holdTicks{0}, maxHoldTicks{0}, windows{0};
    // v110 (contended acquisitions only, written directly): wait ticks by waiting role; how each wait
    // ended (a try_lock while spinning, or the futex); and holds of this site that ended while another
    // thread was waiting for R ("blocking" holds: the sites that make others wait, and for how long).
    std::array<std::atomic<std::uint64_t>, RegistryRoles> roleWaitTicks{};
    std::atomic<std::uint64_t> spun{0}, blocked{0}, blockingHolds{0}, blockingHoldTicks{0}, maxBlockingHoldTicks{0};
    std::atomic<RegistryLockSite*> next{nullptr};
    std::atomic<bool> listed{false};
    RegistryLockSite(const char* f, int l) noexcept : function(f), line(l) {}
};
void registryListSite(RegistryLockSite& site) noexcept;
struct RegistryThreadState;
// Per-thread accumulation of one section's accounting; flushed to the site's shared counters when
// a thread's small batch table evicts the site or every 64 sections (so the counters that the
// 5 s log reads are at most 63 sections per thread and site behind).
void registryAccount(RegistryThreadState& state, RegistryLockSite& site, std::uint64_t acquisitions, std::uint64_t contended,
        std::uint64_t waitTicks, std::uint64_t holdTicks, std::uint64_t maxIntervalTicks, std::uint64_t blockingHolds = 0,
        std::uint64_t blockingHoldTicks = 0, std::uint64_t maxBlockingHoldTicks = 0) noexcept;
// v110: R after a failed try_lock. Spins (try_lock with yields, bounded to ~20 us) before blocking in
// the futex unless files/private-trace/diag-registry-no-spin exists (v109: straight to the futex).
// Counts the wait in the section's site (role, spun/blocked) and in the global waiter count that
// releasing holders read. Returns the wait in registry ticks.
class RegistrySection;
std::uint64_t registryLockContended(std::mutex& mutex, RegistrySection& section) noexcept;
bool registrySpinEnabled() noexcept;
// v110 host tests: the global accounting of contended acquisitions.
struct RegistryContentionTotals { std::uint64_t spun, blocked, spinTicks, blockTicks, waitersNow; };
RegistryContentionTotals registryContentionTotals() noexcept;
void registryMaybeLog(RegistryThreadState& state) noexcept;
void registryReadLockAcquired(bool spun, bool blocked, std::uint64_t blockedNs) noexcept;

// ---- per registry generation word and published snapshot.
struct RegistryValidationSlot {
    std::atomic<const GuestImportRegistry*> owner{nullptr};
    // generation << 16 | open windows. Changed only by RMWs.
    std::atomic<std::uint64_t> word{0};
    std::shared_ptr<const GuestRegistryValidationSnapshot> published; // guarded by the owner's R
    std::atomic<std::uint64_t> publishedGeneration{~std::uint64_t{0}}; // hint for readers; stored under R
};
constexpr std::uint64_t RegistryWindowOne = 1;
constexpr std::uint64_t RegistryGenerationOne = std::uint64_t{1} << 16;
constexpr std::uint64_t RegistryWindowMask = RegistryGenerationOne - 1;
RegistryValidationSlot* registryFindSlot(const GuestImportRegistry* registry) noexcept;
RegistryValidationSlot* registryClaimSlot(const GuestImportRegistry* registry) noexcept;
// For code holding R: the published snapshot when it provably equals the live state (the calling
// thread's innermost section holds R for this registry and opened no window, no window is open
// anywhere, and the published generation is current); else nullptr. Valid until R is released.
const GuestRegistryValidationSnapshot* registryCurrentSnapshotLocked(const GuestImportRegistry* registry) noexcept;
// Any overlap of [begin, end), end > begin, with a merged ascending range list: equal to testing
// every original entry with the loops' formula `begin < e.end && e.begin < end` (degenerate [b,b)
// entries included). Not for empty queries (touching entries merge; see overlapsThreadTlsStorage).
bool registryRangesOverlap(const std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges,
        std::uint64_t begin, std::uint64_t end) noexcept;

// ---- sections: every holder of R.
// All per-thread registry state (innermost section, import-site cache, accounting batches, the
// snapshot reader cache) lives in one thread_local object, resolved once per section: a TLS access
// of a dlopen'ed library is a resolver call.
struct RegistryThreadState;
RegistryThreadState& registryThreadState() noexcept;
class RegistrySection {
public:
    RegistryThreadState& thread;
    const GuestImportRegistry* const registry;
    RegistryLockSite& site;
    std::unique_lock<std::mutex>* unique{nullptr}; // unique sections: the lock handlers unlock/relock
    bool held{false};                             // other sections: R held by this section now
    RegistryValidationSlot* window{nullptr};       // open mutation window, if any
    bool windowPinned{false};                      // kept open across unlocks until the section ends
    std::uint64_t acquiredAt{0}, heldTicks{0}, maxIntervalTicks{0};
    std::uint64_t acquisitionCount{0}, contendedCount{0}, contendedWaitTicks{0};
    std::uint64_t blockingHolds{0}, blockingHoldTicks{0}, maxBlockingHoldTicks{0}; // v110
    RegistrySection* const previous;

    bool holdsMutex() const noexcept { return unique ? unique->owns_lock() : held; }
    void openWindow() noexcept;  // under R
    // generation += 1, windows -= 1; at an unlock (atEnd=false) a pinned window stays open.
    void closeWindow(bool atEnd = false) noexcept;
    void noteAcquired(bool contended, std::uint64_t waitTicks) noexcept;
    void noteReleasedInterval() noexcept; // adds now - acquiredAt to heldTicks
    void publishHold() noexcept;          // site counters, once at the end of the section
    RegistrySection(const RegistrySection&) = delete;
    RegistrySection& operator=(const RegistrySection&) = delete;
protected:
    RegistrySection(RegistryThreadState& state, const GuestImportRegistry* r, RegistryLockSite& s) noexcept;
    ~RegistrySection();
};

// lock_guard form (no spin: dispatch-like holders keep bionic's futex behaviour).
class RegistryGuard final : public RegistrySection {
public:
    RegistryGuard(const GuestImportRegistry* r, RegistryLockSite& s) noexcept;
    ~RegistryGuard();
private:
    std::mutex& mutex_ref_;
};

// unique_lock form: `native()` is the std::unique_lock handlers receive; they may unlock/relock it.
class RegistryUnique final : public RegistrySection {
public:
    RegistryUnique(const GuestImportRegistry* r, RegistryLockSite& s) noexcept;
    // dispatch(): the section site of that import (per-thread cached lookup).
    RegistryUnique(const GuestImportRegistry* r, std::string_view importName) noexcept;
    RegistryUnique(RegistryThreadState& state, const GuestImportRegistry* r, std::string_view importName) noexcept;
    ~RegistryUnique();
    std::unique_lock<std::mutex>& native() noexcept { return lock_; }
private:
    std::unique_lock<std::mutex> lock_;
};

// imports-fast A: dispatch's section for the hot imports while diag-imports-fast is on. The same R acquisition (try_lock,
// then registryLockContended's spin / futex and its contended-wait accounting), mutation windows and logging cadence as
// RegistryUnique, without the hold accounting (no clock read at acquisition and release, nothing published to the site;
// feeds only RENDER_DIAG_REGISTRY_HOLD*). New functions only: RegistrySection's members and every existing section keep
// their code (and their PGO profile). A handler's RECOMP_REGISTRY_UNLOCK / RELOCK still does its (unpublished) accounting.
// `site` (windows, contended waits) is the caller's static site per hot import (same label as the import's dispatch site).
class RegistryUniqueQuiet final : public RegistrySection {
public:
    RegistryUniqueQuiet(RegistryThreadState& state, const GuestImportRegistry* r, RegistryLockSite& site) noexcept;
    ~RegistryUniqueQuiet();
    std::unique_lock<std::mutex>& native() noexcept { return lock_; }
private:
    std::unique_lock<std::mutex> lock_;
};

// Renderer reads (const members): spin up to ~20 us before blocking (v33 policy), legacy
// RENDER_DIAG_REGISTRY_READ_LOCK counters, and an early unlock() before copying bytes.
class RegistryReader final : public RegistrySection {
public:
    RegistryReader(const GuestImportRegistry* r, RegistryLockSite& s) noexcept;
    ~RegistryReader();
    void unlock() noexcept;
private:
    std::mutex& mutex_ref_;
};

// Unlock/relock of a unique section's lock inside a handler: closes the window and keeps the hold
// accounting exact. A raw lock.unlock() stays correct (the window just stays open longer).
void registryUnlock(std::unique_lock<std::mutex>& lock);
void registryRelock(std::unique_lock<std::mutex>& lock);

// ---- the V write funnel. `where` names the writing function in the poison diagnostic.
// pin: the write continues after R is released (a backend call made outside R through a copied
// pointer), so the window stays open until the section ends.
void registryNoteValidationMutation(const GuestImportRegistry* self, const char* where, bool pin = false) noexcept;
template<class T>
inline T& registryMutableValidation(const GuestImportRegistry* self, const T& member,
        const char* where = __builtin_FUNCTION()) noexcept {
    registryNoteValidationMutation(self, where);
    return const_cast<T&>(member);
}
template<class P>
inline auto* registryMutableBackend(const GuestImportRegistry* self, const P& pointer,
        const char* where = __builtin_FUNCTION()) noexcept {
    registryNoteValidationMutation(self, where);
    using T = std::remove_const_t<typename P::element_type>;
    return const_cast<T*>(pointer.get());
}
template<class P>
inline auto registryMutableBackendShared(const GuestImportRegistry* self, const P& pointer,
        const char* where = __builtin_FUNCTION()) noexcept {
    registryNoteValidationMutation(self, where, true);
    using T = std::remove_const_t<typename P::element_type>;
    return std::const_pointer_cast<T>(pointer);
}

} // namespace recomp::bootstrap

#define RECOMP_REGISTRY_CAT2(a, b) a##b
#define RECOMP_REGISTRY_CAT(a, b) RECOMP_REGISTRY_CAT2(a, b)
#define RECOMP_REGISTRY_SITE_NAME RECOMP_REGISTRY_CAT(recompRegistrySite_, __LINE__)
// Two statements: a static site for this line, then the section. Use as a declaration statement.
#define RECOMP_REGISTRY_LOCK(name) \
    static ::recomp::bootstrap::RegistryLockSite RECOMP_REGISTRY_SITE_NAME{__func__, __LINE__}; \
    ::recomp::bootstrap::RegistryGuard name(this, RECOMP_REGISTRY_SITE_NAME)
#define RECOMP_REGISTRY_READER(name) \
    static ::recomp::bootstrap::RegistryLockSite RECOMP_REGISTRY_SITE_NAME{__func__, __LINE__}; \
    ::recomp::bootstrap::RegistryReader name(this, RECOMP_REGISTRY_SITE_NAME)
#define RECOMP_REGISTRY_UNIQUE(name) \
    static ::recomp::bootstrap::RegistryLockSite RECOMP_REGISTRY_SITE_NAME{__func__, __LINE__}; \
    ::recomp::bootstrap::RegistryUnique RECOMP_REGISTRY_CAT(name, Section)(this, RECOMP_REGISTRY_SITE_NAME); \
    auto& name = RECOMP_REGISTRY_CAT(name, Section).native()
#define RECOMP_REGISTRY_UNLOCK(lock) ::recomp::bootstrap::registryUnlock(lock)
#define RECOMP_REGISTRY_RELOCK(lock) ::recomp::bootstrap::registryRelock(lock)
#define RECOMP_V(member) (::recomp::bootstrap::registryMutableValidation(this, (member)))
#define RECOMP_V_BACKEND() (::recomp::bootstrap::registryMutableBackend(this, graphicsBackend_))
#define RECOMP_V_BACKEND_SHARED() (::recomp::bootstrap::registryMutableBackendShared(this, graphicsBackend_))

// Every acquisition of R in these translation units is a section above (see the header comment).
#define mutex_ registry_mutex_is_taken_only_through_RECOMP_REGISTRY_sections
