#include "guest_import_registry.h"
#include "guest_cooperative_executor.h"
#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <thread>

using namespace recomp::bootstrap;
namespace {
constexpr std::uint32_t Parent = 0x30000000, Scratch = 0x30001000, Timeout = Scratch + 16;
void put(GuestAddressSpace& s, std::uint32_t a, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) s.translate(a)[i] = std::uint8_t(v >> (24 - i * 8));
}
std::uint32_t get(GuestAddressSpace& s, std::uint32_t a) {
    const auto* b = s.translate(a);
    return (std::uint32_t{b[0]} << 24) | (std::uint32_t{b[1]} << 16) | (std::uint32_t{b[2]} << 8) | b[3];
}
void ticks(GuestAddressSpace& s, std::uint64_t v) { put(s, Timeout, v >> 32); put(s, Timeout + 4, v); }
GuestImportDispatchResult call(GuestImportRegistry& r, std::string_view name, GuestImportCall& c,
        std::initializer_list<std::uint32_t> args) {
    c = {}; c.returnValue = 0xDEADBEEF;
    std::copy(args.begin(), args.end(), c.arguments.begin());
    return r.dispatch(name, c);
}
std::uint32_t invoke(GuestImportRegistry& r, std::string_view name, std::initializer_list<std::uint32_t> args) {
    GuestImportCall c; assert(call(r, name, c, args).handled()); return c.returnValue;
}
std::uint32_t setup(GuestImportRegistry& r, GuestAddressSpace& s, bool physical = false) {
    assert(r.initialize(s, 0x10000000, 0x10000)); assert(r.bindGuestThreadToken(Parent));
    const GuestThreadStorageConfig config{{0x20000000, 0x100000}, {0x100000, 0x10000},
        {0x101000, 0x1000}, 0x100010, 4, 0x20, 4, 0x4000, 2};
    assert(r.bindThreadStorageArena(config, {{Parent, 0x2000}}, [](auto a) { return a == 0x101000; }));
    // These fixtures enqueue bounded host lambdas directly, never ExCreateThread.
    assert(r.bindThreadResumeHandler([](const auto&, std::int32_t) { return false; }));
    ticks(s, 0);
    if (physical) {
        assert(r.bindPhysicalMemoryArena(0xA0000000, 0x10000, 0));
        return invoke(r, "__imp__MmAllocatePhysicalMemoryEx", {1, 0x1000, 4, 0, UINT32_MAX, 0});
    }
    put(s, Scratch, 0); put(s, Scratch + 4, 0x1000);
    assert(invoke(r, "__imp__NtAllocateVirtualMemory", {Scratch, Scratch + 4, 0x3000, 4, 0}) == 0);
    return get(s, Scratch);
}
void event(GuestAddressSpace& s, std::uint32_t a, std::uint32_t header, std::uint32_t signal = 0) {
    put(s, a, header); put(s, a + 4, signal); put(s, a + 8, a + 8); put(s, a + 12, a + 8);
}
void bind(GuestImportRegistry& r, GuestCooperativeExecutor& e) {
    assert(r.bindThreadWaitHandler([&](const auto& ready) { return e.waitUntil(ready); }));
    assert(r.bindThreadTimedWaitHandler([&](const auto& ready, auto deadline) { return e.waitUntil(ready, deadline); }));
}
void testImmediateAndValidation(GuestAddressSpace& s) {
    GuestImportRegistry r; const auto owner = setup(r, s, true), a = owner + 0xFF0;
    GuestImportCall c;
    for (const auto header : {0U, 0x01000000U, 0x400U, 0x01000400U}) {
        std::fill_n(s.translate(owner), 0x1000, 0xA5); event(s, a, header);
        assert(invoke(r, "__imp__KeWaitForSingleObject", {a, 3, 1, 0, Timeout}) == 0x102);
        assert(invoke(r, "__imp__KeSetEvent", {a, 0, 0}) == 0);
        assert(invoke(r, "__imp__KeSetEvent", {a, 0, 0}) == 1);
        assert(invoke(r, "__imp__KeWaitForSingleObject", {a, 0, 0, 0, Timeout}) == 0);
        assert(get(s, a + 4) == (header >> 24 ? 0U : 1U));
        assert(invoke(r, "__imp__KeResetEvent", {a}) == (header >> 24 ? 0U : 1U));
        assert(invoke(r, "__imp__KeResetEvent", {a}) == 0);
        assert(get(s, a) == header && get(s, a + 8) == a + 8 && get(s, a + 12) == a + 8);
        assert(std::all_of(s.translate(owner), s.translate(a), [](auto b) { return b == 0xA5; }));
    }
    event(s, a, 0);
    for (const auto bad : {0U, a + 1, a + 4, 0x7FC8112CU, 0xFFFFFFF8U, 0x10000000U}) {
        assert(!call(r, "__imp__KeSetEvent", c, {bad, 0, 0}).handled());
        assert(!call(r, "__imp__KeWaitForSingleObject", c, {bad, 3, 1, 0, Timeout}).handled());
    }
    const auto ro = invoke(r, "__imp__MmAllocatePhysicalMemoryEx", {1, 0x1000, 2, 0, UINT32_MAX, 0});
    event(s, ro, 0); assert(!call(r, "__imp__KeSetEvent", c, {ro, 0, 0}).handled());
    for (auto badHeader : {2U << 24, 5U << 24, 15U << 24, 0x80000000U, 0xFFFFFFFFU}) {
        put(s, a, badHeader); assert(!call(r, "__imp__KeResetEvent", c, {a}).handled());
        assert(get(s, a) == badHeader);
    }
    event(s, a, 0); put(s, a + 8, 0); assert(!call(r, "__imp__KeResetEvent", c, {a}).handled());
    event(s, a, 0, 2); assert(!call(r, "__imp__KeResetEvent", c, {a}).handled());
    event(s, a, 0);
    for (const auto args : {std::array<std::uint32_t, 2>{1, 0}, {0, 1}, {0xFFFFFFFF, 0}})
        assert(!call(r, "__imp__KeSetEvent", c, {a, args[0], args[1]}).handled());
    for (auto badTimeout : {Timeout + 1, 0xFFFFFFFCU, 0x7FC81128U})
        assert(!call(r, "__imp__KeWaitForSingleObject", c, {a, 3, 1, 0, badTimeout}).handled());
    assert(!call(r, "__imp__KeWaitForSingleObject", c, {a, 4, 1, 0, Timeout}).handled());
    assert(!call(r, "__imp__KeWaitForSingleObject", c, {a, 3, 2, 0, Timeout}).handled());
    assert(!call(r, "__imp__KeWaitForSingleObject", c, {a, 3, 1, 1, Timeout}).handled());
    assert(!call(r, "__imp__KeWaitForSingleObject", c, {a, 3, 1, 0, 0}).handled());
    r.unbindPhysicalMemoryArena(); assert(!call(r, "__imp__KeResetEvent", c, {a}).handled());
}
void testQueuedSignalsSurviveReset(GuestAddressSpace& s, bool automatic) {
    GuestImportRegistry r; const auto owner = setup(r, s), a = owner + 4;
    event(s, a, automatic ? 0x01000000 : 0);
    GuestCooperativeExecutor executor; bind(r, executor);
    int parked = 0, woke = 0;
    const auto result = executor.run(Parent, [&] {
        for (auto token : {Parent + 0x100, Parent + 0x200}) {
            assert(executor.enqueue(token, [&] {
                ++parked;
                assert(invoke(r, "__imp__KeWaitForSingleObject", {a, 3, 1, 0, 0}) == 0);
                ++woke;
            }));
        }
        while (parked < 2) GuestExecutionGuard::checkpoint("park_both");
        assert(woke == 0);
        assert(invoke(r, "__imp__KeSetEvent", {a, 0, 0}) == 0);
        // No checkpoint between Set/Reset: already satisfied waits must survive.
        assert(invoke(r, "__imp__KeResetEvent", {a}) == (automatic ? 0U : 1U));
        assert(executor.waitUntil([&] { return woke == (automatic ? 1 : 2); }));
        if (automatic) {
            assert(woke == 1);
            assert(invoke(r, "__imp__KeSetEvent", {a, 0, 0}) == 0);
            assert(invoke(r, "__imp__KeResetEvent", {a}) == 0);
            assert(executor.waitUntil([&] { return woke == 2; }));
        }
        assert(get(s, a + 4) == 0);
    }, {100, 1, 3}, [&](auto token) { assert(r.bindGuestThreadToken(token)); });
    assert(result.execution.completed() && woke == 2);
}
void testTimeoutAndPinCleanup(GuestAddressSpace& s, bool physical) {
    GuestImportRegistry r; const auto owner = setup(r, s, physical), a = owner + 4;
    event(s, a, 0); GuestCooperativeExecutor executor; bind(r, executor);
    const auto release = [&](GuestImportCall& c) {
        if (physical) return call(r, "__imp__MmFreePhysicalMemory", c, {1, owner});
        put(s, Scratch, owner); put(s, Scratch + 4, 0);
        return call(r, "__imp__NtFreeVirtualMemory", c, {Scratch, Scratch + 4, 0x8000, 0});
    };
    const auto result = executor.run(Parent, [&] {
        GuestImportCall c;
        for (auto v : {1ULL, 0x8000000000000000ULL, 0ULL - 600000001ULL}) {
            ticks(s, v); assert(!call(r, "__imp__KeWaitForSingleObject", c, {a, 3, 1, 0, Timeout}).handled());
        }
        ticks(s, 0ULL - 20000);
        const auto start = std::chrono::steady_clock::now();
        assert(invoke(r, "__imp__KeWaitForSingleObject", {a, 3, 1, 0, Timeout}) == 0x102);
        assert(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(2));
        assert(executor.enqueue(Parent + 0x100, [&] {
            GuestImportCall child; assert(!release(child).handled());
            assert(get(s, a + 8) == a + 8);
            assert(invoke(r, "__imp__KeSetEvent", {a, 0, 0}) == 0);
        }));
        ticks(s, 0ULL - 10000000);
        assert(invoke(r, "__imp__KeWaitForSingleObject", {a, 3, 1, 0, Timeout}) == 0);
        assert(release(c).handled()); // All parked references were removed.
        assert(!call(r, "__imp__KeResetEvent", c, {a}).handled());
    }, {100, 32, 2}, [&](auto token) { assert(r.bindGuestThreadToken(token)); });
    assert(result.execution.completed());
}
void testCancelledWaitReleasesPin(GuestAddressSpace& s) {
    for (int mode = 0; mode < 3; ++mode) {
        GuestImportRegistry r; const auto owner = setup(r, s, true), a = owner + 4;
        event(s, a, 0);
        assert(r.bindThreadWaitHandler([&](const auto& ready) {
            assert(!ready());
            if (mode == 0) throw std::runtime_error("synthetic cancellation");
            if (mode == 1) return false;
            put(s, a, 0x01000000); assert(ready()); return true; // Mutated header rejects.
        }));
        GuestImportCall c; bool threw = false;
        try { assert(!call(r, "__imp__KeWaitForSingleObject", c, {a, 3, 1, 0, 0}).handled()); }
        catch (const std::runtime_error&) { threw = true; }
        assert(threw == (mode == 0));
        assert(call(r, "__imp__MmFreePhysicalMemory", c, {1, owner}).handled());
    }
}
void testSettledWaitCannotStealAnotherSignal(GuestAddressSpace& s) {
    GuestImportRegistry r; const auto owner = setup(r, s, true), a = owner + 4;
    event(s, a, 0x01000000);
    assert(r.bindThreadWaitHandler([&](const auto& ready) {
        put(s, a + 4, 1); // An actual serialized guest write, consumed by polling.
        assert(ready() && get(s, a + 4) == 0);
        assert(invoke(r, "__imp__KeSetEvent", {a, 0, 0}) == 0);
        assert(get(s, a + 4) == 1); // First wait is settled, even before it returns.
        assert(ready() && get(s, a + 4) == 1); // Repeated readiness is idempotent.
        return true;
    }));
    assert(invoke(r, "__imp__KeWaitForSingleObject", {a, 3, 1, 0, 0}) == 0);
    assert(invoke(r, "__imp__KeWaitForSingleObject", {a, 3, 1, 0, Timeout}) == 0);
    assert(get(s, a + 4) == 0);
}
void testSignalAfterDeadlineIsNotConsumed(GuestAddressSpace& s) {
    GuestImportRegistry r; const auto owner = setup(r, s), a = owner + 4;
    event(s, a, 0x01000000);
    assert(r.bindThreadWaitHandler([](const auto&) { return false; }));
    assert(r.bindThreadTimedWaitHandler([&](const auto& ready, auto deadline) {
        std::this_thread::sleep_until(deadline);
        assert(invoke(r, "__imp__KeSetEvent", {a, 0, 0}) == 0);
        assert(ready() && get(s, a + 4) == 1);
        return true;
    }));
    ticks(s, 0ULL - 20000);
    assert(invoke(r, "__imp__KeWaitForSingleObject", {a, 3, 1, 0, Timeout}) == 0x102);
    ticks(s, 0);
    assert(invoke(r, "__imp__KeWaitForSingleObject", {a, 3, 1, 0, Timeout}) == 0);
}
void testRegistryReplacementCannotSatisfyOldWait(GuestAddressSpace& s) {
    GuestImportRegistry r; const auto owner = setup(r, s, true), a = owner + 4;
    event(s, a, 0x01000000);
    // Isolated host-lifecycle misuse fixture, no concurrent guest workers.
    // Even a same-address fresh registry/allocation must not satisfy an old wait.
    assert(r.bindThreadWaitHandler([&](const auto& ready) {
        r.reset(); assert(setup(r, s, true) == owner); event(s, a, 0x01000000, 1);
        assert(ready() && get(s, a + 4) == 1); return true;
    }));
    GuestImportCall c;
    assert(!call(r, "__imp__KeWaitForSingleObject", c, {a, 3, 1, 0, 0}).handled());
    assert(invoke(r, "__imp__KeWaitForSingleObject", {a, 3, 1, 0, Timeout}) == 0);
}
constexpr std::uint32_t Objects=Scratch+0x100;
void testEventPaddingIsOpaque(GuestAddressSpace& s) {
    GuestImportRegistry r;const auto owner=setup(r,s,true);put(s,Objects,owner);
    for (unsigned type=0;type<256;++type) {
        for (std::uint32_t padding:{0U,0x88000U,0x50U,0xFFFFFFU,0x123456U}) {
            const auto header=(type<<24)|padding;event(s,owner,header,1);
            GuestImportCall c;
            const auto result=call(r,"__imp__KeWaitForMultipleObjects",c,{1,Objects,1,3,1,0,Timeout,0});
            assert(result.handled()==(type<=1));
            assert(get(s,owner)==header && get(s,owner+8)==owner+8 && get(s,owner+12)==owner+8);
            assert(get(s,owner+4)==(type==1?0U:1U));
        }
    }
    for (unsigned type=0;type<2;++type) for (unsigned byte=0;byte<3;++byte) for (unsigned value=0;value<256;++value) {
        const auto header=(type<<24)|(value<<(byte*8));event(s,owner,header);
        assert(invoke(r,"__imp__KeSetEvent",{owner,0,0})==0);
        assert(invoke(r,"__imp__KeWaitForSingleObject",{owner,3,1,0,Timeout})==0);
        assert(get(s,owner)==header);
        assert(invoke(r,"__imp__KeResetEvent",{owner})==(type?0U:1U));
        assert(get(s,owner)==header);
    }
    // Opaque bytes are preserved on queued wakeups as well. A parked object's
    // entire header must still stay stable; accepting padding does not permit
    // an allocation to be repurposed while an old wait holds it.
    for (unsigned type=0;type<2;++type) for (bool mutate:{false,true}) {
        GuestImportRegistry queued;const auto address=setup(queued,s,true);
        const auto header=(type<<24)|0x88050U;
        put(s,Objects,address);event(s,address,header);
        assert(queued.bindThreadWaitHandler([&](const auto& ready) {
            assert(!ready());
            if (mutate) put(s,address,header^1U);
            else assert(invoke(queued,"__imp__KeSetEvent",{address,0,0})==0);
            assert(ready());return true;
        }));
        GuestImportCall c;
        const auto result=call(queued,"__imp__KeWaitForMultipleObjects",c,{1,Objects,1,3,1,0,0,0});
        assert(result.handled()==!mutate);
        assert(get(s,address)==(mutate?header^1U:header));
        assert(get(s,address+4)==(!mutate && !type?1U:0U));
        assert(call(queued,"__imp__MmFreePhysicalMemory",c,{1,address}).handled());
    }
}
void testWaitAnyImmediate(GuestAddressSpace& s) {
    GuestImportRegistry r; const auto owner=setup(r,s,true);
    assert(!r.inspectDispatcherEvent(0));assert(!r.inspectDispatcherEvent(0x7FC81100));
    assert(!r.inspectDispatcherEvent(0xFFFFFFFF));assert(!r.inspectDispatcherEvent(0x2000));
    event(s,owner,0x01000400,1);
    assert((r.inspectDispatcherEvent(owner)==std::array<std::uint32_t,3>{0x01000400,1,1}));
    put(s,owner,0x05000400);
    assert((r.inspectDispatcherEvent(owner)==std::array<std::uint32_t,3>{0x05000400,1,0}));
    assert(get(s,owner)==0x05000400 && get(s,owner+4)==1);
    for (std::uint32_t count=1;count<=64;++count) {
        for (std::uint32_t i=0;i<count;++i) {
            put(s,Objects+i*4,owner+i*16);
            event(s,owner+i*16,0x01000400,i==count-1?1:0);
        }
        assert(invoke(r,"__imp__KeWaitForMultipleObjects",{count,Objects,1,3,1,0,Timeout,0})==count-1);
        assert(get(s,owner+(count-1)*16+4)==0);
        assert(invoke(r,"__imp__KeWaitForMultipleObjects",{count,Objects,1,0,0,0,Timeout,0})==0x102);
    }
    for (unsigned automatic=0;automatic<2;++automatic) {
        for (unsigned i=0;i<3;++i) {put(s,Objects+i*4,owner+i*16);event(s,owner+i*16,automatic<<24,1);}
        assert(invoke(r,"__imp__KeWaitForMultipleObjects",{3,Objects,1,3,1,0,Timeout,Objects+32})==0);
        assert(get(s,owner+4)==(automatic?0U:1U));
        assert(get(s,owner+20)==1 && get(s,owner+36)==1);
    }
    event(s,owner,0x01000000,1); event(s,owner+16,0);
    GuestImportCall c;
    for (auto count:{0U,65U,UINT32_MAX})
        assert(!call(r,"__imp__KeWaitForMultipleObjects",c,{count,Objects,1,3,1,0,Timeout,0}).handled());
    for (auto array:{0U,Objects+1,0xFFFFFFFCU,0x7FC81100U})
        assert(!call(r,"__imp__KeWaitForMultipleObjects",c,{2,array,1,3,1,0,Timeout,0}).handled());
    for (auto kind:{0U,2U,UINT32_MAX})
        assert(!call(r,"__imp__KeWaitForMultipleObjects",c,{2,Objects,kind,3,1,0,Timeout,0}).handled());
    for (auto timeout:{Timeout+1,0xFFFFFFFCU,0x7FC81100U})
        assert(!call(r,"__imp__KeWaitForMultipleObjects",c,{2,Objects,1,3,1,0,timeout,0}).handled());
    for (auto bad:{0U,owner,owner+4,0xFFFFFFF8U}) {
        put(s,Objects+4,bad);
        assert(!call(r,"__imp__KeWaitForMultipleObjects",c,{2,Objects,1,3,1,0,Timeout,0}).handled());
        assert(get(s,owner+4)==1); // Invalid later object cannot consume earlier signal.
    }
    put(s,Objects+4,owner+16);
    assert(!call(r,"__imp__KeWaitForMultipleObjects",c,{2,Objects,1,4,1,0,Timeout,0}).handled());
    assert(!call(r,"__imp__KeWaitForMultipleObjects",c,{2,Objects,1,3,2,0,Timeout,0}).handled());
    assert(!call(r,"__imp__KeWaitForMultipleObjects",c,{2,Objects,1,3,1,1,Timeout,0}).handled());
    assert(!call(r,"__imp__KeWaitForMultipleObjects",c,{2,Objects,1,3,1,0,Timeout,0xFFFFFFFF}).handled());
    assert(get(s,owner+4)==1);
}
void testWaitAnyReservesOnlyOneSignal(GuestAddressSpace& s, unsigned winner) {
    GuestImportRegistry r; const auto owner=setup(r,s,true);
    for (unsigned i=0;i<2;++i) {put(s,Objects+i*4,owner+i*16);event(s,owner+i*16,0x01000000);}
    GuestCooperativeExecutor executor; bind(r,executor);
    unsigned parked=0,woke=0;
    const auto result=executor.run(Parent,[&] {
        assert(executor.enqueue(Parent+0x100,[&] {
            ++parked;
            assert(invoke(r,"__imp__KeWaitForMultipleObjects",{2,Objects,1,3,1,0,0,0})==winner);
            ++woke;
        }));
        assert(executor.enqueue(Parent+0x200,[&] {
            ++parked;
            assert(invoke(r,"__imp__KeWaitForSingleObject",{owner+(1-winner)*16,3,1,0,0})==0);
            ++woke;
        }));
        while (parked<2) GuestExecutionGuard::checkpoint("park_wait_any_and_single");
        assert(woke==0);
        // No checkpoint between signals: the already satisfied group must not
        // consume a second auto-reset signal intended for the single waiter.
        assert(invoke(r,"__imp__KeSetEvent",{owner+winner*16,0,0})==0);
        assert(invoke(r,"__imp__KeResetEvent",{owner+winner*16})==0);
        assert(invoke(r,"__imp__KeSetEvent",{owner+(1-winner)*16,0,0})==0);
        assert(invoke(r,"__imp__KeResetEvent",{owner+(1-winner)*16})==0);
        assert(executor.waitUntil([&] {return woke==2;}));
    },{100,1,3},[&](auto token) {assert(r.bindGuestThreadToken(token));});
    assert(result.execution.completed() && woke==2);
}
void testWaitAnyCancellationAndTimeout(GuestAddressSpace& s) {
    for (int mode=0;mode<5;++mode) {
        GuestImportRegistry r; const auto a=setup(r,s,true);
        const auto b=invoke(r,"__imp__MmAllocatePhysicalMemoryEx",{1,0x1000,4,0,UINT32_MAX,0});
        put(s,Objects,a);put(s,Objects+4,b);event(s,a,0x01000000);event(s,b,0x01000000);
        GuestImportCall c;
        assert(r.bindThreadWaitHandler([&](const auto& ready) {
            assert(!ready());
            GuestImportCall release;
            assert(!call(r,"__imp__MmFreePhysicalMemory",release,{1,a}).handled());
            assert(!call(r,"__imp__MmFreePhysicalMemory",release,{1,b}).handled());
            if (mode==0) throw std::runtime_error("cancel_wait_any");
            if (mode==1) return false;
            if (mode==2) {put(s,b,0);assert(ready());return true;}
            // Polling real direct writes selects the lowest ready index; a
            // repeated readiness check cannot consume a second signal.
            put(s,b+4,1);put(s,a+4,1);assert(ready());
            assert(get(s,a+4)==0 && get(s,b+4)==1);assert(ready());return true;
        }));
        assert(r.bindThreadTimedWaitHandler([&](const auto& ready,auto deadline) {
            assert(!ready());std::this_thread::sleep_until(deadline);
            assert(invoke(r,"__imp__KeSetEvent",{b,0,0})==0);
            assert(ready() && get(s,b+4)==1);return true;
        }));
        bool threw=false;
        try {
            ticks(s,0ULL-20000);
            const auto result=call(r,"__imp__KeWaitForMultipleObjects",c,{2,Objects,1,3,1,0,mode==4?Timeout:0,0});
            assert(result.handled()==(mode>=3));
            if (result.handled()) assert(c.returnValue==(mode==4?0x102U:0U));
        } catch (const std::runtime_error&) {threw=true;}
        assert(threw==(mode==0));
        assert(call(r,"__imp__MmFreePhysicalMemory",c,{1,a}).handled());
        assert(call(r,"__imp__MmFreePhysicalMemory",c,{1,b}).handled());
    }
}
void testWaitAnyNotificationBroadcast(GuestAddressSpace& s) {
    GuestImportRegistry r;const auto owner=setup(r,s,true);
    put(s,Objects,owner);put(s,Objects+4,owner+16);event(s,owner,0);event(s,owner+16,0);
    GuestCooperativeExecutor executor;bind(r,executor);unsigned parked=0,woke=0;
    const auto result=executor.run(Parent,[&] {
        for (unsigned i=1;i<=3;++i) assert(executor.enqueue(Parent+i*0x100,[&] {
            ++parked;assert(invoke(r,"__imp__KeWaitForMultipleObjects",{2,Objects,1,3,1,0,0,0})==1);++woke;
        }));
        while (parked<3) GuestExecutionGuard::checkpoint("park_notification_groups");
        assert(invoke(r,"__imp__KeSetEvent",{owner+16,0,0})==0);
        assert(invoke(r,"__imp__KeResetEvent",{owner+16})==1);
        assert(executor.waitUntil([&] {return woke==3;}));
    },{100,1,4},[&](auto token) {assert(r.bindGuestThreadToken(token));});
    assert(result.execution.completed() && woke==3);
}
void testWaitAnyRegistryReplacement(GuestAddressSpace& s) {
    GuestImportRegistry r;const auto owner=setup(r,s,true);
    put(s,Objects,owner);put(s,Objects+4,owner+16);event(s,owner,0x01000000);event(s,owner+16,0);
    assert(r.bindThreadWaitHandler([&](const auto& ready) {
        r.reset();assert(setup(r,s,true)==owner);
        event(s,owner,0x01000000,1);event(s,owner+16,0,1);
        assert(ready() && get(s,owner+4)==1 && get(s,owner+20)==1);return true;
    }));
    GuestImportCall c;
    assert(!call(r,"__imp__KeWaitForMultipleObjects",c,{2,Objects,1,3,1,0,0,0}).handled());
    assert(invoke(r,"__imp__KeWaitForSingleObject",{owner,3,1,0,Timeout})==0);
}
}
int main() {
    GuestAddressSpace space; assert(space.reserve());
    testImmediateAndValidation(space);
    testQueuedSignalsSurviveReset(space, false); testQueuedSignalsSurviveReset(space, true);
    testTimeoutAndPinCleanup(space, false); testTimeoutAndPinCleanup(space, true);
    testCancelledWaitReleasesPin(space);
    testSettledWaitCannotStealAnotherSignal(space);
    testSignalAfterDeadlineIsNotConsumed(space);
    testRegistryReplacementCannotSatisfyOldWait(space);
    testWaitAnyImmediate(space);
    testEventPaddingIsOpaque(space);
    testWaitAnyReservesOnlyOneSignal(space,0);testWaitAnyReservesOnlyOneSignal(space,1);
    testWaitAnyCancellationAndTimeout(space);
    testWaitAnyNotificationBroadcast(space);testWaitAnyRegistryReplacement(space);
}
