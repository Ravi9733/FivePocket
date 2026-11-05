// native-imports (native_imports.h; lane native-syscalls, attempt v526): the fast functions and their hand-over to the
// registry's locked paths. Included as the last line of guest_import_registry.cpp: it uses that file's local state (the
// stripes, the calling thread's binding, the v252 TLS cache, the quiet registry section) and adds no member to the class
// (the shared header android/app/src/main/cpp/guest_import_registry.h fixes them, and a copy of it in an attempt's src/
// redefines the class for the repo headers that include it from their own directory: imports_direct.h, v481).
//
// CRITICAL SECTIONS WITHOUT A HOST LOCK
// -------------------------------------
// What the registry keeps per section today, and where:
//   guest memory   bytes 0..15 the dispatcher header (written once, at initialization), +16 LockCount (recursion - 1, or
//                  -1 when free; it does NOT count waiters, unlike the console's), +20 RecursionCount, +24 OwningThread.
//   host           criticalSections_[address] = {a copy of the header, ownerToken, recursionCount}: the registry's record,
//                  which every operation compares with the guest words first (criticalSectionStorageMatches) and refuses
//                  on any difference; criticalSectionWaits_[address] = the FIFO of parked enters; edramCalibrationLock_;
//                  the recorded-rejection flag; the executor's exclusive scope.
// The waiter queue is host state that the guest words cannot express: LockCount is defined as recursion - 1, so "free but
// promised to the front waiter" looks exactly like "free". A compare-and-swap on the guest words alone would let a
// newcomer take a section past its queue and would let a leave skip the wake. So the guest words carry the lock, and one
// host word per section (the gate) says whether the lock-free path may be used at all:
//
//   The gate (NativeCsEntry::gate, odd = fast, even = slow, only ever incremented). A section is put on the fast path
//   (nativeCsPromote, under its stripe) only when the registry knows it, its storage matches the record, it has no
//   waiter and it is not the EDRAM calibration lock. Every locked path that reads or writes the record of a section
//   first takes it off the fast path (csSlow: gate odd -> even) and waits until no fast call is still working on it.
//   While a section has waiters it cannot be promoted, so on the fast path "queued" is always false.
//
//   Single source of truth for ownership: the guest OwningThread word (+24). A fast enter of a free section is ONE
//   compare-and-swap 0 -> token on it; nothing else can make a thread the owner on this path. RecursionCount, LockCount
//   and the registry's record (ownerToken, recursionCount in the map node) are written only by the owner, between its
//   compare-and-swap and its release; the release is the last store (OwningThread = 0, after the record and the two
//   counts say "free"). Hence whenever OwningThread is 0 and no fast call is in flight, guest words and record are both
//   "free", and whenever it is T and T is not inside a fast call, they both say (T, n): the record the locked paths find
//   after csSlow is exactly the one they would have written themselves. Every fast call checks all of today's storage
//   match (header, three words, record) before it changes anything, so a mismatch is found in the same call as today
//   and left to the locked path, which reports it as today.
//
//   Hand-over fast -> slow (csSlow). A fast call announces the section's address in its own thread record (inflight,
//   a store) BEFORE it reads the gate, and clears it when it is done. csSlow makes the gate even and THEN reads every
//   thread's inflight word; both sides use sequentially consistent operations, so either the fast call sees the even
//   gate and does nothing, or csSlow sees the announcement and waits for it to clear (the fast call takes no lock and
//   makes no call that can block, so the wait is short and cannot deadlock). After csSlow returns, the section's words
//   and record are stable until its stripe is released AND somebody promotes it again; promotion needs that stripe.
//   Hand-over slow -> fast (nativeCsPromote): by the next fast call that finds the gate even, under the stripe, with the
//   checks above; the locked paths never have to give a section back.
//
//   The table. Entries are never freed. A way is re-keyed (eviction by a promotion into a full set, the window poll's
//   emptying of the table while the switch is off, a registry cleared by initialize / reset) only under the table lock,
//   with its gate even and after the same wait for the fast calls of the section it held; a fast call reads an entry's
//   fields only after its announcement and after it has read the gate odd, so the fields it reads are its section's and
//   stay so until it ends. While such a re-keying waits, it names the address in one marker word (nativeCsEvicting), so
//   that a locked path arriving for that section in the meantime (it finds the gate already even) waits as well. csSlow
//   itself makes a gate even under the table lock, where the way's key cannot change under it. The record pointer in
//   an entry is used only inside a fast call that passed the gate; the registry erases a node only after csRetire or
//   csRetireAll, so no fast call can hold a pointer to an erased node.
//
// What does not change: which thread may own a section and when a waiter runs (FIFO among parked enters; a section with
// waiters is entirely the locked path's), the values of the three words at every point a call has returned, and every
// refusal. What does change inside a call: the order of the three stores (the owner word first on an enter, last on a
// final leave; today LockCount, RecursionCount, OwningThread), which only a guest read racing with the call could see,
// as it can see today's order.
//
// HANDLE IMPORTS (lockedCall) still take the registry lock. An NT event or semaphore has no guest structure: its only
// state is KernelObject::signalCount in kernelHandles_, a std::map guarded by the registry lock, and a parked wait is
// protected from a lost wake-up by that same lock (the waiter registers under it before unlocking; the signaller changes
// the object and reads the parked-wait count under it). A lock-free form needs the object's count in a word of its own
// that every one of the registry's ~50 uses of the map reads and writes atomically, and a waiter that sleeps on that word;
// that is the executor's parking redone, not a fast path in front of it. Not built here; see the lane's report.
#include <pthread.h>
#if defined(RECOMP_NATIVE_TEST_SCHEDULE_POINTS) // (native_imports.h: host tests only)
namespace recomp::native_imports { void testPause() noexcept {
    static std::atomic<unsigned> points{0};
    if ((points.fetch_add(1, std::memory_order_relaxed) & 15u) == 0) ::usleep(80); else ::sched_yield();
} }
#endif

std::atomic<bool> recomp_native_imports{false};
std::atomic<bool> recomp_native_imports_locked{true};

#if RECOMP_PARALLEL_REGISTRY
extern "C" const void* recomp_v362_guard_active_slot() noexcept; // guest_execution_guard.cpp: &activeExecution of the calling thread

