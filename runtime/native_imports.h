#pragma once
// native-imports (lane native-syscalls, attempt v526, 2026-10-11): the game's hot system calls bound by import id to native
// fast functions, behind the in-run switch files/private-trace/diag-native-imports -> extern "C" std::atomic<bool>
// recomp_native_imports (strong definition in guest_import_registry.cpp through native_imports.inc; the renderer's
// 60-present window poll sets it by calling window() below; default off = v524's path, instruction for instruction: every
// bridge stub then goes straight to dispatchBoundedImport as before, and the registry's slow-path hooks return at once).
//
// What a bridge stub does with the switch on (generated_guest_import_bridge.cpp, written by the lane's
// tools/bridge_native_pass.py): it calls the import's fast function FIRST, by a direct C++ call, with the arguments it
// needs taken from the guest registers; no import name, no GuestImportCall for the lock-free ones and no entry into
// GuestImportRegistry::dispatch(). A fast function either completes the call with exactly the guest-visible effect of
// today's handler and returns true, or changes nothing and returns false; then the stub goes on exactly as in v524.
//
//   csEnter / csLeave   RtlEnterCriticalSection / RtlLeaveCriticalSection of an uncontended section: no host lock, no map
//                       lookup, no thread_local resolver call. The section's guest OwningThread word is taken and given
//                       back by compare-and-swap; see native_imports.inc for the invariant and the hand-over to the
//                       registry's locked paths (contention, waiters, recursion limit, the EDRAM calibration lock, an
//                       open exclusive scope, any storage mismatch, a section the registry does not know yet).
//   tlsGet              KeTlsGetValue: v252's lock-free repeat (the same checks on the same words), reached without the
//                       name tests and with the calling thread's state found without the resolver.
//   lockedCall          NtSetEvent, NtClearEvent, NtReleaseSemaphore and an NtWaitForSingleObjectEx that does not have to
//                       park: the import's OWN handler, called directly under the registry lock in the same quiet section
//                       as today (diag-imports-fast). These still take the lock: the objects live only in the registry's
//                       handle map, and a signal and a parking waiter are ordered by that lock (native_imports.inc says
//                       what a lock-free form would have to move). files/private-trace/diag-native-imports-cs-only
//                       leaves these four on the old path (recomp_native_imports_locked = false).
//
// Guest-visible state is the same in both settings: the same values in the same guest words, the same return values, the
// same wake-ups. Only how a thread gets there changes.
#include <atomic>
#include <cstdint>
#include "native_clock_quiet.h" // recomp_native_clock_quiet: the executor's accounting stamps (set by window() below)

namespace recomp::bootstrap { class GuestImportRegistry; }

// Host tests only (the lane's tests/build.sh "sp" variants): a pause at each point where another thread's next step decides
// whether the hand-over between the fast path and the locked paths holds (a yield, and every sixteenth time a sleep long
// enough for a locked path to run to its end), so that the stress tests meet interleavings a few instructions wide.
// Nothing in any other build.
#if defined(RECOMP_NATIVE_TEST_SCHEDULE_POINTS)
namespace recomp::native_imports { void testPause() noexcept; }
#define RECOMP_NATIVE_SCHEDULE_POINT() (::recomp::native_imports::testPause())
#else
#define RECOMP_NATIVE_SCHEDULE_POINT() ((void)0)
#endif

extern "C" std::atomic<bool> recomp_native_imports;        // diag-native-imports (the whole feature)
extern "C" std::atomic<bool> recomp_native_imports_locked; // true unless diag-native-imports-cs-only exists

namespace recomp::native_imports {

[[gnu::always_inline]] inline bool on() noexcept { return ::recomp_native_imports.load(std::memory_order_relaxed); }
[[gnu::always_inline]] inline bool lockedOn() noexcept { return ::recomp_native_imports_locked.load(std::memory_order_relaxed); }

// The four imports whose own handler lockedCall runs (the order of its site table and of the bridge's calls).
enum Locked : unsigned { LockedNtSetEvent, LockedNtClearEvent, LockedNtReleaseSemaphore, LockedNtWaitForSingleObjectEx, LockedCount };

// Fast functions. `base` is the guest base the chunk passed (the bridge has checked it against the bound one).
// true: the call is complete. false: nothing guest-visible was changed; the caller takes the old path.
bool csEnter(bootstrap::GuestImportRegistry* registry, std::uint8_t* base, std::uint32_t address) noexcept;
bool csLeave(bootstrap::GuestImportRegistry* registry, std::uint8_t* base, std::uint32_t address) noexcept;
bool tlsGet(bootstrap::GuestImportRegistry* registry, std::uint8_t* base, std::uint32_t index, std::uint64_t& value) noexcept;
// arguments = the eight integer argument registers r3..r10 (copied by the stub), stackPointer = r1.
bool lockedCall(bootstrap::GuestImportRegistry* registry, Locked which, const std::uint64_t* arguments,
                std::uint32_t stackPointer, std::uint64_t& returnValue);

// Registry slow-path hooks (guest_import_registry.cpp only; each returns at once while no section was ever handed to the
// fast path). The caller holds the address's stripe or all stripes (CsAll).
//   csSlow       before the registry reads or writes its record of the section at `address`: the section leaves the fast
//                path (if it was on it) and every fast call still working on it has finished when this returns.
//   csRetire     before the registry erases its record of the section at `address` (csSlow, then the entry is dropped).
//   csRetireAll  before the registry clears all its sections (initialize / reset).
void csSlow(const bootstrap::GuestImportRegistry* registry, std::uint32_t address) noexcept;
void csRetire(const bootstrap::GuestImportRegistry* registry, std::uint32_t address) noexcept;
void csRetireAll(const bootstrap::GuestImportRegistry* registry) noexcept;

// The renderer's 60-present window poll: reads the switch files, publishes them, and logs
//   RENDER_GENERIC_V210Z native_imports=<1 when the file was present for the whole window> native_imports_cs_only=<same>
//   RENDER_GENERIC_NATIVE imports=<state the window ran under> stable=<1 when the next window keeps it> cs_enter_fast=<n>
//       cs_leave_fast=<n> cs_fallback=<n> tls_fast=<n> event_fast=<n> sem_fast=<n> wait_fast=<n> other_fallback=<n>
//   RENDER_DIAG_NATIVE (the fast path's own bookkeeping: sections handed over each way, table use, threads)
// The counts are the window's (differences of per-thread counters that only their own thread writes).
void window() noexcept;

// The counters since the start (the order of the RENDER_GENERIC_NATIVE fields), then the fast path's bookkeeping: sections
// put on it, taken off it, evicted from the table, promotions refused, calls from threads without a record (host tests).
struct Totals { unsigned long long counters[8]; unsigned long long promoted, takenOver, evicted, promoteRefused, unregistered; };
Totals totals() noexcept;

} // namespace recomp::native_imports
