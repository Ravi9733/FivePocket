#include "guest_cooperative_executor.h"
#include "guest_import_registry.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <initializer_list>
#include <system_error>

using namespace recomp::bootstrap;
namespace {
constexpr std::uint32_t Parent = 0x30000000U, Out = 0x30010000U, Code = 0x00101000U;
std::uint32_t read32(GuestAddressSpace& space, std::uint32_t p) {
    const auto* b = space.translate(p);
    return (std::uint32_t{b[0]} << 24) | (std::uint32_t{b[1]} << 16) | (std::uint32_t{b[2]} << 8) | b[3];
}
GuestImportDispatchResult call(GuestImportRegistry& registry, std::string_view name,
        GuestImportCall& c, std::initializer_list<std::uint32_t> args) {
    c = {};
    c.returnValue = 0x123456789ABCDEF0ULL;
    std::copy(args.begin(), args.end(), c.arguments.begin());
    return registry.dispatch(name, c);
}
std::uint32_t invoke(GuestImportRegistry& registry, std::string_view name,
        std::initializer_list<std::uint32_t> args) {
    GuestImportCall c;
    assert(call(registry, name, c, args).handled());
    return static_cast<std::uint32_t>(c.returnValue);
}
std::uint32_t initialize(GuestImportRegistry& registry, GuestAddressSpace& space) {
    assert(registry.initialize(space, 0x10000000, 0x10000));
    assert(registry.bindGuestThreadToken(Parent));
    assert(registry.bindGuestThreadTlsStorage(Parent, Parent + 0x1000, 4));
    GuestThreadStorageConfig config{{0x20000000, 0x100000}, {0x100000, 0x10000}, {Code, 0x1000},
            0x100010, 4, 0x20, 4, 0x4000, 2};
    assert(registry.bindThreadStorageArena(config, {{Parent, 0x2000}, {Out, 16}},
            [](auto address) { return address == Code; }));
    assert(invoke(registry, "__imp__ExCreateThread", {Out, 0, Out + 4, 0, Code, 0x1234, 1}) == 0);
    return read32(space, Out);
}

void testRealCallbackExecution(GuestAddressSpace& space) {
    GuestImportRegistry registry;
    const auto handle = initialize(registry, space);
    const auto thread = registry.lookupSuspendedThread(handle);
    assert(thread);
    assert(registry.bindThreadObjectType(Out + 0x100));
    GuestImportCall c;
    assert(call(registry, "__imp__NtResumeThread", c, {handle, Out}).state == GuestImportDispatchState::Rejected);
    assert(invoke(registry, "__imp__NtResumeThread", {0x123, Out}) == 0xC0000008U);
    assert(!call(registry, "__imp__NtResumeThread", c, {handle, Out + 1}).handled());
    assert(!call(registry, "__imp__NtResumeThread", c, {handle, thread->layout.threadAddress}).handled());
    const auto slot = invoke(registry, "__imp__KeTlsAlloc", {});
    assert(invoke(registry, "__imp__KeTlsSetValue", {slot, 0x11}) == 1);
    GuestCooperativeExecutor executor;
    bool workerRan = false;
    assert(invoke(registry, "__imp__KeSetBasePriorityThread", {thread->layout.threadAddress, 2}) == 0);
    assert(registry.bindThreadResumeHandler([&](const auto& prepared, std::int32_t priority) {
        assert(priority == 2);
        return executor.enqueue(prepared.layout.threadAddress, [&, prepared] {
            assert(prepared.startContext == 0x1234 && prepared.startRoutine == Code);
            assert(invoke(registry, "__imp__KeTlsGetValue", {slot}) == 0);
            assert(invoke(registry, "__imp__KeTlsSetValue", {slot, 0x22}) == 1);
            const auto stackOutput = prepared.layout.stackBase + 0x100;
            assert(invoke(registry, "__imp__ObReferenceObjectByHandle", {handle, Out + 0x100, stackOutput}) == 0);
            assert(read32(space, stackOutput) == prepared.layout.threadAddress);
            GuestImportCall outputCall;
            assert(call(registry, "__imp__ObDereferenceObject", outputCall, {prepared.layout.threadAddress}).handled());
            assert(invoke(registry, "__imp__NtResumeThread", {handle, stackOutput}) == 0);
            assert(read32(space, stackOutput) == 0);
            assert(invoke(registry, "__imp__ExCreateThread", {stackOutput, 0, stackOutput + 4, 0, Code, 0, 1}) == 0);
            const auto secondHandle = read32(space, stackOutput);
            const auto second = registry.lookupSuspendedThread(secondHandle);
            assert(second);
            for (const auto badOutput : {prepared.layout.pcrAddress, prepared.layout.threadAddress,
                    prepared.layout.tlsAddress, prepared.layout.stackBase + prepared.layout.stackSize,
                    second->layout.stackBase, second->layout.stackBase + 0x100}) {
                assert(!call(registry, "__imp__NtResumeThread", outputCall, {handle, badOutput}).handled());
                assert(!call(registry, "__imp__ObReferenceObjectByHandle", outputCall,
                        {handle, Out + 0x100, badOutput}).handled());
                assert(!call(registry, "__imp__ExCreateThread", outputCall,
                        {badOutput, 0, 0, 0, Code, 0, 1}).handled());
            }
            GuestExecutionGuard::checkpoint("synthetic_worker");
            workerRan = true;
            assert(registry.markGuestThreadReturned(prepared.layout.threadAddress, 7));
        }, priority);
    }));
    assert(!registry.bindThreadResumeHandler([](const auto&, std::int32_t) { return true; }));
    const auto result = executor.run(Parent, [&] {
        std::memset(space.translate(Out + 8), 0, 8);
        assert(invoke(registry, "__imp__NtWaitForSingleObjectEx", {handle, 1, 0, Out + 8}) == 0x102);
        assert(invoke(registry, "__imp__NtResumeThread", {handle, Out}) == 0);
        assert(read32(space, Out) == 1 && !registry.lookupSuspendedThread(handle) && !workerRan);
        assert(invoke(registry, "__imp__NtResumeThread", {handle, Out}) == 0);
        assert(read32(space, Out) == 0);
        assert(!call(registry, "__imp__KeSetBasePriorityThread", c, {thread->layout.threadAddress, 1}).handled());
        GuestExecutionGuard::checkpoint("synthetic_main");
        assert(workerRan);
        assert(invoke(registry, "__imp__KeTlsGetValue", {slot}) == 0x11);
        assert(invoke(registry, "__imp__NtWaitForSingleObjectEx", {handle, 1, 0, Out + 8}) == 0);
        assert(invoke(registry, "__imp__NtWaitForSingleObjectEx", {handle, 1, 0, Out + 8}) == 0);
        assert(!call(registry, "__imp__NtResumeThread", c, {handle, Out}).handled());
        assert(!registry.markGuestThreadReturned(thread->layout.threadAddress, 8));
    }, {100, 32, 3}, [&](auto token) { assert(registry.bindGuestThreadToken(token)); });
    assert(result.execution.completed() && result.workersCreated == 1 && result.workersEntered == 1);
    assert(result.execution.completedFunctionEntries == 2);
    assert(invoke(registry, "__imp__NtClose", {handle}) == 0);
    registry.reset(); // Executor has joined every worker before guest storage is scrubbed.
}

void testEnqueueFailureCanRetry(GuestAddressSpace& space) {
    GuestImportRegistry registry;
    const auto handle = initialize(registry, space);
    bool fail = true, ran = false;
    GuestCooperativeExecutor executor([&](auto body) -> std::thread {
        if (fail) { throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again)); }
        return std::thread(std::move(body));
    });
    assert(registry.bindThreadResumeHandler([&](const auto& thread, std::int32_t) {
        return executor.enqueue(thread.layout.threadAddress, [&] { ran = true; });
    }));
    const auto result = executor.run(Parent, [&] {
        std::memset(space.translate(Out), 0xA5, 4);
        assert(invoke(registry, "__imp__NtResumeThread", {handle, Out}) == 0xC0000017U);
        assert(read32(space, Out) == 0xA5A5A5A5U && registry.lookupSuspendedThread(handle));
        fail = false;
        assert(invoke(registry, "__imp__NtResumeThread", {handle, Out}) == 0 && read32(space, Out) == 1);
        GuestExecutionGuard::checkpoint("retry");
        assert(ran);
    }, {10, 32, 2}, [&](auto token) { assert(registry.bindGuestThreadToken(token)); });
    assert(result.execution.completed() && result.workersCreated == 1 && ran);
    registry.reset();
}