// Private members by explicit instantiation (the standard exempts the arguments of an explicit instantiation from access
// checks, [temp.spec]): member(Tag{}) is the member pointer, a constant, so a call or an access through it compiles to the
// direct one. Reads only, except the record of a section this file owns on the fast path (see above).
namespace recomp::native_imports::access {
template<class Tag, auto Member> struct Open { friend constexpr auto member(Tag) noexcept { return Member; } };
#define RECOMP_NATIVE_OPEN(tag, name) \
    struct tag { friend constexpr auto member(tag) noexcept; }; \
    template struct Open<tag, &::recomp::bootstrap::GuestImportRegistry::name>;
RECOMP_NATIVE_OPEN(Sections, criticalSections_)
RECOMP_NATIVE_OPEN(SectionWaits, criticalSectionWaits_)
RECOMP_NATIVE_OPEN(Space, addressSpace_)
RECOMP_NATIVE_OPEN(EdramLock, edramCalibrationLock_)
RECOMP_NATIVE_OPEN(StorageMatches, criticalSectionStorageMatches)
RECOMP_NATIVE_OPEN(ReadU32, readU32)
RECOMP_NATIVE_OPEN(TlsSlots, TlsSlotCount)
RECOMP_NATIVE_OPEN(SceneScope, nativeTileSceneInterruptScope_)
RECOMP_NATIVE_OPEN(TileLockScope, nativeTileLockScope_)
RECOMP_NATIVE_OPEN(InterruptScope, nativeInterruptScope_)
RECOMP_NATIVE_OPEN(XamAddress, nativeXamAddress_)
RECOMP_NATIVE_OPEN(SignedOutUsers, signedOutUsersBound_)
RECOMP_NATIVE_OPEN(SetEvent, dispatchNtSetEvent)
RECOMP_NATIVE_OPEN(ClearEvent, dispatchNtClearEvent)
RECOMP_NATIVE_OPEN(ReleaseSemaphore, dispatchNtReleaseSemaphore)
RECOMP_NATIVE_OPEN(WaitSingle, dispatchNtWaitForSingleObjectEx)
#undef RECOMP_NATIVE_OPEN
} // namespace recomp::native_imports::access

