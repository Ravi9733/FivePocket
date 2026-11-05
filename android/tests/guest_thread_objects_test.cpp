#include "guest_import_registry.h"
#include "guest_data_variable_binding.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <initializer_list>

using namespace recomp::bootstrap;
namespace {
constexpr std::uint32_t Heap = 0x10000000U, Arena = 0x20000000U;
constexpr std::uint32_t Output = 0x30000000U, Initial = 0x31000000U, Type = 0x40000000U;
constexpr std::uint32_t Code = 0x00101000U;

void write32(GuestAddressSpace& s, std::uint32_t address, std::uint32_t value) {
    auto* p = s.translate(address);
    p[0] = value >> 24; p[1] = value >> 16; p[2] = value >> 8; p[3] = value;
}
std::uint32_t read32(GuestAddressSpace& s, std::uint32_t address) {
    const auto* p = s.translate(address);
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}
GuestImportDispatchResult call(GuestImportRegistry& r, std::string_view name,
        GuestImportCall& c, std::initializer_list<std::uint32_t> args) {
    c = {};
    c.returnValue = 0x123456789ABCDEF0ULL;
    std::copy(args.begin(), args.end(), c.arguments.begin());
    return r.dispatch(name, c);
}
std::uint32_t invoke(GuestImportRegistry& r, std::string_view name,
        std::initializer_list<std::uint32_t> args) {
    GuestImportCall c;
    assert(call(r, name, c, args).handled());
    return static_cast<std::uint32_t>(c.returnValue);
}
void initialize(GuestImportRegistry& registry, GuestAddressSpace& space) {
    assert(registry.initialize(space, Heap, 0x10000));
    assert(registry.bindGuestThreadToken(Initial));
    assert(registry.bindGuestThreadTlsStorage(Initial, Initial + 0x1000, 4));
    write32(space, Initial + 0x14C, 1);
    assert(registry.bindInitialThreadObject(Initial, 1));
    assert(registry.bindInitialThreadObject(Initial, 1));
    assert(!registry.bindInitialThreadObject(Initial, 2));
    GuestThreadStorageConfig config{{Arena, 0x100000}, {0x100000, 0x10000}, {Code, 0x1000},
            0x100010, 4, 0x20, 4, 0x4000, 2};
    assert(registry.bindThreadStorageArena(config, {{Initial, 0x2000}, {Type, 4}},
            [](auto address) { return address == Code; }));
    write32(space, Type, 0);
    assert(registry.bindThreadObjectType(Type));
}

void testReferencesPriorityAndClose(GuestAddressSpace& space) {
    GuestImportRegistry registry;
    initialize(registry, space);
    GuestImportCall network;
    assert(!call(registry, "__imp__NetDll_WSACleanup", network, {1}).handled());
    assert(registry.bindNetworkLifecycle());
    assert(invoke(registry, "__imp__NetDll_WSAGetLastError", {}) == 0);
    assert(invoke(registry, "__imp__NetDll_WSACleanup", {1}) == 0xFFFFFFFFU);
    assert(invoke(registry, "__imp__NetDll_WSAGetLastError", {}) == 10093);
    assert(invoke(registry, "__imp__NetDll_XNetCleanup", {1}) == 0);
    assert(invoke(registry, "__imp__NetDll_WSAGetLastError", {}) == 10093);
    assert(!call(registry, "__imp__NetDll_WSACleanup", network, {0}).handled());
    write32(space, Initial + 0x160, 0);
    GuestImportCall boost;
    assert(!call(registry, "__imp__KeSetDisableBoostThread", boost, {Initial, 1}).handled());
    assert(registry.bindThreadResumeHandler([](const auto&, std::int32_t) { return false; }));
    assert(invoke(registry, "__imp__KeSetDisableBoostThread", {Initial, 1}) == 0);
    assert(invoke(registry, "__imp__KeSetDisableBoostThread", {Initial, 1}) == 1);
    assert(invoke(registry, "__imp__KeSetDisableBoostThread", {Initial, 0}) == 1);
    assert(!call(registry, "__imp__KeSetDisableBoostThread", boost, {Initial, 2}).handled());
    assert(!call(registry, "__imp__KeSetDisableBoostThread", boost, {Initial + 4, 1}).handled());
    assert(invoke(registry, "__imp__KeSetDisableBoostThread", {Initial, 0}) == 0);
    assert(registry.bindThreadObjectType(Type));
    assert(!registry.bindThreadObjectType(Type + 4));
    assert(invoke(registry, "__imp__ObReferenceObjectByHandle", {0xFFFFFFFEU, Type, Output}) == 0);
    assert(read32(space, Output) == Initial && registry.threadObjectReferenceCount(Initial) == 1);
    assert(invoke(registry, "__imp__KeQueryBasePriorityThread", {Initial}) == 0);
    GuestImportCall c;
    assert(!call(registry, "__imp__KeSetBasePriorityThread", c, {Initial, 1}).handled());
    const auto request = registry.lastThreadPriorityRequest();
    assert(request && request->increment == 1 && request->previousIncrement == 0
            && request->initial && request->current && !request->suspended && !request->returned);
    assert(invoke(registry, "__imp__KeSetBasePriorityThread", {Initial, 0}) == 0);
    bool acceptPriority = false;
    int priorityChanges = 0;
    assert(!registry.bindThreadPriorityHandler({}));
    assert(registry.bindThreadPriorityHandler([&](auto token, auto increment) {
        assert(token == Initial && ((increment >= -2 && increment <= 2) || increment == 16 || increment == -16));
        ++priorityChanges;
        return acceptPriority;
    }));
    assert(!registry.bindThreadPriorityHandler([](auto, auto) { return true; }));
    assert(!call(registry, "__imp__KeSetBasePriorityThread", c, {Initial, 2}).handled());
    assert(invoke(registry, "__imp__KeQueryBasePriorityThread", {Initial}) == 0);
    acceptPriority = true;
    assert(invoke(registry, "__imp__KeSetBasePriorityThread", {Initial, 2}) == 0);
    assert(invoke(registry, "__imp__KeSetBasePriorityThread", {Initial, 2}) == 2);
    assert(priorityChanges == 2); // An idempotent request does not call the setter.
    acceptPriority = false;
    assert(!call(registry, "__imp__KeSetBasePriorityThread", c, {Initial, 17}).handled());
    assert(invoke(registry, "__imp__KeQueryBasePriorityThread", {Initial}) == 2);
    acceptPriority = true;
    assert(invoke(registry, "__imp__KeSetBasePriorityThread", {Initial, 17}) == 2);
    assert(invoke(registry, "__imp__KeQueryBasePriorityThread", {Initial}) == 16);
    const auto changes = priorityChanges;
    assert(invoke(registry, "__imp__KeSetBasePriorityThread", {Initial, 0x7FFFFFFF}) == 16);
    assert(priorityChanges == changes); // Same saturation tier is idempotent.
    assert(invoke(registry, "__imp__KeSetBasePriorityThread", {Initial, 0x80000000}) == 16);
    assert(invoke(registry, "__imp__KeQueryBasePriorityThread", {Initial}) == 0xFFFFFFF0U);
    assert(invoke(registry, "__imp__KeSetBasePriorityThread", {Initial, 0xFFFFFFEF}) == 0xFFFFFFF0U);
    for (auto unsupported : {3U, 15U, 0xFFFFFFFDU, 0xFFFFFFF1U}) {
        assert(!call(registry, "__imp__KeSetBasePriorityThread", c, {Initial, unsupported}).handled());
        assert(invoke(registry, "__imp__KeQueryBasePriorityThread", {Initial}) == 0xFFFFFFF0U);
    }
    assert(invoke(registry, "__imp__KeSetBasePriorityThread", {Initial, 2}) == 0xFFFFFFF0U);
    assert(invoke(registry, "__imp__KeSetBasePriorityThread", {Initial, 0xFFFFFFFEU}) == 2);
    assert(invoke(registry, "__imp__KeSetBasePriorityThread", {Initial, 0}) == 0xFFFFFFFEU);
    assert(call(registry, "__imp__ObDereferenceObject", c, {Initial}).handled());
    assert(c.returnValue == 0x123456789ABCDEF0ULL); // Void descriptor preserves r3.
    assert(registry.threadObjectReferenceCount(Initial) == 0);
    assert(!call(registry, "__imp__ObDereferenceObject", c, {Initial}).handled());
    assert(invoke(registry, "__imp__ExCreateThread", {Output, 0, Output + 4, 0, Code, 0, 1}) == 0);
    const auto handle = read32(space, Output);
    const auto thread = registry.lookupSuspendedThread(handle);
    assert(thread);
    const auto object = thread->layout.threadAddress;
    assert(invoke(registry, "__imp__ObReferenceObjectByHandle", {handle, 0, Output}) == 0);
    assert(read32(space, Output) == object);
    assert(invoke(registry, "__imp__ObReferenceObjectByHandle", {handle, Type, Output}) == 0);
    assert(registry.threadObjectReferenceCount(object) == 2);
    write32(space, Output, 0xA5A5A5A5);
    assert(invoke(registry, "__imp__ObReferenceObjectByHandle", {handle, Type + 4, Output}) == 0xC0000024U);
    assert(read32(space, Output) == 0xA5A5A5A5 && registry.threadObjectReferenceCount(object) == 2);
    for (auto badOutput : {0U, Output + 1, object, Initial, Type}) {
        assert(!call(registry, "__imp__ObReferenceObjectByHandle", c, {handle, Type, badOutput}).handled());
        assert(registry.threadObjectReferenceCount(object) == 2);
    }
    auto previous = 0U;
    for (std::int32_t increment : {-2, -1, 0, 1, 2}) {
        const auto bits = static_cast<std::uint32_t>(increment);
        assert(invoke(registry, "__imp__KeSetBasePriorityThread", {object, bits}) == previous);
        assert(invoke(registry, "__imp__KeQueryBasePriorityThread", {object}) == bits);
        previous = bits;
    }
    assert(!call(registry, "__imp__KeSetBasePriorityThread", c, {object, 15}).handled());
    assert(invoke(registry, "__imp__KeQueryBasePriorityThread", {object}) == 2);
    assert(!call(registry, "__imp__KeQueryBasePriorityThread", c, {object + 4}).handled());
    write32(space, object + 0x14C, 0);
    assert(!call(registry, "__imp__ObDereferenceObject", c, {object}).handled());
    assert(registry.threadObjectReferenceCount(object) == 2);
    write32(space, object + 0x14C, thread->layout.threadId);
    assert(invoke(registry, "__imp__NtClose", {handle}) == 0);
    assert(invoke(registry, "__imp__ObReferenceObjectByHandle", {handle, Type, Output}) == 0xC0000008U);
    assert(invoke(registry, "__imp__KeQueryBasePriorityThread", {object}) == 2);
    assert(call(registry, "__imp__ObDereferenceObject", c, {object}).handled());
    assert(call(registry, "__imp__ObDereferenceObject", c, {object}).handled());
    assert(!call(registry, "__imp__ObDereferenceObject", c, {object}).handled());
    assert(registry.createdThreadCount() == 1 && registry.threadObjectReferenceCount(object) == 0);
    assert(invoke(registry, "__imp__NtCreateSemaphore", {Output, 0, 0, 1}) == 0);
    const auto semaphore = read32(space, Output);
    assert(invoke(registry, "__imp__ObReferenceObjectByHandle", {semaphore, Type, Output}) == 0xC0000024U);
    assert(!call(registry, "__imp__ObReferenceObjectByHandle", c, {semaphore, 0, Output}).handled());
    registry.reset();
    assert(registry.threadObjectReferenceCount(object) == 0 && registry.createdThreadCount() == 0);
    assert(!registry.lastThreadPriorityRequest());
}

void testReferenceLimit(GuestAddressSpace& space) {
    GuestImportRegistry registry;
    initialize(registry, space);
    for (std::uint32_t i = 0; i < GuestImportRegistry::MaximumThreadObjectReferences; ++i) {
        assert(invoke(registry, "__imp__ObReferenceObjectByHandle", {0xFFFFFFFEU, 0, Output}) == 0);
    }
    write32(space, Output, 0xA5A5A5A5);
    assert(invoke(registry, "__imp__ObReferenceObjectByHandle", {0xFFFFFFFEU, Type, Output}) == 0xC0000017U);
    assert(registry.threadObjectReferenceCount(Initial) == GuestImportRegistry::MaximumThreadObjectReferences);
    assert(read32(space, Output) == 0xA5A5A5A5);
    GuestImportCall c;
    assert(call(registry, "__imp__ObDereferenceObject", c, {Initial}).handled());
    assert(invoke(registry, "__imp__ObReferenceObjectByHandle", {0xFFFFFFFEU, Type, Output}) == 0);
    registry.reset(); // Outstanding references belong to the discarded execution world.
    assert(registry.threadObjectReferenceCount(Initial) == 0);
}

void testCriticalRegions(GuestAddressSpace& space) {
    constexpr auto Enter = "__imp__KeEnterCriticalRegion", Leave = "__imp__KeLeaveCriticalRegion";
    GuestImportRegistry registry; GuestImportCall c;
    assert(!call(registry,Enter,c,{}).handled());
    assert(!registry.guestThreadCriticalRegionDepth(Initial));
    initialize(registry,space);
    assert(registry.guestThreadCriticalRegionDepth(Initial) == 0);
    assert(!call(registry,Leave,c,{}).handled());
    for (unsigned depth = 1; depth <= 2; ++depth) {
        assert(call(registry,Enter,c,{}).handled());
        assert(c.returnValue == 0x123456789ABCDEF0ULL);
        assert(registry.guestThreadCriticalRegionDepth(Initial) == depth);
    }
    assert(invoke(registry,"__imp__ExCreateThread",{Output,0,Output+4,0,Code,0,1}) == 0);
    const auto handle = read32(space,Output);
    const auto worker = registry.lookupSuspendedThread(handle); assert(worker);
    const auto token = worker->layout.threadAddress;
    assert(registry.bindGuestThreadToken(token));
    assert(!call(registry,Enter,c,{}).handled());
    assert(registry.guestThreadCriticalRegionDepth(token) == 0);
    assert(registry.bindGuestThreadToken(Initial));
    assert(registry.bindThreadResumeHandler([](const auto&,std::int32_t){return true;}));
    assert(invoke(registry,"__imp__NtResumeThread",{handle,Output}) == 0);
    assert(registry.bindGuestThreadToken(token));
    assert(call(registry,Enter,c,{}).handled());
    assert(registry.guestThreadCriticalRegionDepth(token) == 1);
    assert(registry.guestThreadCriticalRegionDepth(Initial) == 2);
    assert(call(registry,Leave,c,{}).handled());
    assert(registry.markGuestThreadReturned(token,0));
    assert(!call(registry,Enter,c,{}).handled());
    assert(registry.bindGuestThreadToken(Initial));
    write32(space,Initial+0x14C,99);
    assert(!call(registry,Leave,c,{}).handled());
    assert(!registry.guestThreadCriticalRegionDepth(Initial));
    write32(space,Initial+0x14C,1);
    assert(registry.guestThreadCriticalRegionDepth(Initial) == 2);
    assert(call(registry,Leave,c,{}).handled());
    assert(call(registry,Leave,c,{}).handled());
    assert(!call(registry,Leave,c,{}).handled());
    // Operational bound, not the claimed architectural width of Xbox KTHREAD.
    for (unsigned i = 0; i < 65535; ++i) assert(call(registry,Enter,c,{}).handled());
    assert(!call(registry,Enter,c,{}).handled());
    assert(registry.guestThreadCriticalRegionDepth(Initial) == 65535);
    for (unsigned i = 0; i < 65535; ++i) assert(call(registry,Leave,c,{}).handled());
    registry.unbindGuestThreadToken();
    assert(!call(registry,Enter,c,{}).handled());
    assert(registry.bindGuestThreadToken(Initial+4));
    assert(!call(registry,Enter,c,{}).handled());
    registry.reset(); assert(!registry.guestThreadCriticalRegionDepth(Initial));
    initialize(registry,space); assert(registry.guestThreadCriticalRegionDepth(Initial) == 0);
    registry.reset();
}

void testCalibrationLockBinding(GuestAddressSpace& space) {
    constexpr std::uint32_t Slot = 0x100100, Lock = 0x62004600;
    GuestImportRegistry registry; initialize(registry,space);
    GuestDataVariableBinding binding; GuestImportCall c;
    write32(space,Slot,0x1C1);
    space.translate(Lock-1)[0] = 0xA5; space.translate(Lock+28)[0] = 0x5A;
    assert(binding.initialize(space,{0x100000,0x10000},Slot,0x1C1,Lock,std::vector<std::uint8_t>(28),{}));
    assert(read32(space,Slot) == Lock);
    assert(invoke(registry,"__imp__RtlInitializeCriticalSectionAndSpinCount",{Lock,10000}) == 0);
    assert(space.translate(Lock)[0] == 1 && space.translate(Lock)[1] == 40);
    assert(read32(space,Lock+16) == 0xFFFFFFFF && read32(space,Lock+20) == 0 && read32(space,Lock+24) == 0);
    for (unsigned i = 1; i <= 2; ++i) {
        assert(call(registry,"__imp__RtlEnterCriticalSection",c,{read32(space,Slot)}).handled());
        assert(read32(space,Lock+20) == i && read32(space,Lock+24) == Initial);
    }
    assert(registry.bindGuestThreadToken(Initial+0x800));
    assert(!call(registry,"__imp__RtlLeaveCriticalSection",c,{Lock}).handled());
    // Contended waits remain guarded; rejection must preserve the original owner.
    assert(!call(registry,"__imp__RtlEnterCriticalSection",c,{Lock}).handled());
    assert(read32(space,Lock+20) == 2 && read32(space,Lock+24) == Initial);
    assert(registry.bindGuestThreadToken(Initial));
    assert(call(registry,"__imp__RtlLeaveCriticalSection",c,{Lock}).handled());
    assert(call(registry,"__imp__RtlLeaveCriticalSection",c,{Lock}).handled());
    assert(read32(space,Lock+16) == 0xFFFFFFFF && read32(space,Lock+24) == 0);
    binding.reset();
    assert(read32(space,Slot) == 0x1C1);
    for (unsigned i = 0; i < 28; ++i) assert(space.translate(Lock)[i] == 0);
    assert(space.translate(Lock-1)[0] == 0xA5 && space.translate(Lock+28)[0] == 0x5A);
    registry.reset();
}

void testBindingRejects(GuestAddressSpace& space) {
    GuestImportRegistry registry;
    assert(!registry.bindThreadObjectType(Type));
    assert(registry.initialize(space, Heap, 0x10000));
    write32(space, Type, 123);
    assert(!registry.bindThreadObjectType(Type));
    write32(space, Type, 0);
    assert(!registry.bindThreadObjectType(0));
    assert(!registry.bindThreadObjectType(Type + 1));
    assert(!registry.bindThreadObjectType(Heap));
    GuestImportCall c;
    assert(!call(registry, "__imp__ObReferenceObjectByHandle", c, {1, 0, Output}).handled());
    assert(registry.bindGuestThreadToken(Initial));
    write32(space, Initial + 0x14C, 99);
    assert(!registry.bindInitialThreadObject(Initial, 1));
    assert(!registry.bindInitialThreadObject(Initial + 4, 1));
    write32(space, Initial + 0x14C, 1);
    assert(registry.bindInitialThreadObject(Initial, 1));
    assert(!registry.bindThreadObjectType(Initial));
    registry.reset();
}
} // namespace

int main() {
    GuestAddressSpace space;
    assert(space.reserve());
    std::memset(space.translate(0x100010), 0x5A, 4);
    testReferencesPriorityAndClose(space);
    testReferenceLimit(space);
    testBindingRejects(space);
    testCriticalRegions(space);
    testCalibrationLockBinding(space);
}