void testSingletonVirtualAffinity(GuestAddressSpace& space) {
    GuestImportRegistry registry;
    assert(!registry.bindVirtualCpuAffinity());
    const auto handle = initialize(registry, space);
    const auto thread = registry.lookupSuspendedThread(handle);
    assert(thread && !registry.bindVirtualCpuAffinity());
    const auto token = thread->layout.threadAddress;
    auto* pcrCpu = space.translate(thread->layout.pcrAddress + 0x10C);
    auto* threadCpu = space.translate(token + 0xBF);
    GuestImportCall c;
    assert(!call(registry, "__imp__KeSetAffinityThread", c, {token, 1, Out}).handled());
    GuestCooperativeExecutor executor;
    assert(registry.bindThreadResumeHandler([&](const auto& prepared, std::int32_t) {
        return executor.enqueue(prepared.layout.threadAddress, [&] {
            assert(*pcrCpu == 5 && *threadCpu == 5);
            const auto output = thread->layout.stackBase + 0x100;
            assert(invoke(registry, "__imp__KeSetAffinityThread", {token, 8, output}) == 0);
            assert(read32(space, output) == 32 && *pcrCpu == 3 && *threadCpu == 3);
            assert(registry.markGuestThreadReturned(token, 0));
        });
    }));
    assert(registry.bindVirtualCpuAffinity() && registry.bindVirtualCpuAffinity());
    const auto result = executor.run(Parent, [&] {
        std::uint32_t previous = 1;
        for (std::uint32_t cpu = 0; cpu < 6; ++cpu) {
            const auto mask = 1U << cpu;
            assert(invoke(registry, "__imp__KeSetAffinityThread", {token, mask, Out}) == 0);
            assert(read32(space, Out) == previous && *pcrCpu == cpu && *threadCpu == cpu);
            assert(pcrCpu[-1] == 0 && pcrCpu[1] == 0 && threadCpu[-1] == 0 && threadCpu[1] == 0);
            previous = mask;
        }
        for (const auto invalid : {0U, 64U, 0x80000000U, 0xFFFFFFFFU}) {
            assert(invoke(registry, "__imp__KeSetAffinityThread", {token, invalid, Out}) == 0xC000000DU);
            assert(read32(space, Out) == 16 && *pcrCpu == 5);
        }
        for (const auto multiple : {3U, 0x3FU}) {
            assert(!call(registry, "__imp__KeSetAffinityThread", c, {token, multiple, Out}).handled());
        }
        for (const auto bad : {token, thread->layout.pcrAddress, thread->layout.tlsAddress,
                thread->layout.stackBase, Out + 1, 0xFFFFFFFCU + 1}) {
            assert(!call(registry, "__imp__KeSetAffinityThread", c, {token, 1, bad}).handled());
            assert(*pcrCpu == 5 && *threadCpu == 5);
        }
        assert(!call(registry, "__imp__KeSetAffinityThread", c, {token + 4, 1, Out}).handled());
        assert(!call(registry, "__imp__KeSetAffinityThread", c, {Parent, 1, Out}).handled());
        *pcrCpu = 6;
        assert(!call(registry, "__imp__KeSetAffinityThread", c, {token, 1, Out}).handled());
        *pcrCpu = 5; *threadCpu = 4;
        assert(!call(registry, "__imp__KeSetAffinityThread", c, {token, 1, Out}).handled());
        *threadCpu = 5;
        assert(invoke(registry, "__imp__KeSetAffinityThread", {token, 32, 0}) == 0);
        assert(invoke(registry, "__imp__NtResumeThread", {handle, Out}) == 0);
        GuestExecutionGuard::checkpoint("resume_affinity");
        assert(*pcrCpu == 3 && *threadCpu == 3);
        assert(!call(registry, "__imp__KeSetAffinityThread", c, {token, 1, Out}).handled());
    }, {100, 32, 2}, [&](auto t) { assert(registry.bindGuestThreadToken(t)); });
    assert(result.execution.completed() && result.workersEntered == 1);
    registry.reset();
    assert(!registry.bindVirtualCpuAffinity());
}

