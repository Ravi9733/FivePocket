#include "guest_execution_guard.h"
#include "guest_parallel_wake.h" // v119: wakeAllGuestWaiters

#include <algorithm>
#include <android/log.h>
#include <cstdio>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <typeinfo>
#include <utility>

namespace recomp::bootstrap {
namespace {

// Multicore plan WP4 (local-analysis/parallel-design/plan.md): RECOMP_GUEST_THREADS selects the
// guest-thread model; 0 or undefined is the cooperative executor, whose behaviour is unchanged.
#if defined(RECOMP_GUEST_THREADS) && RECOMP_GUEST_THREADS != 0
constexpr bool ParallelGuestThreads = true;
#else
constexpr bool ParallelGuestThreads = false;
#endif

constexpr std::size_t MaximumImportNameBytes = 128;
constexpr std::size_t MaximumHostExceptionBytes = 192;

// Reporting only. The private trace protocol is line oriented and key=value,
// so an unsanitised what() could inject newlines or forge a protocol key.
// Appends stay inside the caller's reserved capacity so a detail built while
// unwinding std::bad_alloc cannot itself allocate.
void appendBoundedDetail(std::string& out, const char* text) {
    if (!text) text = "?";
    for (; *text && out.size() < MaximumHostExceptionBytes; ++text) {
        const auto byte = static_cast<unsigned char>(*text);
        out.push_back(byte < 0x20 || byte > 0x7e ? '?' : *text);
    }
}

struct StopSignal final {};

struct ActiveExecution {
    std::uint64_t maximumFunctionEntries;
    std::uint64_t completedFunctionEntries{0};
    GuestExecutionStopReason reason{GuestExecutionStopReason::Completed};
    std::string detail;
    const GuestExecutionGuard::FunctionTraceSink* traceSink{nullptr};
    std::optional<GuestImportArguments> importArguments{};
    std::optional<GuestScalarAccess> scalarAccess{};
};

// One native handler reached from guest MMIO (CP_RB_WPTR submission, XMA registers). Re-entry
// on the same host thread is caught by a thread_local flag at the call site. Another host thread
// inside the handler is detected by an atomic owner flag (compare_exchange_strong never fails
// spuriously). Rejecting keeps the former global flag's fail-closed contract, which also covers a
// guest worker running while its submitter is parked inside the handler across a native IRQ
// handoff. enter() waits instead (bounded spin, then park); it is used only where the handler is
// thread-safe itself and never runs guest code. Not a leaf: it ranks above the registry mutex R
// (the XMA handler's map_ takes R while it is held), and no R holder ever performs guest MMIO.
class HandlerExclusion final {
public:
    bool tryEnter() noexcept {
        bool expected = false;
        return busy_.compare_exchange_strong(expected, true, std::memory_order_acquire, std::memory_order_relaxed);
    }
    void enter() {
        for (unsigned spins = 0; spins < 64; ++spins) {
            if (tryEnter()) return;
            if (spins >= 16) std::this_thread::yield();
        }
        std::unique_lock lock(parkLock_);
        parked_.wait(lock, [&] { return tryEnter(); });
    }
    void leave() {
        busy_.store(false, std::memory_order_release);
        if constexpr (ParallelGuestThreads) { std::lock_guard lock(parkLock_); parked_.notify_one(); }
    }
private:
    std::atomic<bool> busy_{false};
    std::mutex parkLock_;
    std::condition_variable parked_;
};

// Monotonic clamp shared by every guest thread. "No reading yet" is stored as 0: every unsigned
// reading is >= 0, so it accepts exactly what the former empty std::optional accepted.
struct ThreadClockHistory { std::uint64_t epoch{0}, last{0}; };
class MonotonicClock final {
public:
    void reset() noexcept {
        last_.store(0, std::memory_order_relaxed);
        epoch_.fetch_add(1, std::memory_order_relaxed);
    }
    // Cooperative: a reading below the last published one is a source error (false), the former
    // check. Multicore: readers on different threads may publish out of order, so a regression is
    // judged per host thread (still a source error), and a reading below another thread's
    // publication is raised to it: no guest thread observes time behind a value already returned
    // to another. `exposed` is the value to return to the guest.
    bool publish(std::uint64_t reading, std::uint64_t& exposed, ThreadClockHistory& mine) noexcept {
        if constexpr (ParallelGuestThreads) {
            const auto epoch = epoch_.load(std::memory_order_relaxed);
            if (mine.epoch != epoch) mine = {epoch, 0};
            if (reading < mine.last) return false;
            mine.last = reading;
        }
        auto previous = last_.load(std::memory_order_acquire);
        while (previous < reading && !last_.compare_exchange_weak(previous, reading,
                std::memory_order_acq_rel, std::memory_order_acquire)) {}
        if (previous > reading) {
            if constexpr (!ParallelGuestThreads) return false;
            exposed = previous;
            return true;
        }
        exposed = reading;
        return true;
    }
private:
    std::atomic<std::uint64_t> last_{0};
    std::atomic<std::uint64_t> epoch_{0};
};

thread_local ActiveExecution* activeExecution = nullptr;
std::vector<std::uint32_t> unboundDataSlots;
bool dataSlotsBound = false;
// Bound sources and handlers are written only by bind/reset, before guest workers start or after
// they join (safe publication by thread start/join); guest threads only read them.
std::uint32_t uptimeBundle = 0;
std::function<std::optional<std::uint64_t>()> uptimeSource;
MonotonicClock uptimeClock;
thread_local ThreadClockHistory uptimeHistory;
std::function<std::optional<std::uint64_t>()> experimentalGpuClockSource;
MonotonicClock experimentalGpuClock;
thread_local ThreadClockHistory experimentalGpuClockHistory;
std::atomic<std::uint64_t> experimentalGpuReads{0};
std::function<bool(std::uint32_t)> graphicsSubmissionHandler;
HandlerExclusion graphicsSubmissionExclusion;
thread_local bool graphicsSubmissionActive = false;
// v119: with the renderer callers in separate permit domains (diag-domain-split-render), a submitter that finds the
// handler busy parks through this waiter (the executor's waitUntil: a safepoint-visible park, so an exclusive span
// run by the holder is not blocked by it) instead of being rejected. Unbound (every other mode): v118's rejection.
// waitUntil refuses scopes and handoffs, so a native IRQ worker is still rejected exactly as before.
std::function<bool(const std::function<bool()>&)> graphicsSubmissionWaiter;
std::atomic<unsigned> graphicsSubmissionParked{0};
std::atomic<std::uint64_t> graphicsSubmissionParkedEntries{0};
std::function<bool(std::uint32_t,bool,std::uint32_t&)> xmaRegisterHandler;
HandlerExclusion xmaRegisterExclusion;
thread_local bool xmaRegisterActive = false;

bool touchesUptime(std::uint32_t address, std::uint32_t width) {
    return uptimeBundle && width && address < std::uint64_t{uptimeBundle} + 24 &&
        std::uint64_t{address} + width > uptimeBundle;
}

class ActiveExecutionScope final {
public:
    explicit ActiveExecutionScope(ActiveExecution& execution) noexcept
        : previous_(activeExecution) {
        activeExecution = &execution;
    }