namespace recomp::bootstrap {
namespace {
namespace nat = ::recomp::native_imports::access;
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the guest words are byte-swapped as on a little-endian host");

// ---- the RENDER_GENERIC_NATIVE fields, in order
enum NativeCounter : unsigned { NativeCsEnterFast, NativeCsLeaveFast, NativeCsFallback, NativeTlsFast, NativeEventFast,
    NativeSemFast, NativeWaitFast, NativeOtherFallback, NativeCounterCount };

// ---- one record per host thread that has made a fast call (never freed; reused by a later thread after its thread ends)
// The calling thread finds its record by its thread pointer (one register read on the device) in a small table instead of
// a thread_local of this dlopen'ed library, each access of which is a call into the linker's TLSDESC resolver. The record
// holds the ADDRESSES of that thread's own thread_local variables which the fast calls read (v362's argument: the same
// objects, only the address computation differs), the in-flight word of the hand-over above, and the thread's counters.
struct alignas(64) NativeThread {
    std::atomic<std::uint32_t> inflight{0};       // the section a fast call of this thread is working on (0: none)
    const BoundGuestThread* bound{nullptr};       // &boundGuestThread
    const void* guardActive{nullptr};             // &activeExecution (GuestExecutionGuard::isActive tests it for null)
    const TlsReadCache* tlsRead{nullptr};         // &tlsReadCache (v252)
    RegistryThreadState* registryState{nullptr};  // &registryThreadState() (the quiet section's per-thread state)
    NativeThread* next{nullptr};                  // every record ever made (push-only list)
    NativeThread* nextFree{nullptr};              // records of ended threads (under nativeThreadLock)
    std::array<std::atomic<std::uint64_t>, NativeCounterCount> counters{}; // written by the owning thread only (relaxed)
};
constexpr unsigned NativeTpSlots = 256, NativeTpProbe = 8;
struct NativeTpSlot { std::atomic<std::uintptr_t> identity{0}; std::atomic<NativeThread*> record{nullptr}; };
std::array<NativeTpSlot, NativeTpSlots> nativeTpSlots;
std::atomic<NativeThread*> nativeThreads{nullptr};
NativeThread* nativeFreeThreads = nullptr;
std::mutex nativeThreadLock; // registration and release only (a leaf)
std::atomic<unsigned long long> nativeUnregistered{0}, nativePromoted{0}, nativeTakenOver{0}, nativeEvicted{0}, nativePromoteRefused{0};

[[gnu::always_inline]] inline unsigned nativeTpHash(std::uintptr_t identity) noexcept {
    return static_cast<unsigned>(((identity >> 6) * 0x9E3779B97F4A7C15ull) >> 56); // 8 bits
}
static_assert(NativeTpSlots == 256);
// A thread's slot is released when the thread ends (before its thread pointer can be given to a new thread, as v362's
// claim is): a slot whose identity equals the caller's was therefore written by the caller itself.
struct NativeThreadExit {
    NativeTpSlot* slot = nullptr;
    ~NativeThreadExit() {
        if (slot == nullptr) return;
        std::lock_guard lock(nativeThreadLock);
        auto* record = slot->record.load(std::memory_order_relaxed);
        slot->identity.store(0, std::memory_order_release);
        slot->record.store(nullptr, std::memory_order_release);
        if (record != nullptr) { record->inflight.store(0, std::memory_order_seq_cst); record->nextFree = nativeFreeThreads; nativeFreeThreads = record; }
    }
};
thread_local NativeThreadExit nativeThreadExit; // (touched once per thread, by nativeThreadSlow)
[[gnu::noinline]] NativeThread* nativeThreadSlow(std::uintptr_t identity) noexcept {
    const unsigned first = nativeTpHash(identity);
    for (unsigned n = 1; n < NativeTpProbe; ++n) {
        auto& slot = nativeTpSlots[(first + n) & (NativeTpSlots - 1)];
        if (slot.identity.load(std::memory_order_relaxed) == identity) return slot.record.load(std::memory_order_relaxed);
    }
    std::lock_guard lock(nativeThreadLock);
    NativeTpSlot* slot = nullptr;
    for (unsigned n = 0; n < NativeTpProbe && slot == nullptr; ++n) {
        auto& candidate = nativeTpSlots[(first + n) & (NativeTpSlots - 1)];
        if (candidate.identity.load(std::memory_order_relaxed) == 0) slot = &candidate;
    }
    NativeThread* record = slot != nullptr ? nativeFreeThreads : nullptr;
    if (record != nullptr) nativeFreeThreads = record->nextFree;
    else if (slot != nullptr) record = new (std::nothrow) NativeThread;
    if (record == nullptr) { nativeUnregistered.fetch_add(1, std::memory_order_relaxed); return nullptr; } // (the old path)
    const bool fresh = record->bound == nullptr;
    record->bound = &boundGuestThread;
    record->guardActive = recomp_v362_guard_active_slot();
    record->tlsRead = &tlsReadCache;
    record->registryState = &registryThreadState();
    record->nextFree = nullptr;
    if (fresh) { // published before this thread's first announcement (csQuiet reads the list after it changed a gate)
        auto* head = nativeThreads.load(std::memory_order_seq_cst);
        do { record->next = head; } while (!nativeThreads.compare_exchange_weak(head, record, std::memory_order_seq_cst));
    }
    slot->record.store(record, std::memory_order_release);
    slot->identity.store(identity, std::memory_order_release);
    nativeThreadExit.slot = slot;
    return record;
}
[[gnu::always_inline]] inline NativeThread* nativeThread() noexcept {
    const auto identity = ::recomp::v362::threadIdentity();
    auto& slot = nativeTpSlots[nativeTpHash(identity)];
    if (slot.identity.load(std::memory_order_relaxed) == identity) [[likely]] return slot.record.load(std::memory_order_relaxed);
    return nativeThreadSlow(identity);
}
[[gnu::always_inline]] inline void nativeCount(NativeThread& th, NativeCounter which) noexcept {
    auto& c = th.counters[which];
    c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); // (one writer: no read-modify-write instruction)
}
[[gnu::always_inline]] inline bool nativeMiss(NativeThread& th, NativeCounter which) noexcept { nativeCount(th, which); return false; }
// RECOMP_CURRENT_GUEST_TOKEN's first case, through the cached addresses: the calling host thread's own binding while it
// executes guest code. 0 = any other case (the registry-wide member: the old path computes it).
[[gnu::always_inline]] inline std::uint32_t nativeToken(const NativeThread& th, const GuestImportRegistry* registry) noexcept {
    const auto& bound = *th.bound;
    const void* active; std::memcpy(&active, th.guardActive, sizeof active); // (the pointer variable's own bytes, as v362 reads it)
    return bound.registry == registry && bound.token != 0 &&
           bound.epoch == guestIdentityEpoch.load(std::memory_order_acquire) && active != nullptr ? bound.token : 0;
}

// ---- guest words (big endian; aligned: the callers checked address & 3 and the base is page aligned)
[[gnu::always_inline]] inline std::uint32_t nativeLoad(const std::uint8_t* at) noexcept {
    return __builtin_bswap32(__atomic_load_n(reinterpret_cast<const std::uint32_t*>(at), __ATOMIC_RELAXED));
}
[[gnu::always_inline]] inline void nativeStore(std::uint8_t* at, std::uint32_t value) noexcept {
    __atomic_store_n(reinterpret_cast<std::uint32_t*>(at), __builtin_bswap32(value), __ATOMIC_RELAXED);
}

// ---- the table: (registry, address) -> gate, record, header copy. 2048 sets of 4 ways. The four addresses of a set
// share 16 bytes (one cache line holds four sets' keys); each way's other fields have a cache line of their own.
using NativeCsMap = std::remove_reference_t<decltype(std::declval<GuestImportRegistry&>().*member(nat::Sections{}))>;
using NativeCsState = NativeCsMap::mapped_type;
#if defined(RECOMP_NATIVE_TEST_SET_BITS) // host tests only (tests/build.sh's "sp" variants): a table of a few ways, so that a handful of sections evict each other all the time
constexpr unsigned NativeCsSetBits = RECOMP_NATIVE_TEST_SET_BITS;
#else
constexpr unsigned NativeCsSetBits = 11;
#endif
constexpr unsigned NativeCsSets = 1u << NativeCsSetBits, NativeCsWays = 4;
static_assert(NativeCsSetBits >= 1 && NativeCsSetBits <= 16);
struct alignas(64) NativeCsEntry {
    std::atomic<std::uint64_t> gate{0};                          // odd: fast. Only incremented.
    std::atomic<const GuestImportRegistry*> registry{nullptr};
    std::atomic<NativeCsState*> state{nullptr};                  // the registry's record (its map node)
    std::atomic<std::uint8_t*> host{nullptr};                    // addressSpace_->translate(address)
    std::uint64_t header[2]{};                                   // the record's dispatcher header (written with the gate even)
};
static_assert(sizeof(NativeCsEntry) == 64);
alignas(64) std::array<std::atomic<std::uint32_t>, NativeCsSets * NativeCsWays> nativeCsKeys; // a way's section address (0: free)
std::array<NativeCsEntry, NativeCsSets * NativeCsWays> nativeCsTable;
std::array<std::uint8_t, NativeCsSets> nativeCsClock{}; // next way to evict per set (under nativeCsTableLock)
std::mutex nativeCsTableLock; // entry writers (promotion, eviction, retirement); a leaf, taken under a stripe
// True from the first promotion until the window poll has emptied the table with the switch off (nativeCsIdle): while it
// is false no gate is odd and no fast call is in flight, and the locked paths' hooks return at once.
std::atomic<bool> nativeCsUsed{false};
std::atomic<std::uint32_t> nativeCsEvicting{0}; // the address whose entry an eviction is taking off the fast path now (0: none)
[[gnu::always_inline]] inline unsigned nativeCsSet(std::uint32_t address) noexcept {
    return (((address >> 2) * 0x9E3779B1u) >> (32 - NativeCsSetBits)) * NativeCsWays; // index of the set's first way
}

// No fast call is working on `address` any more. Called after the gate was made even (or found even).
[[gnu::noinline]] void nativeCsQuiet(std::uint32_t address) noexcept {
    for (unsigned round = 0;; ++round) {
        bool busy = false;
        for (auto* t = nativeThreads.load(std::memory_order_seq_cst); t != nullptr && !busy; t = t->next)
            busy = t->inflight.load(std::memory_order_seq_cst) == address;
        if (!busy) return;
        if (round >= 64) ::sched_yield(); // (the fast call was preempted between its announcement and its end)
    }
}
// The way of (registry, address) in its set, whatever its gate; -1 if none. Its key may change at any time unless the
// caller holds nativeCsTableLock.
inline int nativeCsLookup(const GuestImportRegistry* registry, std::uint32_t address) noexcept {
    const unsigned set = nativeCsSet(address);
    for (unsigned way = 0; way < NativeCsWays; ++way)
        if (nativeCsKeys[set + way].load(std::memory_order_seq_cst) == address &&
                nativeCsTable[set + way].registry.load(std::memory_order_seq_cst) == registry) return static_cast<int>(set + way);
    return -1;
}
// odd -> even; true when this call did it.
[[gnu::always_inline]] inline bool nativeCsDemote(NativeCsEntry& entry) noexcept {
    auto gate = entry.gate.load(std::memory_order_seq_cst);
    while (gate & 1)
        if (entry.gate.compare_exchange_weak(gate, gate + 1, std::memory_order_seq_cst)) {
            nativeTakenOver.fetch_add(1, std::memory_order_relaxed); return true; }
    return false;
}
// Clears one way (its gate is even and no fast call works on its section). Under nativeCsTableLock.
inline void nativeCsClearWay(unsigned index) noexcept {
    nativeCsKeys[index].store(0, std::memory_order_seq_cst);
    auto& entry = nativeCsTable[index];
    entry.registry.store(nullptr, std::memory_order_seq_cst);
    entry.state.store(nullptr, std::memory_order_relaxed);
    entry.host.store(nullptr, std::memory_order_relaxed);
}
// Every way of `registry` (nullptr: of every registry) leaves the fast path and is cleared. Under nativeCsTableLock.
void nativeCsClearAll(const GuestImportRegistry* registry) noexcept {
    for (unsigned index = 0; index < nativeCsTable.size(); ++index) {
        const auto address = nativeCsKeys[index].load(std::memory_order_seq_cst);
        if (address == 0 || (registry != nullptr && nativeCsTable[index].registry.load(std::memory_order_seq_cst) != registry)) continue;
#if !defined(RECOMP_NATIVE_MUTANT_EVICT_NO_MARKER)
        nativeCsEvicting.store(address, std::memory_order_seq_cst); // (as an eviction: a csSlow of this address waits with us)
#endif
        (void)nativeCsDemote(nativeCsTable[index]);
        RECOMP_NATIVE_SCHEDULE_POINT();
        nativeCsQuiet(address);
        nativeCsClearWay(index);
        nativeCsEvicting.store(0, std::memory_order_seq_cst);
    }
}

// A fast call's admission: announce, then find the section's entry with its gate odd. On success the announcement stays
// (the caller clears it when it is done); on failure it is cleared and nullptr returned.
[[gnu::always_inline]] inline NativeCsEntry* nativeCsOpen(NativeThread& th, const GuestImportRegistry* registry,
        std::uint8_t* base, std::uint32_t address) noexcept {
#if !defined(RECOMP_NATIVE_MUTANT_NO_ANNOUNCE) // (mutant: a take-over does not wait for this call)
    th.inflight.store(address, std::memory_order_seq_cst);
#endif
    RECOMP_NATIVE_SCHEDULE_POINT();
    const unsigned set = nativeCsSet(address);
    for (unsigned way = 0; way < NativeCsWays; ++way) {
        if (nativeCsKeys[set + way].load(std::memory_order_relaxed) != address) continue;
        auto& entry = nativeCsTable[set + way];
        RECOMP_NATIVE_SCHEDULE_POINT();
        const auto gate = entry.gate.load(std::memory_order_seq_cst); // after the announcement (see csSlow)
        // The fields are read after the gate and trusted only as of an odd gate (they change only while it is even). A way
        // keyed to this address when the filter above read it cannot be re-keyed before this call ends: a re-keying waits
        // for the announcement just made. The registry compare tells two registries' sections at one guest address apart;
        // the host compare keeps this function right for a caller whose base is not that registry's (the bridge has
        // already refused such a call).
        if ((gate & 1) && entry.registry.load(std::memory_order_relaxed) == registry &&
                entry.host.load(std::memory_order_relaxed) == base + address) { RECOMP_NATIVE_SCHEDULE_POINT(); return &entry; }
    }
    th.inflight.store(0, std::memory_order_release);
    return nullptr;
}
[[gnu::always_inline]] inline bool nativeHeaderMatches(const NativeCsEntry& entry, const std::uint8_t* section) noexcept {
#if defined(RECOMP_NATIVE_MUTANT_NO_HEADER_CHECK) // (mutant: a changed dispatcher header is not a storage mismatch)
    (void)entry; (void)section; return true;
#else
    std::uint64_t a, b; std::memcpy(&a, section, 8); std::memcpy(&b, section + 8, 8);
    return a == entry.header[0] && b == entry.header[1];
#endif
}

// Slow -> fast, by a fast call that found no odd gate. Under the address's stripe: no v197 stripe path on this address and
// no CsAll holder (the structure of both maps, every record) runs now.
[[gnu::noinline]] bool nativeCsPromote(GuestImportRegistry& registry, std::uint8_t* base, std::uint32_t address) noexcept {
    std::unique_lock stripe(csStripeLocks[csStripe(address)]);
    const auto refused = [] { nativePromoteRefused.fetch_add(1, std::memory_order_relaxed); return false; };
    auto& sections = registry.*member(nat::Sections{});
    const auto section = sections.find(address);
    if (section == sections.end()) return refused(); // new or to be adopted: the locked path
#if !defined(RECOMP_NATIVE_MUTANT_PROMOTE_WITH_WAITERS) // (mutant: a section with parked enters goes on the fast path; its leaves wake nobody)
    if ((registry.*member(nat::SectionWaits{})).count(address) != 0) return refused();
#endif
#if !defined(RECOMP_NATIVE_MUTANT_PROMOTE_EDRAM) // (mutant: the EDRAM calibration lock goes on the fast path)
    if (address == registry.*member(nat::EdramLock{})) return refused();
#endif
    auto* const space = registry.*member(nat::Space{});
    if (space == nullptr || space->translate(address) != base + address) return refused();
    std::lock_guard table(nativeCsTableLock);
    int index = nativeCsLookup(&registry, address);
    if (index >= 0 && (nativeCsTable[index].gate.load(std::memory_order_seq_cst) & 1)) return true; // promoted by another thread meanwhile
    // The registry's own check (header, three words, record). Its csSlow finds this section's gate even or no entry.
    if (!(registry.*member(nat::StorageMatches{}))(address, section->second)) return refused();
    const unsigned set = nativeCsSet(address);
    for (unsigned way = 0; way < NativeCsWays && index < 0; ++way)
        if (nativeCsKeys[set + way].load(std::memory_order_relaxed) == 0) index = static_cast<int>(set + way);
    if (index < 0) { // evict (round robin): the victim goes back to the locked paths, like a section never promoted
        index = static_cast<int>(set + nativeCsClock[set / NativeCsWays]++ % NativeCsWays);
        const auto victim = nativeCsKeys[index].load(std::memory_order_relaxed);
#if !defined(RECOMP_NATIVE_MUTANT_EVICT_NO_MARKER) // (mutant: a locked path arriving for the victim now does not wait for its fast calls)
        nativeCsEvicting.store(victim, std::memory_order_seq_cst); // (before the gate changes: csSlow of the victim then waits too)
#endif
        (void)nativeCsDemote(nativeCsTable[index]);
        RECOMP_NATIVE_SCHEDULE_POINT();
#if !defined(RECOMP_NATIVE_MUTANT_EVICT_NO_QUIET) // (mutant: the way is given to another section while fast calls still work on the victim)
        nativeCsQuiet(victim);
#endif
        nativeCsClearWay(static_cast<unsigned>(index));
        nativeCsEvicting.store(0, std::memory_order_seq_cst);
        nativeEvicted.fetch_add(1, std::memory_order_relaxed);
    }
    // Written with the gate even (no reader takes the fields), the key last, then the gate.
    auto& entry = nativeCsTable[index];
    nativeCsKeys[index].store(0, std::memory_order_seq_cst);
    entry.registry.store(&registry, std::memory_order_seq_cst);
    entry.state.store(&section->second, std::memory_order_relaxed);
    entry.host.store(base + address, std::memory_order_relaxed);
    std::memcpy(entry.header, section->second.dispatcherHeader.data(), sizeof entry.header);
    static_assert(sizeof entry.header == sizeof section->second.dispatcherHeader);
    nativeCsKeys[index].store(address, std::memory_order_seq_cst);
    RECOMP_NATIVE_SCHEDULE_POINT();
    nativeCsUsed.store(true, std::memory_order_seq_cst);
    entry.gate.store(entry.gate.load(std::memory_order_relaxed) + 1, std::memory_order_seq_cst); // even -> odd (only this lock's holders raise an even gate)
    nativePromoted.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// v197's admission conditions, shared by enter and leave (anything else is the locked path's). `enter` adds the recorded
// rejection, as there.
[[gnu::always_inline]] inline std::uint32_t nativeCsAdmit(const NativeThread& th, GuestImportRegistry* registry,
        std::uint32_t address, bool enter) noexcept {
    if (!csFastEnabled() || registry->*member(nat::Space{}) == nullptr) return 0;
    const auto token = nativeToken(th, registry);
    if (token == 0 || address == 0 || (address & 3) != 0) return 0;
#if !defined(RECOMP_NATIVE_MUTANT_IGNORE_REJECTION) // (mutant: enters stay on the fast path while a rejection is recorded)
    if (enter && csRejectionRecorded.load(std::memory_order_acquire)) return 0;
#endif
#if !defined(RECOMP_NATIVE_MUTANT_PROMOTE_EDRAM)
    if (address == registry->*member(nat::EdramLock{})) return 0;
#endif
#if !defined(RECOMP_NATIVE_MUTANT_IGNORE_EXCLUSIVE) // (mutant: an open exclusive scope does not send the call to the locked path)
    if (guestExclusiveScopeActive()) return 0;
#endif
    return token;
}

[[gnu::hot]] bool nativeCsEnter(GuestImportRegistry* registry, std::uint8_t* base, std::uint32_t address) noexcept {
    NativeThread* const th = nativeThread();
    if (th == nullptr) return false;
    const auto token = nativeCsAdmit(*th, registry, address, true);
    if (token == 0) return nativeMiss(*th, NativeCsFallback);
    NativeCsEntry* entry = nativeCsOpen(*th, registry, base, address);
    if (entry == nullptr) {
        if (!nativeCsPromote(*registry, base, address) || (entry = nativeCsOpen(*th, registry, base, address)) == nullptr)
            return nativeMiss(*th, NativeCsFallback);
    }
    std::uint8_t* const section = base + address;
    NativeCsState& record = *entry->state.load(std::memory_order_relaxed);
    const auto owner = nativeLoad(section + CriticalSectionOwningThreadOffset);
    bool done = false;
    if (owner == 0) {
        // Free: the storage must look free before the owner word is touched (nothing is written into storage that no
        // longer is this section), then the word is taken, then the registry's record is checked by its new owner.
        std::uint32_t expected = 0;
        if (nativeHeaderMatches(*entry, section) && nativeLoad(section + CriticalSectionLockCountOffset) == 0xFFFFFFFFu &&
                nativeLoad(section + CriticalSectionRecursionCountOffset) == 0 &&
#if defined(RECOMP_NATIVE_MUTANT_NO_CAS) // (mutant: two enters can both take a free section)
                (nativeStore(section + CriticalSectionOwningThreadOffset, token), true)) {
#elif defined(RECOMP_NATIVE_MUTANT_WRONG_WORD) // (mutant: the compare-and-swap is on RecursionCount, not OwningThread)
                __atomic_compare_exchange_n(reinterpret_cast<std::uint32_t*>(section + CriticalSectionRecursionCountOffset), &expected,
                        __builtin_bswap32(token), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
#else
                __atomic_compare_exchange_n(reinterpret_cast<std::uint32_t*>(section + CriticalSectionOwningThreadOffset), &expected,
                        __builtin_bswap32(token), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
#endif
            RECOMP_NATIVE_SCHEDULE_POINT();
            if (__atomic_load_n(&record.ownerToken, __ATOMIC_RELAXED) == 0 && __atomic_load_n(&record.recursionCount, __ATOMIC_RELAXED) == 0) {
                nativeStore(section + CriticalSectionRecursionCountOffset, 1);
                nativeStore(section + CriticalSectionLockCountOffset, 0);
                __atomic_store_n(&record.recursionCount, 1u, __ATOMIC_RELAXED);
                __atomic_store_n(&record.ownerToken, token, __ATOMIC_RELAXED);
                done = true;
            } else { // the record disagrees: give the word back; the locked path finds and reports the mismatch
                __atomic_store_n(reinterpret_cast<std::uint32_t*>(section + CriticalSectionOwningThreadOffset), 0u, __ATOMIC_SEQ_CST);
            }
        }
    } else if (owner == token) { // recursive: only the owner writes the counts and the record
        const auto recursion = __atomic_load_n(&record.recursionCount, __ATOMIC_RELAXED);
        if (__atomic_load_n(&record.ownerToken, __ATOMIC_RELAXED) == token && recursion != 0 &&
#if !defined(RECOMP_NATIVE_MUTANT_NO_RECURSION_LIMIT) // (mutant: the count may pass the signed maximum)
                recursion != MaximumCriticalSectionRecursion &&
#endif
                nativeHeaderMatches(*entry, section) && nativeLoad(section + CriticalSectionRecursionCountOffset) == recursion &&
                nativeLoad(section + CriticalSectionLockCountOffset) == recursion - 1U) {
            nativeStore(section + CriticalSectionLockCountOffset, recursion);
            nativeStore(section + CriticalSectionRecursionCountOffset, recursion + 1U);
            __atomic_store_n(&record.recursionCount, recursion + 1U, __ATOMIC_RELAXED);
            done = true;
        }
    } // else owned by another thread: the locked path queues and parks
    th->inflight.store(0, std::memory_order_release);
    if (!done) return nativeMiss(*th, NativeCsFallback);
    nativeCount(*th, NativeCsEnterFast);
    return true;
}

[[gnu::hot]] bool nativeCsLeave(GuestImportRegistry* registry, std::uint8_t* base, std::uint32_t address) noexcept {
    NativeThread* const th = nativeThread();
    if (th == nullptr) return false;
    const auto token = nativeCsAdmit(*th, registry, address, false);
    if (token == 0) return nativeMiss(*th, NativeCsFallback);
    NativeCsEntry* entry = nativeCsOpen(*th, registry, base, address);
    if (entry == nullptr) {
        if (!nativeCsPromote(*registry, base, address) || (entry = nativeCsOpen(*th, registry, base, address)) == nullptr)
            return nativeMiss(*th, NativeCsFallback);
    }
    std::uint8_t* const section = base + address;
    NativeCsState& record = *entry->state.load(std::memory_order_relaxed);
    const auto recursion = __atomic_load_n(&record.recursionCount, __ATOMIC_RELAXED);
    bool done = false;
    // The owner check is on the guest word (the truth) AND on the record (today's check): only the owner goes on. On the
    // fast path the section has no waiter, so v197's "last level with a queue" case cannot arise.
    if (
#if !defined(RECOMP_NATIVE_MUTANT_NO_OWNER_CHECK) // (mutant: any thread may leave a section)
            nativeLoad(section + CriticalSectionOwningThreadOffset) == token &&
            __atomic_load_n(&record.ownerToken, __ATOMIC_RELAXED) == token &&
#endif
            recursion != 0 && nativeHeaderMatches(*entry, section) &&
            nativeLoad(section + CriticalSectionRecursionCountOffset) == recursion &&
            nativeLoad(section + CriticalSectionLockCountOffset) == recursion - 1U) {
        if (recursion != 1) {
            nativeStore(section + CriticalSectionLockCountOffset, recursion - 2U);
#if !defined(RECOMP_NATIVE_MUTANT_NO_RECURSION_RESTORE) // (mutant: a nested leave does not give its level back)
            nativeStore(section + CriticalSectionRecursionCountOffset, recursion - 1U);
            __atomic_store_n(&record.recursionCount, recursion - 1U, __ATOMIC_RELAXED);
#endif
        } else { // the last level: the counts and the record say "free" before the owner word does
            nativeStore(section + CriticalSectionLockCountOffset, 0xFFFFFFFFu);
            nativeStore(section + CriticalSectionRecursionCountOffset, 0);
            __atomic_store_n(&record.recursionCount, 0u, __ATOMIC_RELAXED);
            __atomic_store_n(&record.ownerToken, 0u, __ATOMIC_RELAXED);
            RECOMP_NATIVE_SCHEDULE_POINT();
            __atomic_store_n(reinterpret_cast<std::uint32_t*>(section + CriticalSectionOwningThreadOffset), 0u, __ATOMIC_SEQ_CST);
        }
        done = true;
    }
    th->inflight.store(0, std::memory_order_release);
    if (!done) return nativeMiss(*th, NativeCsFallback);
    nativeCount(*th, NativeCsLeaveFast);
    return true;
}

// KeTlsGetValue: v252's lock-free repeat in dispatch(), condition for condition, with the caller's cache and binding
// reached through the thread record.
[[gnu::hot]] bool nativeTlsGet(GuestImportRegistry* registry, std::uint32_t index, std::uint64_t& value) noexcept {
    NativeThread* const th = nativeThread();
    if (th == nullptr) return false;
    if (v252Baseline() || registry->*member(nat::Space{}) == nullptr) return nativeMiss(*th, NativeOtherFallback);
    const auto& c = *th->tlsRead;
    const auto token = nativeToken(*th, registry);
    if (token != 0 && c.registry == registry && c.token == token && index < c.slotCount && index < *member(nat::TlsSlots{})) {
        const auto* mirror = tlsSlotMirror(registry, false);
        const auto* slot = mirror ? registryFindSlot(registry) : nullptr;
#if defined(RECOMP_NATIVE_MUTANT_TLS_NO_SLOT_BIT) // (mutant: a freed slot still reads its old value)
        if (slot) {
#else
        if (slot && ((mirror->bits[index / 64].load(std::memory_order_acquire) >> (index % 64)) & 1U)) {
#endif
            const auto word = slot->word.load(std::memory_order_acquire);
            std::uint32_t read = 0;
#if defined(RECOMP_NATIVE_MUTANT_TLS_NO_GENERATION) // (mutant: the cached storage answers after the registry's state changed)
            if ((registry->*member(nat::ReadU32{}))(c.base + index * 4U, read)) {
#else
            if ((word & RegistryWindowMask) == 0 && (word >> 16) == c.generation &&
                    (registry->*member(nat::ReadU32{}))(c.base + index * 4U, read)) {
#endif
                value = read;
                nativeCount(*th, NativeTlsFast);
                return true;
            }
        }
    }
    return nativeMiss(*th, NativeOtherFallback);
}

// The four handle imports: the import's own handler under the registry lock, as dispatch() + dispatchUnderRegistryLock
// run it today for a hot import with diag-imports-fast on, minus the name tests. Every step those two functions take for
// such an import before its handler is taken here too or sends the call back (false) before anything was changed:
//   the wake collector (RegistryWakes, delivered after the lock is released); the import-records diagnostic (a start-up
//   switch: with it on, the old path); the quiet registry section; the address-space check; the v282 and spinlock names
//   (not these imports); the v160 stack diagnostic (skipped for hot imports); the saves include (which handles none of the
//   four; when it could still create the saves state, the old path); the three native-scope refusals (only for the
//   scope's own worker: the old path). The ~150 name tests in between are false for these names and have no other effect
//   (the lane's tests/source_check.py re-checks that on the source).
// A handler result that is not "handled" (a refusal, or a wait that has to park) changed nothing but, for a wait, the
// refresh of a due timer, which every poll repeats anyway: the old path then runs the whole call and reports or parks.
[[gnu::hot]] bool nativeLockedCall(GuestImportRegistry* registry, ::recomp::native_imports::Locked which,
        const std::uint64_t* arguments, std::uint32_t stackPointer, std::uint64_t& returnValue) {
    NativeThread* const th = nativeThread();
    if (th == nullptr) return false;
    static const bool records = ::access(RECOMP_APP_PATH("/private-trace/diag-import-records"), F_OK) == 0;
    const auto token = nativeToken(*th, registry);
    if (!::recomp::imports_fast::on() || records || token == 0) return nativeMiss(*th, NativeOtherFallback);
    GuestImportCall call{};
    call.stackPointer = stackPointer;
    for (std::size_t i = 0; i < call.arguments.size(); ++i) call.arguments[i] = arguments[i];
    bool handled = false;
    {
        [[maybe_unused]] RegistryWakes wakes; // (declared first: destroyed, and its wakes delivered, after the section)
        static RegistryLockSite sites[::recomp::native_imports::LockedCount] = {{"__imp__NtSetEvent", 0},
                {"__imp__NtClearEvent", 0}, {"__imp__NtReleaseSemaphore", 0}, {"__imp__NtWaitForSingleObjectEx", 0}};
#if !defined(RECOMP_NATIVE_MUTANT_LOCKED_NO_LOCK) // (mutant: the handler runs without the registry lock)
        RegistryUniqueQuiet section(*th->registryState, registry, sites[which]);
#endif
        auto* const space = registry->*member(nat::Space{});
        const auto& scene = registry->*member(nat::SceneScope{});
        const auto& tileLock = registry->*member(nat::TileLockScope{});
        const auto& interrupt = registry->*member(nat::InterruptScope{});
        if (space != nullptr && space->isReserved() &&
                (!(registry->*member(nat::XamAddress{}) != 0 && registry->*member(nat::SignedOutUsers{})) || savesStateSettled(registry)) &&
#if !defined(RECOMP_NATIVE_MUTANT_LOCKED_NO_SCOPE_CHECK) // (mutant: a native scope's worker is not sent to the old path's refusals)
                !(scene && token == scene->worker) && !(tileLock && token == tileLock->worker) &&
                !(interrupt && token == interrupt->worker) &&
#endif
                true) {
            switch (which) {
            case ::recomp::native_imports::LockedNtSetEvent:
                handled = (registry->*member(nat::SetEvent{}))(call).handled(); break;
            case ::recomp::native_imports::LockedNtClearEvent:
                handled = (registry->*member(nat::ClearEvent{}))(call).handled(); break;
            case ::recomp::native_imports::LockedNtReleaseSemaphore:
                handled = (registry->*member(nat::ReleaseSemaphore{}))(call).handled(); break;
            case ::recomp::native_imports::LockedNtWaitForSingleObjectEx: {
                bool pending = false; std::uint64_t relativeTicks = 0;
                handled = (registry->*member(nat::WaitSingle{}))(call, pending, relativeTicks).handled();
#if defined(RECOMP_NATIVE_MUTANT_LOCKED_PENDING_DONE) // (mutant: a wait that has to park returns at once)
                handled = handled || pending;
#endif
                break; }
            default: break;
            }
        }
    }
    if (!handled) return nativeMiss(*th, NativeOtherFallback);
    returnValue = call.returnValue;
    nativeCount(*th, which == ::recomp::native_imports::LockedNtReleaseSemaphore ? NativeSemFast
            : which == ::recomp::native_imports::LockedNtWaitForSingleObjectEx ? NativeWaitFast : NativeEventFast);
    return true;
}

} // namespace
} // namespace recomp::bootstrap

namespace recomp::native_imports {
bool csEnter(bootstrap::GuestImportRegistry* registry, std::uint8_t* base, std::uint32_t address) noexcept {
    return bootstrap::nativeCsEnter(registry, base, address);
}
bool csLeave(bootstrap::GuestImportRegistry* registry, std::uint8_t* base, std::uint32_t address) noexcept {
    return bootstrap::nativeCsLeave(registry, base, address);
}
bool tlsGet(bootstrap::GuestImportRegistry* registry, std::uint8_t*, std::uint32_t index, std::uint64_t& value) noexcept {
    return bootstrap::nativeTlsGet(registry, index, value);
}
bool lockedCall(bootstrap::GuestImportRegistry* registry, Locked which, const std::uint64_t* arguments,
                std::uint32_t stackPointer, std::uint64_t& returnValue) {
    return bootstrap::nativeLockedCall(registry, which, arguments, stackPointer, returnValue);
}
[[gnu::noinline]] void csSlow(const bootstrap::GuestImportRegistry* registry, std::uint32_t address) noexcept {
    using namespace bootstrap;
#if defined(RECOMP_NATIVE_MUTANT_NO_TAKEOVER) // (mutant: the locked paths use a section while fast calls still work on it)
    (void)registry; (void)address; return;
#else
    if (!nativeCsUsed.load(std::memory_order_seq_cst)) return; // no section is on the fast path: v524's path exactly
    if (address == 0) return; // (no section lives at 0: an unset EDRAM calibration lock asks for it; 0 also means "free way" and "no eviction")
    // The gate is made even under the table lock, where the entry's key cannot change (an eviction by a promotion of
    // another address could otherwise re-key the way between the look-up and the change, and this call would take a
    // different section off the fast path without waiting for ITS fast calls). The lock is taken only when the look-up
    // finds an odd gate. (nativeCsPromote calls the storage match, and so this function, with the table lock held: it
    // does so only for a section whose gate is even, and it is the only raiser of gates.)
    bool wait = false;
    if (const int seen = nativeCsLookup(registry, address); seen >= 0 && (nativeCsTable[seen].gate.load(std::memory_order_seq_cst) & 1)) {
        RECOMP_NATIVE_SCHEDULE_POINT();
#if defined(RECOMP_NATIVE_MUTANT_SLOW_NO_TABLE_LOCK) // (mutant: the way found a moment ago is taken off the fast path, whatever section it holds now)
        wait = nativeCsDemote(nativeCsTable[seen]);
#else
        std::lock_guard table(nativeCsTableLock);
        if (const int index = nativeCsLookup(registry, address); index >= 0) wait = nativeCsDemote(nativeCsTable[index]);
#endif
    }
    RECOMP_NATIVE_SCHEDULE_POINT();
    // Wait when this call made the gate even, and when an eviction or the window poll's clearing is doing so for this
    // address right now (it sets the marker before it changes the gate, and clears the key only after its own wait). An
    // even gate found otherwise was made even, and waited out, by an earlier holder of this stripe or of the table lock;
    // no fast call has passed it since.
    if (wait || nativeCsEvicting.load(std::memory_order_seq_cst) == address) nativeCsQuiet(address);
#endif
}
[[gnu::noinline]] void csRetire(const bootstrap::GuestImportRegistry* registry, std::uint32_t address) noexcept {
    using namespace bootstrap;
    if (!nativeCsUsed.load(std::memory_order_seq_cst)) return;
    // Off the fast path (today's only eraser, RtlInitializeCriticalSection, has already done that through its "active
    // section in range" check; this keeps it true for any eraser), then the way is freed for another section. An entry's
    // fields are read only under an odd gate, and promotion rewrites them all before it makes the gate odd, so the stale
    // record pointer of an entry left behind could never be read: clearing it is housekeeping, not a rule of its own.
    csSlow(registry, address);
    std::lock_guard table(nativeCsTableLock);
    // (its gate is even and stays so: promotion needs the stripe the caller holds)
    if (const int index = nativeCsLookup(registry, address); index >= 0) nativeCsClearWay(static_cast<unsigned>(index));
}
[[gnu::noinline]] void csRetireAll(const bootstrap::GuestImportRegistry* registry) noexcept {
    using namespace bootstrap;
    if (!nativeCsUsed.load(std::memory_order_seq_cst)) return;
#if !defined(RECOMP_NATIVE_MUTANT_NO_RETIRE_ALL) // (mutant: sections of a cleared registry stay on the fast path, with pointers to their erased records)
    std::lock_guard table(nativeCsTableLock);
    nativeCsClearAll(registry);
#endif
}
// The window poll, with the switch off: empty the table, so that the locked paths' hooks cost one load again. A fast call
// that read the switch just before it went off may promote a section afterwards; the next poll empties the table again.
static void csIdle() noexcept {
    using namespace bootstrap;
    if (!nativeCsUsed.load(std::memory_order_seq_cst)) return;
    std::lock_guard table(nativeCsTableLock);
    nativeCsClearAll(nullptr);
    nativeCsUsed.store(false, std::memory_order_seq_cst); // (under the lock every promotion takes before it raises a gate)
}
Totals totals() noexcept {
    using namespace bootstrap;
    Totals t{};
    for (auto* th = nativeThreads.load(std::memory_order_seq_cst); th != nullptr; th = th->next)
        for (unsigned i = 0; i < NativeCounterCount; ++i) t.counters[i] += th->counters[i].load(std::memory_order_relaxed);
    t.promoted = nativePromoted.load(std::memory_order_relaxed); t.takenOver = nativeTakenOver.load(std::memory_order_relaxed);
    t.evicted = nativeEvicted.load(std::memory_order_relaxed); t.promoteRefused = nativePromoteRefused.load(std::memory_order_relaxed);
    t.unregistered = nativeUnregistered.load(std::memory_order_relaxed);
    return t;
}
} // namespace recomp::native_imports
#else // cooperative builds: no fast path (the calling thread has no binding of its own)
namespace recomp::native_imports {
bool csEnter(bootstrap::GuestImportRegistry*, std::uint8_t*, std::uint32_t) noexcept { return false; }
bool csLeave(bootstrap::GuestImportRegistry*, std::uint8_t*, std::uint32_t) noexcept { return false; }
bool tlsGet(bootstrap::GuestImportRegistry*, std::uint8_t*, std::uint32_t, std::uint64_t&) noexcept { return false; }
bool lockedCall(bootstrap::GuestImportRegistry*, Locked, const std::uint64_t*, std::uint32_t, std::uint64_t&) { return false; }
void csSlow(const bootstrap::GuestImportRegistry*, std::uint32_t) noexcept {}
void csRetire(const bootstrap::GuestImportRegistry*, std::uint32_t) noexcept {}
void csRetireAll(const bootstrap::GuestImportRegistry*) noexcept {}
Totals totals() noexcept { return {}; }
static void csIdle() noexcept {}
} // namespace recomp::native_imports
#endif

// The renderer's window poll (native_imports.h). Out of line there and here: the poll keeps its control flow.
#if defined(RECOMP_DIAG_SWITCH_DIR)
#define RECOMP_NATIVE_IMPORTS_SWITCH(name) RECOMP_DIAG_SWITCH(name) // host tests
#else
#define RECOMP_NATIVE_IMPORTS_SWITCH(name) RECOMP_APP_PATH("/private-trace/" name)
#endif
namespace recomp::native_imports {
[[gnu::noinline]] void window() noexcept {
    static bool file = false, only = false;
    static Totals last{};
    const bool ran = file, ranOnly = only; // what the window that just ended ran under (set at its start)
    file = ::access(RECOMP_NATIVE_IMPORTS_SWITCH("diag-native-imports"), F_OK) == 0;
    only = ::access(RECOMP_NATIVE_IMPORTS_SWITCH("diag-native-imports-cs-only"), F_OK) == 0;
    ::recomp_native_imports_locked.store(!only, std::memory_order_relaxed);
    ::recomp_native_imports.store(file, std::memory_order_relaxed);
    ::recomp_native_clock_quiet.store(file, std::memory_order_relaxed); // native_clock_quiet.h: the executor's accounting stamps
    if (!file) csIdle();
    const Totals now = totals();
    const auto d = [&](unsigned i) { return now.counters[i] - last.counters[i]; };
    __android_log_print(ANDROID_LOG_INFO, "RecompRenderer", "RENDER_GENERIC_V210Z native_imports=%u native_imports_cs_only=%u",
            unsigned(ran && file), unsigned(ranOnly && only));
    __android_log_print(ANDROID_LOG_INFO, "RecompRenderer",
            "RENDER_GENERIC_NATIVE imports=%u stable=%u cs_enter_fast=%llu cs_leave_fast=%llu cs_fallback=%llu tls_fast=%llu "
            "event_fast=%llu sem_fast=%llu wait_fast=%llu other_fallback=%llu",
            unsigned(ran), unsigned(ran == file && ranOnly == only), d(0), d(1), d(2), d(3), d(4), d(5), d(6), d(7));
    __android_log_print(ANDROID_LOG_INFO, "RecompRenderer",
            "RENDER_DIAG_NATIVE cs_only=%u promoted=%llu taken_over=%llu evicted=%llu promote_refused=%llu unregistered=%llu",
            unsigned(ranOnly), now.promoted - last.promoted, now.takenOver - last.takenOver, now.evicted - last.evicted,
            now.promoteRefused - last.promoteRefused, now.unregistered - last.unregistered);
    last = now;
}
} // namespace recomp::native_imports