void testCooperativeSignalWaits(GuestAddressSpace& space) {
    for (int kind = 0; kind < 4; ++kind) { // Semaphore, manual event, auto event, thread completion.
        GuestImportRegistry registry;
        assert(!registry.bindThreadWaitHandler([](const auto&) { return true; }));
        const auto worker = initialize(registry, space);
        std::uint32_t signalHandle = worker;
        if (kind == 0) {
            assert(invoke(registry, "__imp__NtCreateSemaphore", {Out + 8, 0, 0, 1}) == 0);
            signalHandle = read32(space, Out + 8);
        } else if (kind < 3) {
            assert(invoke(registry, "__imp__NtCreateEvent", {Out + 8, 0, static_cast<std::uint32_t>(kind - 1), 0}) == 0);
            signalHandle = read32(space, Out + 8);
        }
        GuestCooperativeExecutor executor;
        bool waiting = false, resumed = false;
        assert(registry.bindThreadResumeHandler([&](const auto& thread, std::int32_t) {
            return executor.enqueue(thread.layout.threadAddress, [&, thread] {
                assert(waiting && !resumed);
                if (kind == 0) { assert(invoke(registry, "__imp__NtReleaseSemaphore", {signalHandle, 1, 0}) == 0); }
                else if (kind < 3) { assert(invoke(registry, "__imp__NtSetEvent", {signalHandle, 0}) == 0); }
                assert(registry.markGuestThreadReturned(thread.layout.threadAddress, 0));
            });
        }));
        assert(registry.bindThreadWaitHandler([&](const auto& ready) { return executor.waitUntil(ready); }));
        assert(!registry.bindThreadWaitHandler([](const auto&) { return true; }));
        const auto result = executor.run(Parent, [&] {
            assert(invoke(registry, "__imp__NtResumeThread", {worker, 0}) == 0);
            waiting = true;
            assert(invoke(registry, "__imp__NtWaitForSingleObjectEx", {signalHandle, 1, 0, 0}) == 0);
            resumed = true;
            std::memset(space.translate(Out + 16), 0, 8);
            const auto second = invoke(registry, "__imp__NtWaitForSingleObjectEx", {signalHandle, 1, 0, Out + 16});
            assert(second == (kind == 1 || kind == 3 ? 0U : 0x102U));
        }, {100, 32, 2}, [&](auto token) { assert(registry.bindGuestThreadToken(token)); });
        assert(result.execution.completed() && resumed && result.waitOperations == 1);
        assert(result.contextSwitches == 2);
        registry.reset();
    }
}