    ~ActiveExecutionScope() {
        activeExecution = previous_;
    }

    ActiveExecutionScope(const ActiveExecutionScope&) = delete;
    ActiveExecutionScope& operator=(const ActiveExecutionScope&) = delete;

private:
    ActiveExecution* previous_;
};

std::string boundedDetail(std::string_view value) {
    const auto length = std::min(value.size(), MaximumImportNameBytes);
    return std::string(value.substr(0, length));
}

} // namespace

bool bindExperimentalGuestGpuClock(std::function<std::optional<std::uint64_t>()> source) {
    if (!source || experimentalGpuClockSource || activeExecution) return false;
    experimentalGpuClockSource = std::move(source);
    experimentalGpuClock.reset(); experimentalGpuReads.store(0, std::memory_order_relaxed);
    return true;
}

void resetExperimentalGuestGpuClock() noexcept {
    experimentalGpuClockSource = {}; experimentalGpuClock.reset(); experimentalGpuReads.store(0, std::memory_order_relaxed);
}

std::uint64_t experimentalGuestGpuClockReads() noexcept { return experimentalGpuReads.load(std::memory_order_relaxed); }

bool tryReadExperimentalGuestGpuClock(std::uint32_t address, std::uint32_t width, std::uint64_t& value) {
    if (!experimentalGpuClockSource || address != 0x7FC8112C || width != 4) return false;
    const auto ns = experimentalGpuClockSource();
    std::uint64_t exposed = 0;
    if (!ns || experimentalGpuReads.load(std::memory_order_relaxed) == UINT64_MAX ||
        !experimentalGpuClock.publish(*ns, exposed, experimentalGpuClockHistory)) return false;
    value = static_cast<std::uint32_t>(exposed / 2); // Hypothesized 500 MHz, modulo 2^32.
    experimentalGpuReads.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool bindGuestGraphicsSubmission(std::function<bool(std::uint32_t)> handler) {
    if (!handler || activeExecution || !graphicsSubmissionExclusion.tryEnter()) return false;
    const bool bound = !graphicsSubmissionHandler;
    if (bound) graphicsSubmissionHandler = std::move(handler);
    graphicsSubmissionExclusion.leave();
    return bound;
}

// v119: bound before guest workers start and cleared after they join (safe publication), like the handler.
bool bindGuestGraphicsSubmissionWaiter(std::function<bool(const std::function<bool()>&)> waiter) {
    if (activeExecution) return false;
    graphicsSubmissionWaiter = std::move(waiter);
    return true;
}
std::uint64_t guestGraphicsSubmissionParkedEntries() noexcept {
    return graphicsSubmissionParkedEntries.load(std::memory_order_relaxed);
}

void resetGuestGraphicsSubmission() noexcept {
    // Do not destroy a callable while any thread executes it. Lifecycle reset belongs
    // after all workers join, never inside a callback/guarded guest run.
    if (activeExecution || graphicsSubmissionActive || !graphicsSubmissionExclusion.tryEnter()) return;
    graphicsSubmissionHandler = {};
    graphicsSubmissionExclusion.leave();
}

bool tryWriteGuestGraphicsSubmission(std::uint32_t address, std::uint32_t width, std::uint64_t value) {
    if (!activeExecution || graphicsSubmissionActive ||
        address != 0x7FC80714 || width != 4 || value > UINT32_MAX) return false;
    // The handler chain is not thread-safe. Another host thread inside it (for example a native
    // IRQ worker running while its submitter is parked in the handler) is rejected in every mode,
    // exactly as the former global flag did.
    if (!graphicsSubmissionExclusion.tryEnter()) {
        if (!graphicsSubmissionWaiter) return false;
        bool mine = false; // the predicate may run several times; it enters at most once
        graphicsSubmissionParked.fetch_add(1, std::memory_order_acq_rel);
        bool ok = false;
        try { ok = graphicsSubmissionWaiter([&] { if (!mine) mine = graphicsSubmissionExclusion.tryEnter(); return mine; }); }
        catch (...) { graphicsSubmissionParked.fetch_sub(1, std::memory_order_acq_rel); if (mine) graphicsSubmissionExclusion.leave(); throw; }
        graphicsSubmissionParked.fetch_sub(1, std::memory_order_acq_rel);
        if (!ok || !mine) { if (mine) graphicsSubmissionExclusion.leave(); return false; }
        graphicsSubmissionParkedEntries.fetch_add(1, std::memory_order_relaxed);
    }
    bool accepted = false;
    if (graphicsSubmissionHandler) {
        graphicsSubmissionActive = true;
        try { accepted = graphicsSubmissionHandler(static_cast<std::uint32_t>(value)); } catch (...) {}
        graphicsSubmissionActive = false;
    }
    graphicsSubmissionExclusion.leave();
    if (graphicsSubmissionParked.load(std::memory_order_acquire)) wakeAllGuestWaiters(); // v119: a parked submitter retries now
    return accepted;
}

// v211: the deferred scene-IRQ deliverer (trace_runner.cpp) holds the submission exclusion while it delivers outside a
// submission, so no submission handler runs between a deferred interrupt's queued stores and the interrupt itself (a
// submitter that finds it busy parks through the bound waiter, as above).
bool tryEnterGuestGraphicsSubmissionExclusion() noexcept {return !graphicsSubmissionActive && graphicsSubmissionExclusion.tryEnter();}
void leaveGuestGraphicsSubmissionExclusion() noexcept {
    graphicsSubmissionExclusion.leave();
    if (graphicsSubmissionParked.load(std::memory_order_acquire)) wakeAllGuestWaiters();
}
bool insideGuestGraphicsSubmission() noexcept {return graphicsSubmissionActive;}

// The inline hull used by loadGuestScalar/storeGuestScalar to skip both special checks.
// v220: the generated chunks' constant-window fast path (attempt guest_scalar_memory.h, RECOMP_GUEST_SPECIAL_CONST_WINDOWS)
// sends every access to these two 4 KB windows down the exact cold path and skips the runtime range tests elsewhere, so
// every bound special range must lie inside one of them (same values as guestSpecialWindowA/B/Bytes there).
constexpr std::uint64_t SpecialWindowA = 0x82000000u, SpecialWindowB = 0x62004000u, SpecialWindowBytes = 0x1000u;
constexpr bool insideSpecialWindows(std::uint64_t lo, std::uint64_t hi) noexcept {
    return lo >= hi || (lo >= SpecialWindowA && hi <= SpecialWindowA + SpecialWindowBytes) ||
        (lo >= SpecialWindowB && hi <= SpecialWindowB + SpecialWindowBytes);
}
static_assert(insideSpecialWindows(0x820008DC, 0x82000A28) && insideSpecialWindows(0x62004200, 0x62004218) &&
              !insideSpecialWindows(0x82000FFC, 0x82001004) && !insideSpecialWindows(0x62003FFC, 0x62004004));
static void updateGuestScalarSpecialHull() noexcept {
    // Split ranges for the inline fast path (WP3 contract in guest_scalar_memory.h): half-open,
    // 64-bit (hi may be 2^32, never clamped), empty = [0,0). Written only by bind/reset.
    guestScalarDataSlotLo = unboundDataSlots.empty() ? 0 : unboundDataSlots.front();
    guestScalarDataSlotHi = unboundDataSlots.empty() ? 0 : std::uint64_t{unboundDataSlots.back()} + 4;
    guestScalarUptimeLo = uptimeBundle ? uptimeBundle : 0;
    guestScalarUptimeHi = uptimeBundle ? std::uint64_t{uptimeBundle} + 24 : 0;
    std::uint64_t lo = UINT64_MAX, hi = 0;
    if (!unboundDataSlots.empty()) {
        lo = unboundDataSlots.front(); hi = std::uint64_t{unboundDataSlots.back()} + 4;
    }
    if (uptimeBundle) {
        lo = std::min<std::uint64_t>(lo, uptimeBundle); hi = std::max<std::uint64_t>(hi, std::uint64_t{uptimeBundle} + 24);
    }
    if (hi == 0) { guestScalarSpecialLo = guestScalarSpecialHi = 0; return; }
    guestScalarSpecialLo = static_cast<std::uint32_t>(lo);
    guestScalarSpecialHi = static_cast<std::uint32_t>(std::min<std::uint64_t>(hi, UINT32_MAX));
}
// Enables the fast path's split ranges even when nothing is ever bound (WP3 contract).
[[maybe_unused]] const bool guestScalarRangesInitialized = (updateGuestScalarSpecialHull(), true);

bool bindGuestScalarDataImportGuard(const std::vector<std::uint32_t>& slots) {
    if (dataSlotsBound || activeExecution || slots.size() > 256) return false;
    if (std::any_of(slots.begin(), slots.end(), [](auto slot) {
        return !slot || (slot & 3U) || classifyGuestScalarAccess(slot, 4) != GuestScalarRegion::Ordinary;
    })) return false;
    auto sorted = slots;
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) return false;
    if (!sorted.empty() && !insideSpecialWindows(sorted.front(), std::uint64_t{sorted.back()} + 4)) { // v220
        __android_log_print(ANDROID_LOG_ERROR, "RecompRenderer", "RENDER_DIAG_V220_REFUSED data-import slots %08x..%08x outside the constant windows",
                     sorted.front(), sorted.back());
        return false;
    }
    unboundDataSlots = std::move(sorted); dataSlotsBound = true;
    updateGuestScalarSpecialHull();
    return true;
}

bool bindGuestXmaRegisters(std::function<bool(std::uint32_t,bool,std::uint32_t&)> handler) {
    if (!handler || activeExecution || !xmaRegisterExclusion.tryEnter()) return false;
    const bool bound=!xmaRegisterHandler;
    if (bound) xmaRegisterHandler=std::move(handler);
    xmaRegisterExclusion.leave();
    return bound;
}
void resetGuestXmaRegisters() noexcept {
    if (activeExecution || xmaRegisterActive || !xmaRegisterExclusion.tryEnter()) return;
    xmaRegisterHandler={};
    xmaRegisterExclusion.leave();
}
bool tryAccessGuestXmaRegisters(std::uint32_t address,std::uint32_t width,bool write,std::uint64_t& value) {
    if (!activeExecution || xmaRegisterActive || width!=4 || (address&3) ||
        address<0x7FEA0000 || address>=0x7FEB0000 || (write && value>UINT32_MAX)) return false;
    // Cooperative: another host thread inside is rejected, as with the former flag. Multicore: the
    // native XMA pool is thread-safe and never runs guest code, so wait for the other accessor.
    if constexpr (ParallelGuestThreads) xmaRegisterExclusion.enter();
    else if (!xmaRegisterExclusion.tryEnter()) return false;
    bool accepted=false;
    if (xmaRegisterHandler) {
        std::uint32_t word=write?static_cast<std::uint32_t>(value):0;
        xmaRegisterActive=true;
        try { accepted=xmaRegisterHandler(address,write,word); } catch (...) {}
        xmaRegisterActive=false;
        if (accepted && !write) value=word;
    }
    xmaRegisterExclusion.leave();
    return accepted;
}

bool bindGuestScalarUptimeClock(std::uint32_t bundle, std::function<std::optional<std::uint64_t>()> source) {
    if (uptimeBundle || activeExecution || !bundle || (bundle & 3U) || !source ||
        classifyGuestScalarAccess(bundle, 24) != GuestScalarRegion::Ordinary) return false;
    if (!insideSpecialWindows(bundle, std::uint64_t{bundle} + 24)) { // v220
        __android_log_print(ANDROID_LOG_ERROR, "RecompRenderer", "RENDER_DIAG_V220_REFUSED uptime bundle %08x outside the constant windows", bundle);
        return false;
    }
    uptimeSource = std::move(source); uptimeBundle = bundle; uptimeClock.reset();
    updateGuestScalarSpecialHull();
    return true;
}

void resetGuestScalarUptimeClock() noexcept {
    uptimeBundle = 0; uptimeSource = {}; uptimeClock.reset();
    updateGuestScalarSpecialHull();
}

bool tryReadGuestScalarUptime(std::uint32_t address, std::uint32_t width, std::uint64_t& value) {
    if (!touchesUptime(address, width)) return false;
    if (address == uptimeBundle + 16 && width == 4) {
        const auto elapsed = uptimeSource();
        std::uint64_t exposed = 0;
        if (elapsed && uptimeClock.publish(*elapsed, exposed, uptimeHistory)) {
            value = static_cast<std::uint32_t>(exposed); // Intentional 49.7-day wrap.
            return true;
        }
    }
    recomp_guest_execution_scalar_access({address, width, 0, false, GuestScalarRegion::UnsupportedTimestampAccess});
}

void checkGuestScalarUptimeWrite(std::uint32_t address, std::uint32_t width, std::uint64_t value) {
    if (touchesUptime(address, width))
        recomp_guest_execution_scalar_access({address, width, value, true, GuestScalarRegion::UnsupportedTimestampAccess});
}

void resetGuestScalarDataImportGuard() noexcept {
    unboundDataSlots.clear(); dataSlotsBound = false;
    updateGuestScalarSpecialHull();
}

bool isUnboundGuestDataImportAccess(std::uint32_t address, std::uint32_t width) noexcept {
    const auto end = std::uint64_t{address} + width;
    if (!width || unboundDataSlots.empty() || end <= unboundDataSlots.front() ||
        address >= std::uint64_t{unboundDataSlots.back()} + 4) return false;
    const auto first = std::lower_bound(unboundDataSlots.begin(), unboundDataSlots.end(),
        address > 3 ? address - 3 : 0);
    return first != unboundDataSlots.end() && *first < end;
}

const char* guestExecutionStopReasonName(GuestExecutionStopReason reason) noexcept {
    switch (reason) {
    case GuestExecutionStopReason::Completed:
        return "completed";
    case GuestExecutionStopReason::InvalidEntry:
        return "invalid-entry";
    case GuestExecutionStopReason::InvalidBudget:
        return "invalid-budget";
    case GuestExecutionStopReason::NestedRun:
        return "nested-run";
    case GuestExecutionStopReason::FunctionEntryBudgetExceeded:
        return "function-entry-budget-exceeded";
    case GuestExecutionStopReason::UnimplementedImport:
        return "unimplemented-import";
    case GuestExecutionStopReason::RejectedImport:
        return "rejected-import";
    case GuestExecutionStopReason::InvalidIndirectTarget:
        return "invalid-indirect-target";
    case GuestExecutionStopReason::HostException:
        return "host-exception";
    case GuestExecutionStopReason::NoRunnableTask:
        return "no-runnable-task";
    case GuestExecutionStopReason::GuestTrap:
        return "guest-trap";
    case GuestExecutionStopReason::UnsupportedScalarAccess:
        return "unsupported-scalar-access";
    }
    return "unknown";
}

GuestExecutionResult GuestExecutionGuard::run(
        std::uint64_t maximumFunctionEntries,
        const Entry& entry,
        const FunctionTraceSink& traceSink) noexcept {
    if (!entry) {
        return {GuestExecutionStopReason::InvalidEntry, 0, "Guest entry callback is empty"};
    }
    if (maximumFunctionEntries == 0) {
        return {GuestExecutionStopReason::InvalidBudget, 0, "Function-entry budget must be positive"};
    }
    if (activeExecution != nullptr) {
        return {GuestExecutionStopReason::NestedRun, 0, "Nested guest execution is not allowed"};
    }

    ActiveExecution execution{
            maximumFunctionEntries,
            0,
            GuestExecutionStopReason::Completed,
            {},
            traceSink ? &traceSink : nullptr,
    };
    ActiveExecutionScope scope(execution);
    // Reserve before running so a host-exception detail can be built
    // without allocating while that exception is being handled.
    execution.detail.reserve(MaximumHostExceptionBytes);
    try {
        entry();
    } catch (const StopSignal&) {
        // The checkpoint/import stop populated the structured result.
    } catch (const std::exception& error) {
        execution.reason = GuestExecutionStopReason::HostException;
        // "host-exception" alone names no boundary; the type and message say
        // which host component failed. Neither is a guest pointer dereference.
        execution.detail.clear();
        appendBoundedDetail(execution.detail, "Guest entry raised a host C++ exception: ");
#if defined(__cpp_rtti) || defined(__GXX_RTTI)
        appendBoundedDetail(execution.detail, typeid(error).name());
        appendBoundedDetail(execution.detail, ": ");
#endif
        appendBoundedDetail(execution.detail, error.what());
    } catch (...) {
        execution.reason = GuestExecutionStopReason::HostException;
        execution.detail = "Guest entry raised an unknown host exception";
    }

    return {
            execution.reason,
            execution.completedFunctionEntries,
            std::move(execution.detail),
            (execution.reason == GuestExecutionStopReason::UnimplementedImport ||
             execution.reason == GuestExecutionStopReason::RejectedImport)
                    ? execution.importArguments : std::nullopt,
            execution.reason == GuestExecutionStopReason::UnsupportedScalarAccess
                    ? execution.scalarAccess : std::nullopt,
    };
}

void GuestExecutionGuard::checkpoint(std::string_view functionLabel) {
    if (activeExecution == nullptr) {
        return;
    }
    if (activeExecution->completedFunctionEntries
            >= activeExecution->maximumFunctionEntries) {
        activeExecution->reason = GuestExecutionStopReason::FunctionEntryBudgetExceeded;
        activeExecution->detail = "Generated guest function-entry budget exhausted";
        throw StopSignal{};
    }
    ++activeExecution->completedFunctionEntries;
    if (activeExecution->traceSink != nullptr) {
        // The sink consumes the view synchronously; avoid a temporary owned
        // string for every translated function entry. Keep the same byte bound.
        (*activeExecution->traceSink)(functionLabel.substr(0, MaximumImportNameBytes));
    }
}

void GuestExecutionGuard::captureImportArguments(const GuestImportArguments& arguments) noexcept {
    if (activeExecution) { activeExecution->importArguments = arguments; }
}

extern "C" [[noreturn]] void recomp_guest_execution_scalar_access(const GuestScalarAccess& access) {
    if (!activeExecution) throw std::runtime_error("Unsupported scalar access outside guarded execution");
    activeExecution->reason = GuestExecutionStopReason::UnsupportedScalarAccess;
    activeExecution->detail = "Scalar access requires hardware, address-wrap or data-import binding semantics";
    activeExecution->scalarAccess = access;
    throw StopSignal{};
}

void GuestExecutionGuard::stopForUnimplementedImport(std::string_view importName) {
    if (activeExecution == nullptr) {
        throw std::runtime_error("Unimplemented guest import called outside guarded execution");
    }
    activeExecution->reason = GuestExecutionStopReason::UnimplementedImport;
    activeExecution->detail = importName.empty()
            ? "unknown-import"
            : boundedDetail(importName);
    throw StopSignal{};
}

void GuestExecutionGuard::stopForInvalidIndirectTarget() {
    if (activeExecution == nullptr) {
        throw std::runtime_error("Invalid guest indirect target outside guarded execution");
    }
    activeExecution->reason = GuestExecutionStopReason::InvalidIndirectTarget;
    activeExecution->detail = "Generated guest indirect target is not mapped";
    throw StopSignal{};
}

void GuestExecutionGuard::stopForRejectedImport(std::string_view importName) {
    if (activeExecution == nullptr) {
        throw std::runtime_error("Rejected guest import called outside guarded execution");
    }
    activeExecution->reason = GuestExecutionStopReason::RejectedImport;
    activeExecution->detail = importName.empty()
            ? "unknown-import" : boundedDetail(importName);
    throw StopSignal{};
}

bool GuestExecutionGuard::isActive() noexcept {
    return activeExecution != nullptr;
}

void GuestExecutionGuard::stopForGuestTrap() {
    if (activeExecution == nullptr) { throw std::runtime_error("Guest trap outside guarded execution"); }
    activeExecution->reason = GuestExecutionStopReason::GuestTrap;
    activeExecution->detail = "Architected guest trap condition was true";
    throw StopSignal{};
}

// v362 (fast_tls_v362.h): the address of the calling thread's activeExecution, which isActive() tests for nullptr. The
// executor's claimed thread caches it once and reads the variable in place (no TLS resolver call per liveTask).
extern "C" const void* recomp_v362_guard_active_slot() noexcept {
    return static_cast<const void*>(&activeExecution);
}

extern "C" [[noreturn]] void recomp_guest_execution_trap() {
    GuestExecutionGuard::stopForGuestTrap();
}

extern "C" void recomp_guest_execution_checkpoint() {
    GuestExecutionGuard::checkpoint();
}

extern "C" void recomp_guest_execution_checkpoint_named(const char* functionLabel) {
    if (functionLabel == nullptr) {
        GuestExecutionGuard::checkpoint();
        return;
    }
    std::size_t length = 0;
    while (length < MaximumImportNameBytes && functionLabel[length] != '\0') {
        ++length;
    }
    GuestExecutionGuard::checkpoint(std::string_view(functionLabel, length));
}

extern "C" void recomp_guest_execution_unimplemented_import(const char* importName) {
    if (importName == nullptr) {
        GuestExecutionGuard::stopForUnimplementedImport({});
    }
    std::size_t length = 0;
    while (length < MaximumImportNameBytes && importName[length] != '\0') {
        ++length;
    }
    GuestExecutionGuard::stopForUnimplementedImport(
            std::string_view(importName, length));
}

} // namespace recomp::bootstrap
