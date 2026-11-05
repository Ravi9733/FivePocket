#include "guest_import_registry.h"
#include "guest_cooperative_executor.h"
#include "guest_thread_entry.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <new>

using namespace recomp::bootstrap;

static int allocationsBeforeFailure = -1;
void* operator new(std::size_t size) {
    if (allocationsBeforeFailure == 0) {
        allocationsBeforeFailure = -1;
        throw std::bad_alloc();
    }
    if (allocationsBeforeFailure > 0) { --allocationsBeforeFailure; }
    if (void* result = std::malloc(size ? size : 1)) { return result; }
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete[](void* p) noexcept { std::free(p); }
#if defined(__cpp_sized_deallocation)
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif

namespace {
constexpr std::uint32_t Heap = 0x10000000U;
constexpr std::uint32_t Arena = 0x20000000U;
constexpr std::uint32_t Output = 0x30000000U;
constexpr std::uint32_t Parent = 0x31000000U;
constexpr std::uint32_t Code = 0x00101000U;

GuestThreadStorageConfig config() {
    return {{Arena, 0x100000U}, {0x00100000U, 0x10000U}, {Code, 0x1000U},
            0x00100010U, 4, 0x20U, 4, 0x4000U, 2};
}
bool exact(std::uint32_t address) { return address == Code || address == Code + 4; }
std::uint32_t read32(GuestAddressSpace& space, std::uint32_t address) {
    const auto* p = space.translate(address);
    return (std::uint32_t{p[0]} << 24U) | (std::uint32_t{p[1]} << 16U)
            | (std::uint32_t{p[2]} << 8U) | p[3];
}
void outputs(GuestAddressSpace& space) { std::memset(space.translate(Output), 0xA5, 16); }
GuestImportCall creation() {
    GuestImportCall call;
    call.arguments = {Output, 0, Output + 4, Code, Code + 4, 0xDEADBEEFU, 1, 0};
    return call;
}
std::uint32_t invoke(GuestImportRegistry& registry, std::string_view name,
        std::initializer_list<std::uint32_t> args) {
    GuestImportCall call;
    std::copy(args.begin(), args.end(), call.arguments.begin());
    assert(registry.dispatch(name, call).handled());
    return static_cast<std::uint32_t>(call.returnValue);
}
void initialize(GuestImportRegistry& registry, GuestAddressSpace& space) {
    assert(registry.initialize(space, Heap, 0x10000U));
    assert(registry.bindGuestThreadToken(Parent));
    assert(registry.bindGuestThreadTlsStorage(Parent, Parent + 0x1000U, 4));
}

void testLifetime(GuestAddressSpace& space) {
    GuestImportRegistry registry;
    initialize(registry, space);
    auto call = creation();
    assert(registry.dispatch("__imp__ExCreateThread", call).state == GuestImportDispatchState::Rejected);
    assert(registry.bindThreadStorageArena(config(), {{Output, 16}}, exact));
    assert(!registry.bindThreadStorageArena(config(), {}, exact));
    assert(!registry.bindPhysicalMemoryArena(Arena, 0x10000U, 0));
    assert(!registry.bindGuestThreadTlsStorage(Parent + 4, Arena, 4));
    const auto slot = invoke(registry, "__imp__KeTlsAlloc", {});
    assert(invoke(registry, "__imp__KeTlsSetValue", {slot, 0x11}) == 1);
    outputs(space);
    call = creation();
    assert(registry.dispatch("__imp__ExCreateThread", call).handled() && call.returnValue == 0);
    const auto handle = read32(space, Output);
    const auto id = read32(space, Output + 4);
    const auto thread = registry.lookupSuspendedThread(handle);
    assert(thread && thread->layout.threadId == id && id == 2);
    assert(thread->creationFlags == 1 && thread->startContext == 0xDEADBEEFU);
    assert(registry.createdThreadCount() == 1);
    assert(invoke(registry, "__imp__KeTlsGetValue", {slot}) == 0x11); // Current thread unchanged.
    assert(registry.bindGuestThreadToken(thread->layout.threadAddress));
    assert(invoke(registry, "__imp__KeTlsGetValue", {slot}) == 0);
    assert(invoke(registry, "__imp__KeTlsSetValue", {slot, 0x22}) == 1);
    assert(read32(space, thread->dynamicTlsAddress) == 0x22);
    assert(!registry.teardownGuestThread(thread->layout.threadAddress));
    assert(invoke(registry, "__imp__NtSetEvent", {handle, 0}) == 0xC0000024U);
    assert(invoke(registry, "__imp__NtClearEvent", {handle}) == 0xC0000024U);
    assert(invoke(registry, "__imp__NtReleaseSemaphore", {handle, 1, 0}) == 0xC0000024U);
    std::memset(space.translate(Output + 8), 0, 8);
    assert(invoke(registry, "__imp__NtWaitForSingleObjectEx", {handle, 1, 0, Output + 8}) == 0x102U);
    GuestImportCall wait;
    wait.arguments = {handle, 1, 0, 0};
    assert(registry.dispatch("__imp__NtWaitForSingleObjectEx", wait).state == GuestImportDispatchState::Rejected);
    wait.arguments = {handle, 0};
    assert(registry.dispatch("__imp__NtResumeThread", wait).state == GuestImportDispatchState::Rejected);
    assert(invoke(registry, "__imp__NtClose", {handle}) == 0);
    assert(!registry.lookupSuspendedThread(handle) && registry.createdThreadCount() == 1);
    assert(read32(space, thread->dynamicTlsAddress) == 0x22); // Close is not termination.
    assert(invoke(registry, "__imp__NtClose", {handle}) == 0xC0000008U);
    registry.reset();
    assert(registry.createdThreadCount() == 0);
    const auto* first = space.translate(thread->allocation.base);
    assert(std::all_of(first, first + thread->allocation.size, [](auto b) { return b == 0; }));
    initialize(registry, space);
    assert(registry.bindThreadStorageArena(config(), {}, exact));
    call = creation();
    call.arguments[2] = 0; // Optional ID output.
    call.arguments[3] = 0; // Direct startup without XAPI trampoline.
    assert(registry.dispatch("__imp__ExCreateThread", call).handled() && call.returnValue == 0);
    assert(read32(space, Output) != handle);
    assert(registry.lookupSuspendedThread(read32(space, Output))->layout.threadId != id);
    registry.reset();
}

void testValidation(GuestAddressSpace& space) {
    GuestImportRegistry registry;
    initialize(registry, space);
    auto invalid = config(); invalid.arena.base = Heap;
    assert(!registry.bindThreadStorageArena(invalid, {}, exact));
    invalid = config(); invalid.arena.base = Parent;
    assert(!registry.bindThreadStorageArena(invalid, {}, exact));
    assert(!registry.bindThreadStorageArena(config(), {{Arena - 4, 8}}, exact));
    assert(!registry.bindThreadStorageArena(config(), {}, {}));
    assert(registry.bindThreadStorageArena(config(), {}, exact));
    const std::pair<std::size_t, std::uint64_t> bad[] = {
        {0, 0}, {0, Output + 1}, {0, Arena}, {2, Output}, {2, Output + 1},
        {2, Arena}, {4, Code + 8}, {3, Code + 8}, {6, 0}, {6, 0x81},
        {1, 0xFFFFFFFFU}, {6, 0x02000001U}, {6, 0x03000001U},
        {6, 0x40000000U}, {6, 0x80000001U}, {6, 0x02000080U},
    };
    for (const auto& [index, value] : bad) {
        outputs(space);
        auto call = creation(); call.arguments[index] = value;
        const auto result = registry.dispatch("__imp__ExCreateThread", call);
        assert(!result.handled() || call.returnValue != 0);
        assert(registry.createdThreadCount() == 0);
        assert(read32(space, Output) == 0xA5A5A5A5U && read32(space, Output + 4) == 0xA5A5A5A5U);
    }
    for (int i = 0; i < 4; ++i) { assert(invoke(registry, "__imp__KeTlsAlloc", {}) == unsigned(i)); }
    assert(invoke(registry, "__imp__KeTlsAlloc", {}) == 0xFFFFFFFFU);
    registry.unbindGuestThreadToken();
    auto call = creation();
    assert(registry.dispatch("__imp__ExCreateThread", call).state == GuestImportDispatchState::Rejected);
    registry.reset();
}

void testCreationAffinity(GuestAddressSpace& space) {
    for (unsigned cpu = 0; cpu < 6; ++cpu) {
        for (unsigned suspended = 0; suspended < 2; ++suspended) {
            GuestImportRegistry registry;
            initialize(registry, space);
            assert(registry.bindThreadStorageArena(config(), {}, exact));
            GuestCooperativeExecutor executor;
            std::optional<GuestPreparedThread> queued;
            bool ran = false;
            const auto mask = 1U << cpu;
            const auto flags = (mask << 24U) | suspended;
            const auto check = [&](const GuestPreparedThread& t) {
                assert(t.creationFlags == flags);
                assert(read32(space, t.layout.threadAddress + 0x16CU) == flags);
                assert(*space.translate(t.layout.pcrAddress + 0x10CU) == cpu);
                assert(*space.translate(t.layout.threadAddress + 0xBFU) == cpu);
                const auto entry = makeGuestThreadEntry(t);
                assert(entry && entry->address == Code && entry->argument0 == Code + 4);
                assert(entry->argument1 == 0xDEADBEEFU && entry->pcrAddress == t.layout.pcrAddress);
            };
            assert(registry.bindThreadResumeHandler([&](const auto& t, std::int32_t priority) {
                check(t);
                assert(priority == 0);
                queued = t;
                return executor.enqueue(t.layout.threadAddress, [&, t] {
                    check(t);
                    ran = true;
                    assert(registry.markGuestThreadReturned(t.layout.threadAddress, 0));
                }, priority);
            }));
            // Even with a scheduler, affinity must be explicitly enabled.
            auto call = creation(); call.arguments[6] = flags;
            outputs(space);
            assert(!registry.dispatch("__imp__ExCreateThread", call).handled());
            assert(registry.createdThreadCount() == 0 && read32(space, Output) == 0xA5A5A5A5U);
            assert(registry.bindVirtualCpuAffinity());
            for (auto badFlags : {0x03000001U, 0x40000001U, 0x02000081U, 0x02000020U}) {
                call = creation(); call.arguments[6] = badFlags;
                assert(!registry.dispatch("__imp__ExCreateThread", call).handled());
                assert(registry.createdThreadCount() == 0 && !queued);
            }
            const auto result = executor.run(Parent, [&] {
                call = creation(); call.arguments[6] = flags;
                assert(registry.dispatch("__imp__ExCreateThread", call).handled() && call.returnValue == 0);
                const auto handle = read32(space, Output);
                const auto t = suspended ? registry.lookupSuspendedThread(handle) : queued;
                assert(t && !ran);
                check(*t);
                const auto token = t->layout.threadAddress;
                const auto otherMask = 1U << ((cpu + 1U) % 6U);
                assert(invoke(registry, "__imp__KeSetAffinityThread", {token, otherMask, Output + 8}) == 0);
                assert(read32(space, Output + 8) == mask);
                assert(invoke(registry, "__imp__KeSetAffinityThread", {token, mask, Output + 8}) == 0);
                assert(read32(space, Output + 8) == otherMask);
                assert(invoke(registry, "__imp__NtResumeThread", {handle, Output + 8}) == 0);
                assert(read32(space, Output + 8) == suspended && queued && !ran);
                GuestExecutionGuard::checkpoint("creation_affinity");
                assert(ran);
                assert(read32(space, token + 0x16CU) == flags);
            }, {10, 32, 2}, [&](auto token) { assert(registry.bindGuestThreadToken(token)); });
            assert(result.execution.completed() && result.workersCreated == 1 && result.workersEntered == 1);
            registry.reset();
        }
    }
}

void testDuplicateThreadOwnership(GuestAddressSpace& space) {
    GuestImportRegistry registry;
    initialize(registry, space);
    std::memset(space.translate(Parent), 0, 0x170);
    space.translate(Parent + 0x14CU)[3] = 1;
    assert(registry.bindInitialThreadObject(Parent, 1));
    assert(registry.bindThreadObjectType(Output + 0x100));
    assert(registry.bindThreadStorageArena(config(), {}, exact));
    auto call = creation();
    assert(registry.dispatch("__imp__ExCreateThread", call).handled() && call.returnValue == 0);
    const auto original = read32(space, Output);
    const auto prepared = registry.lookupSuspendedThread(original);
    assert(prepared);
    for (const auto args : {std::array<std::uint64_t, 8>{original, Output + 1, 0},
            std::array<std::uint64_t, 8>{original, Output + 8, 2},
            std::array<std::uint64_t, 8>{original, 0, 0}}) {
        call = {}; call.arguments = args;
        assert(!registry.dispatch("__imp__NtDuplicateObject", call).handled());
        assert(registry.lookupSuspendedThread(original));
    }
    assert(invoke(registry, "__imp__NtDuplicateObject", {original, Output + 8, 0}) == 0);
    const auto duplicate = read32(space, Output + 8);
    assert(original != duplicate && registry.createdThreadCount() == 1);
    assert(registry.lookupSuspendedThread(duplicate)->layout.threadId == prepared->layout.threadId);
    assert(invoke(registry, "__imp__NtClose", {original}) == 0);
    assert(!registry.lookupSuspendedThread(original) && registry.lookupSuspendedThread(duplicate));
    assert(invoke(registry, "__imp__ObReferenceObjectByHandle", {duplicate, Output + 0x100, Output + 12}) == 0);
    assert(read32(space, Output + 12) == prepared->layout.threadAddress);
    GuestImportCall deref;
    deref.arguments[0] = prepared->layout.threadAddress;
    assert(registry.dispatch("__imp__ObDereferenceObject", deref).handled());
    // A real handle made from the current-thread pseudohandle also resolves
    // the initial thread identity (which is not allocated in the child pool).
    assert(invoke(registry, "__imp__NtDuplicateObject", {0xFFFFFFFEU, Output + 8, 0}) == 0);
    const auto mainHandle = read32(space, Output + 8);
    assert(invoke(registry, "__imp__ObReferenceObjectByHandle", {mainHandle, Output + 0x100, Output + 12}) == 0);
    assert(read32(space, Output + 12) == Parent);
    deref.arguments[0] = Parent;
    assert(registry.dispatch("__imp__ObDereferenceObject", deref).handled());
    assert(invoke(registry, "__imp__NtClose", {mainHandle}) == 0);
    GuestCooperativeExecutor executor;
    bool ran = false;
    assert(registry.bindThreadResumeHandler([&](const auto& t, auto priority) {
        assert(t.layout.threadId == prepared->layout.threadId);
        return executor.enqueue(t.layout.threadAddress, [&] {
            ran = true;
            assert(registry.markGuestThreadReturned(prepared->layout.threadAddress, 7));
        }, priority);
    }));
    const auto result = executor.run(Parent, [&] {
        assert(invoke(registry, "__imp__NtResumeThread", {duplicate, Output + 8}) == 0);
        GuestExecutionGuard::checkpoint("duplicate_thread");
        assert(ran);
        std::memset(space.translate(Output + 32), 0, 8);
        assert(invoke(registry, "__imp__NtWaitForSingleObjectEx", {duplicate, 0, 0, Output + 32}) == 0);
        assert(invoke(registry, "__imp__NtDuplicateObject", {duplicate, Output + 8, 1}) == 0);
        const auto transferred = read32(space, Output + 8);
        assert(transferred != duplicate);
        assert(invoke(registry, "__imp__NtClose", {duplicate}) == 0xC0000008U);
        assert(invoke(registry, "__imp__NtWaitForSingleObjectEx", {transferred, 0, 0, Output + 32}) == 0);
        assert(invoke(registry, "__imp__NtDuplicateObject", {transferred, 0, 1}) == 0);
        assert(invoke(registry, "__imp__NtClose", {transferred}) == 0xC0000008U);
    }, {10, 32, 2}, [&](auto token) { assert(registry.bindGuestThreadToken(token)); });
    assert(result.execution.completed() && result.workersEntered == 1 && registry.createdThreadCount() == 1);
    registry.reset();
}

void testResourceFailure(GuestAddressSpace& space) {
    for (int failAt = 0; failAt < 24; ++failAt) {
        GuestImportRegistry registry;
        initialize(registry, space);
        assert(registry.bindThreadStorageArena(config(), {}, exact));
        outputs(space);
        auto call = creation();
        bool succeeded = false;
        allocationsBeforeFailure = failAt;
        try {
            const auto result = registry.dispatch("__imp__ExCreateThread", call);
            succeeded = result.handled() && call.returnValue == 0;
        } catch (const std::bad_alloc&) {
            // Diagnostic construction before mutation may itself exhaust memory.
        }
        allocationsBeforeFailure = -1;
        assert(registry.createdThreadCount() == (succeeded ? 1U : 0U));
        if (!succeeded) {
            assert(read32(space, Output) == 0xA5A5A5A5U && read32(space, Output + 4) == 0xA5A5A5A5U);
            call = creation();
            assert(registry.dispatch("__imp__ExCreateThread", call).handled() && call.returnValue == 0);
            assert(registry.createdThreadCount() == 1);
        }
        registry.reset();
    }
    GuestImportRegistry registry;
    initialize(registry, space);
    auto small = config(); small.arena.size = 0x7000U;
    assert(registry.bindThreadStorageArena(small, {}, exact));
    auto call = creation();
    assert(registry.dispatch("__imp__ExCreateThread", call).handled() && call.returnValue == 0);
    assert(invoke(registry, "__imp__NtClose", {read32(space, Output)}) == 0);
    outputs(space);
    call = creation();
    assert(registry.dispatch("__imp__ExCreateThread", call).handled() && call.returnValue == 0xC0000017U);
    assert(read32(space, Output) == 0xA5A5A5A5U && registry.createdThreadCount() == 1);
    registry.reset();
}

void testNativeWorkerAllocationFailure(GuestAddressSpace& space) {
    unsigned failures=0,successes=0;
    for(int fail=0;fail<24;++fail) {
        GuestImportRegistry registry;initialize(registry,space);
        assert(registry.bindThreadStorageArena(config(),{},exact));
        std::memset(space.translate(Arena),0,config().arena.size);
        bool queued=false;
        const GuestImportRegistry::NativeCallbackEnqueue enqueue=[&](const auto& thread) {
            queued=true;assert(thread.wrappedContextAddress==thread.startContext);return true;
        };
        allocationsBeforeFailure=fail;
        const auto worker=registry.createNativeCallbackWorker(Code,0xDEADBEEF,enqueue);
        allocationsBeforeFailure=-1;
        assert(queued==worker.has_value() && registry.createdThreadCount()==(worker?1U:0U));
        if(worker) {++successes;assert(read32(space,worker->startContext)==0xDEADBEEF);}
        else ++failures;
        registry.reset();
        assert(std::all_of(space.translate(Arena),space.translate(Arena)+config().arena.size,[](auto b){return b==0;}));
    }
    assert(failures>0 && successes>0);
}

void testHandleExhaustionAndTlsCapacity(GuestAddressSpace& space) {
    GuestImportRegistry registry;
    initialize(registry, space);
    assert(registry.bindThreadStorageArena(config(), {}, exact));
    for (std::size_t i = 0; i < GuestImportRegistry::MaximumKernelHandles; ++i) {
        assert(invoke(registry, "__imp__NtCreateSemaphore", {Output + 8, 0, 0, 1}) == 0);
    }
    const auto lastHandle = read32(space, Output + 8);
    outputs(space);
    auto call = creation();
    assert(registry.dispatch("__imp__ExCreateThread", call).handled()
            && call.returnValue == 0xC0000017U);
    assert(registry.createdThreadCount() == 0 && read32(space, Output) == 0xA5A5A5A5U);
    assert(invoke(registry, "__imp__NtClose", {lastHandle}) == 0);
    call = creation();
    assert(registry.dispatch("__imp__ExCreateThread", call).handled() && call.returnValue == 0);
    const auto fullTableThread = read32(space, Output);
    assert(invoke(registry, "__imp__NtDuplicateObject", {fullTableThread, Output + 8, 1}) == 0xC0000017U);
    assert(read32(space, Output + 8) == 0 && !registry.lookupSuspendedThread(fullTableThread));
    assert(registry.createdThreadCount() == 1); // Close-source also applies on duplication failure.
    registry.reset();

    assert(registry.initialize(space, Heap, 0x10000U));
    assert(registry.bindGuestThreadToken(Parent));
    for (int i = 0; i < 5; ++i) { assert(invoke(registry, "__imp__KeTlsAlloc", {}) == unsigned(i)); }
    assert(!registry.bindThreadStorageArena(config(), {}, exact)); // Existing slot 4 would not fit.
    assert(invoke(registry, "__imp__KeTlsFree", {4}) == 1);
    assert(registry.bindThreadStorageArena(config(), {}, exact));
    registry.reset();
}
} // namespace

int main() {
    GuestAddressSpace space;
    assert(space.reserve());
    std::memset(space.translate(config().tlsRawDataAddress), 0x5A, 4);
    testLifetime(space);
    testValidation(space);
    testCreationAffinity(space);
    testDuplicateThreadOwnership(space);
    testResourceFailure(space);
    testNativeWorkerAllocationFailure(space);
    testHandleExhaustionAndTlsCapacity(space);
}