void testWaitRejectionAndClose(GuestAddressSpace& space) {
    GuestImportRegistry registry;
    const auto worker = initialize(registry, space);
    assert(invoke(registry, "__imp__NtCreateEvent", {Out + 8, 0, 0, 0}) == 0);
    const auto event = read32(space, Out + 8);
    GuestCooperativeExecutor executor;
    assert(registry.bindThreadResumeHandler([&](const auto& thread, std::int32_t) {
        return executor.enqueue(thread.layout.threadAddress, [&] { assert(invoke(registry, "__imp__NtClose", {event}) == 0); });
    }));
    assert(registry.bindThreadWaitHandler([&](const auto& ready) { return executor.waitUntil(ready); }));
    const auto result = executor.run(Parent, [&] {
        GuestImportCall c;
        // No producer is runnable yet; wait rejects immediately without spinning.
        assert(!call(registry, "__imp__NtWaitForSingleObjectEx", c, {event, 1, 0, 0}).handled());
        std::memset(space.translate(Out + 16), 0xFF, 8); // Relative timeout is not implemented.
        assert(!call(registry, "__imp__NtWaitForSingleObjectEx", c, {event, 1, 0, Out + 16}).handled());
        assert(!call(registry, "__imp__NtWaitForSingleObjectEx", c, {event, 1, 1, 0}).handled());
        assert(invoke(registry, "__imp__NtResumeThread", {worker, 0}) == 0);
        assert(!call(registry, "__imp__NtWaitForSingleObjectEx", c, {event, 1, 0, 0}).handled());
    }, {100, 32, 2}, [&](auto token) { assert(registry.bindGuestThreadToken(token)); });
    assert(result.execution.completed() && result.waitOperations == 2);
    registry.reset();
}

void testMultipleWaiters(GuestAddressSpace& space) {
    for (std::uint32_t type = 0; type < 2; ++type) {
        GuestImportRegistry registry;
        const auto first = initialize(registry, space);
        assert(invoke(registry, "__imp__ExCreateThread", {Out + 8, 0, 0, 0, Code, 0, 1}) == 0);
        const auto second = read32(space, Out + 8);
        assert(invoke(registry, "__imp__NtCreateEvent", {Out + 12, 0, type, 0}) == 0);
        const auto event = read32(space, Out + 12);
        const auto slot = invoke(registry, "__imp__KeTlsAlloc", {});
        GuestCooperativeExecutor executor;
        int waiting = 0, woken = 0;
        assert(registry.bindThreadResumeHandler([&](const auto& prepared, std::int32_t) {
            return executor.enqueue(prepared.layout.threadAddress, [&, prepared] {
                assert(invoke(registry, "__imp__KeTlsSetValue", {slot, prepared.layout.threadId}) == 1);
                ++waiting;
                assert(invoke(registry, "__imp__NtWaitForSingleObjectEx", {event, 1, 0, 0}) == 0);
                assert(invoke(registry, "__imp__KeTlsGetValue", {slot}) == prepared.layout.threadId);
                ++woken;
                assert(registry.markGuestThreadReturned(prepared.layout.threadAddress, 0));
            });
        }));
        assert(registry.bindThreadWaitHandler([&](const auto& ready) { return executor.waitUntil(ready); }));
        const auto result = executor.run(Parent, [&] {
            assert(invoke(registry, "__imp__NtResumeThread", {first, 0}) == 0);
            assert(invoke(registry, "__imp__NtResumeThread", {second, 0}) == 0);
            GuestExecutionGuard::checkpoint("two_waiters");
            assert(waiting == 2 && woken == 0);
            assert(invoke(registry, "__imp__NtSetEvent", {event, 0}) == 0);
            GuestExecutionGuard::checkpoint("signal_waiters");
            assert(woken == (type == 0 ? 2 : 1));
            if (type == 1) {
                assert(invoke(registry, "__imp__NtSetEvent", {event, 0}) == 0);
                GuestExecutionGuard::checkpoint("signal_second");
                assert(woken == 2);
            }
            assert(invoke(registry, "__imp__KeTlsGetValue", {slot}) == 0);
        }, {100, 1, 3}, [&](auto token) { assert(registry.bindGuestThreadToken(token)); });
        assert(result.execution.completed() && result.waitOperations == 2 && result.workersEntered == 2);
        registry.reset();
    }
}

void testRelativeWaitDeadlines(GuestAddressSpace& space) {
    GuestImportRegistry registry;
    const auto worker = initialize(registry, space);
    assert(invoke(registry, "__imp__NtCreateEvent", {Out + 8, 0, 1, 0}) == 0);
    const auto event = read32(space, Out + 8);
    const auto timeout = Out + 16;
    const auto setTicks = [&](std::uint64_t value) {
        auto* p = space.translate(timeout);
        for (unsigned i = 0; i < 8; ++i) { p[i] = static_cast<std::uint8_t>(value >> (56 - i * 8)); }
    };
    GuestCooperativeExecutor executor;
    assert(registry.bindThreadResumeHandler([&](const auto& thread, std::int32_t) {
        return executor.enqueue(thread.layout.threadAddress, [&] {
            assert(invoke(registry, "__imp__NtSetEvent", {event, 0}) == 0);
        });
    }));
    assert(!registry.bindThreadTimedWaitHandler([](const auto&, auto) { return true; }));
    assert(registry.bindThreadWaitHandler([&](const auto& ready) { return executor.waitUntil(ready); }));
    assert(registry.bindThreadTimedWaitHandler([&](const auto& ready, auto deadline) {
        return executor.waitUntil(ready, deadline);
    }));
    const auto result = executor.run(Parent, [&] {
        GuestImportCall c;
        setTicks(1); // Absolute FILETIME not supported.
        assert(!call(registry, "__imp__NtWaitForSingleObjectEx", c, {event, 1, 0, timeout}).handled());
        setTicks(0x8000000000000000ULL); // INT64_MIN must not overflow when negated.
        assert(!call(registry, "__imp__NtWaitForSingleObjectEx", c, {event, 1, 0, timeout}).handled());
        setTicks(std::uint64_t{0} - 600000001ULL);
        assert(!call(registry, "__imp__NtWaitForSingleObjectEx", c, {event, 1, 0, timeout}).handled());
        setTicks(std::uint64_t{0} - 20000ULL); // 2 milliseconds, no signal producer.
        const auto start = std::chrono::steady_clock::now();
        assert(invoke(registry, "__imp__NtWaitForSingleObjectEx", {event, 1, 0, timeout}) == 0x102);
        assert(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(2));
        assert(invoke(registry, "__imp__NtResumeThread", {worker, 0}) == 0);
        setTicks(std::uint64_t{0} - 10000000ULL); // A real signal completes before this one-second deadline.
        assert(invoke(registry, "__imp__NtWaitForSingleObjectEx", {event, 1, 0, timeout}) == 0);
        setTicks(0);
        assert(invoke(registry, "__imp__NtWaitForSingleObjectEx", {event, 1, 0, timeout}) == 0x102);
    }, {100, 32, 2}, [&](auto token) { assert(registry.bindGuestThreadToken(token)); });
    assert(result.execution.completed() && result.waitOperations == 2);
    registry.reset();
}
void testUnsuspendedCreationPublicationAndRollback(GuestAddressSpace& space) {
    GuestImportRegistry registry; (void)initialize(registry, space);
    GuestImportCall c;
    std::memset(space.translate(Out + 8), 0xA5, 8);
    assert(!call(registry, "__imp__ExCreateThread", c, {Out + 8, 0, Out + 12, 0, Code, 0x1234, 0}).handled());
    assert(registry.createdThreadCount() == 1 && read32(space, Out + 8) == 0xA5A5A5A5);
    int mode = 0; bool workerRan = false;
    std::optional<GuestPreparedThread> attempted;
    GuestCooperativeExecutor executor([&](auto body) -> std::thread {
        if (mode == 3) throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        return std::thread(std::move(body));
    });
    assert(registry.bindThreadResumeHandler([&](const auto& thread, std::int32_t priority) {
        assert(priority == 0 && thread.creationFlags == 0); attempted = thread;
        assert(read32(space, Out + 8) == 0xA5A5A5A5); // Not published before acceptance.
        if (mode == 0) return false;
        if (mode == 1) throw std::bad_alloc();
        if (mode == 2) throw std::runtime_error("synthetic enqueue failure");
        return executor.enqueue(thread.layout.threadAddress, [&, thread] {
            assert(read32(space, Out + 12) == thread.layout.threadId);
            assert(read32(space, Out + 8) != 0xA5A5A5A5);
            assert(!registry.lookupSuspendedThread(read32(space, Out + 8)));
            assert(thread.startContext == 0x1234 && thread.startRoutine == Code);
            workerRan = true; assert(registry.markGuestThreadReturned(thread.layout.threadAddress, 7));
        }, priority);
    }));
    const auto result = executor.run(Parent, [&] {
        for (mode = 0; mode < 4; ++mode) {
            if (mode == 2) {
                bool threw = false;
                try { (void)invoke(registry, "__imp__ExCreateThread", {Out + 8, 0, Out + 12, 0, Code, 0x1234, 0}); }
                catch (const std::runtime_error&) { threw = true; }
                assert(threw);
            } else assert(invoke(registry, "__imp__ExCreateThread", {Out + 8, 0, Out + 12, 0, Code, 0x1234, 0}) == 0xC0000017);
            assert(registry.createdThreadCount() == 1 && !workerRan);
            assert(read32(space, Out + 8) == 0xA5A5A5A5 && read32(space, Out + 12) == 0xA5A5A5A5);
            assert(attempted);
            const auto* memory = space.translate(attempted->allocation.base);
            assert(std::all_of(memory, memory + attempted->allocation.size, [](auto b) { return b == 0; }));
        }
        assert(invoke(registry, "__imp__ExCreateThread", {Out + 8, 0, Out + 12, 0, Code, 0x1234, 0}) == 0);
        assert(!workerRan && registry.createdThreadCount() == 2);
        const auto handle = read32(space, Out + 8);
        assert(!registry.lookupSuspendedThread(handle));
        assert(invoke(registry, "__imp__NtResumeThread", {handle, Out + 0x20}) == 0);
        assert(read32(space, Out + 0x20) == 0); // Already runnable, no duplicate worker.
        GuestExecutionGuard::checkpoint("synthetic_unsuspended_parent");
        assert(workerRan);
        std::memset(space.translate(Out + 0x28), 0, 8);
        assert(invoke(registry, "__imp__NtWaitForSingleObjectEx", {handle, 1, 0, Out + 0x28}) == 0);
    }, {100, 32, 2}, [&](auto token) { assert(registry.bindGuestThreadToken(token)); });
    assert(result.execution.completed() && result.workersCreated == 1 && result.workersEntered == 1);
    registry.reset();
}
void testNativeThreadExit(GuestAddressSpace& space) {
    GuestImportRegistry registry;
    const auto handle=initialize(registry,space);
    const auto thread=registry.lookupSuspendedThread(handle);
    std::memset(space.translate(Parent+0x14C),0,4);
    space.translate(Parent+0x14C)[3]=1;
    assert(thread && registry.bindInitialThreadObject(Parent,1));
    GuestCooperativeExecutor executor;
    bool workerContinued=false,parentContinued=false;
    std::uint32_t mutant=0;
    assert(registry.bindThreadResumeHandler([&](const auto& prepared,std::int32_t priority) {
        return executor.enqueue(prepared.layout.threadAddress,[&] {
            assert(invoke(registry,"__imp__NtWaitForSingleObjectEx",{mutant,1,0,0})==0);
            (void)invoke(registry,"__imp__ExTerminateThread",{0x87654321});
            workerContinued=true;
        },priority);
    }));
    assert(registry.bindThreadExitHandler([&](auto token,auto code) {
        return executor.terminateCurrentThread(token,[&] {return registry.markGuestThreadReturned(token,code);});
    }));
    assert(registry.bindThreadWaitHandler([&](const auto& ready){return executor.waitUntil(ready);}));
    GuestImportCall callResult;
    assert(!call(registry,"__imp__ExTerminateThread",callResult,{9}).handled());
    assert(!registry.guestThreadExitCode(Parent)); // Outside executor, no mutation.
    const auto result=executor.run(Parent,[&] {
        assert(invoke(registry,"__imp__NtCreateMutant",{Out+32,0,0})==0);
        mutant=read32(space,Out+32);
        assert(invoke(registry,"__imp__NtResumeThread",{handle,Out})==0);
        assert(invoke(registry,"__imp__NtWaitForSingleObjectEx",{handle,1,0,0})==0);
        assert(registry.guestThreadExitCode(thread->layout.threadAddress)==0x87654321);
        assert(!workerContinued);
        assert(invoke(registry,"__imp__NtWaitForSingleObjectEx",{mutant,1,0,0})==0x80);
        (void)invoke(registry,"__imp__ExTerminateThread",{0x12345678});
        parentContinued=true;
    },{100,32,3},[&](auto token){assert(registry.bindGuestThreadToken(token));});
    assert(result.execution.completed() && result.workersEntered==1);
    assert(!parentContinued && !workerContinued);
    assert(registry.guestThreadExitCode(Parent)==0x12345678);
    registry.reset();
}
} // namespace

int main() {
    GuestAddressSpace space;
    assert(space.reserve());
    testRealCallbackExecution(space);
    testEnqueueFailureCanRetry(space);
    testSingletonVirtualAffinity(space);
    testCooperativeSignalWaits(space);
    testWaitRejectionAndClose(space);
    testMultipleWaiters(space);
    testRelativeWaitDeadlines(space);
    testUnsuspendedCreationPublicationAndRollback(space);
    testNativeThreadExit(space);
}
