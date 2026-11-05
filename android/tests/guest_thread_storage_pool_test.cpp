#include "guest_thread_storage_pool.h"
#include "guest_import_registry.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <initializer_list>
#include <new>
#include <type_traits>
#include <vector>

using namespace recomp::bootstrap;

// Deterministically exercise diagnostic-allocation failure without exhausting
// the host. Only enabled for the single prepare() call under test.
static bool failNextAllocation = false;
void* operator new(std::size_t size) {
    if (failNextAllocation) {
        failNextAllocation = false;
        throw std::bad_alloc();
    }
    if (void* value = std::malloc(size == 0 ? 1 : size)) {
        return value;
    }
    throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* value) noexcept { ::operator delete(value); }
#if defined(__cpp_sized_deallocation)
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
#endif

static_assert(!std::is_copy_constructible_v<GuestThreadStoragePool>);
static_assert(!std::is_move_constructible_v<GuestThreadStoragePool>);

namespace {

constexpr std::array<std::uint8_t, 7> Template{0x01, 0xFE, 0x55, 0xAA, 0x42, 0x80, 0x09};

GuestThreadStorageConfig config() {
    return {{0x10000000U, 0x01000000U}, {0x00100000U, 0x10000U},
            {0x00101000U, 0x2000U}, 0x00100083U,
            static_cast<std::uint32_t>(Template.size()), 0x20U, 4U, 0x10000U, 2U};
}

GuestThreadStorageRequest request() {
    return {0, 0x00101000U, 0x00101004U, 0xDEADBEEFU, 1};
}

std::uint32_t read32(const GuestAddressSpace& space, std::uint32_t address) {
    const auto* p = space.translate(address);
    return (std::uint32_t{p[0]} << 24U) | (std::uint32_t{p[1]} << 16U)
            | (std::uint32_t{p[2]} << 8U) | p[3];
}

void filled(const GuestAddressSpace& space, GuestThreadMemoryRange range, std::uint8_t byte) {
    const auto* first = space.translate(range.base);
    assert(std::all_of(first, first + range.size, [byte](auto actual) { return actual == byte; }));
}

void testWrappedCallbackContext(GuestAddressSpace& space) {
    GuestThreadStoragePool pool;const auto c=config();assert(pool.initialize(space,c));
    auto invalid=request();invalid.wrapStartContext=true;
    assert(pool.prepare(invalid).state==GuestThreadStorageState::InvalidRequest && pool.size()==0);
    for(auto context:{0U,1U,0xDEADBEEFU,UINT32_MAX}) {
        auto direct=request();direct.xapiStartup=0;direct.wrapStartContext=true;direct.startContext=context;
        const auto result=pool.prepare(direct);assert(result.succeeded());const auto t=*result.thread;
        assert(t.wrappedContextAddress && t.startContext==t.wrappedContextAddress);
        assert(t.wrappedContextAddress==t.layout.tlsAddress+GuestThreadStoragePool::PageSize);
        assert(t.layout.stackBase==t.wrappedContextAddress+GuestThreadStoragePool::PageSize);
        assert(t.allocation.size==4*GuestThreadStoragePool::PageSize+c.defaultStackSize);
        assert(read32(space,t.wrappedContextAddress)==context);
        filled(space,{t.wrappedContextAddress+4,GuestThreadStoragePool::PageSize-4},0);
        filled(space,{t.layout.stackBase,t.layout.stackSize},0);
        assert(std::memcmp(space.translate(t.layout.tlsAddress),Template.data(),Template.size())==0);
        assert(read32(space,t.layout.pcrAddress)==t.layout.tlsAddress);
        assert(read32(space,t.layout.threadAddress+0x5C)==t.layout.stackBase+t.layout.stackSize);
        // The caller's stack may be completely overwritten without touching
        // the wrapped argument or static/dynamic TLS. Release scrubs all of it.
        std::memset(space.translate(t.layout.stackBase),0xA7,t.layout.stackSize);
        assert(read32(space,t.wrappedContextAddress)==context);
        assert(pool.release(t.layout.threadId));filled(space,t.allocation,0);
    }
    auto normal=request();const auto regular=pool.prepare(normal);assert(regular.succeeded());
    assert(!regular.thread->wrappedContextAddress && regular.thread->startContext==normal.startContext);
    assert(pool.release(regular.thread->layout.threadId));pool.reset();
    auto tight=c;tight.defaultStackSize=0x4000;tight.arena.size=7*GuestThreadStoragePool::PageSize;
    assert(pool.initialize(space,tight));
    auto direct=request();direct.xapiStartup=0;direct.wrapStartContext=true;
    assert(pool.prepare(direct).state==GuestThreadStorageState::ArenaExhausted);
    direct.wrapStartContext=false;assert(pool.prepare(direct).succeeded());
}

void testStorageAndIsolation(GuestAddressSpace& space) {
    GuestThreadStoragePool pool;
    const auto c = config();
    std::memset(space.translate(c.arena.base - 1), 0xA5, c.arena.size + 2U);
    assert(pool.initialize(space, c, {{0x20000000U, 0x10000U}}));
    filled(space, c.arena, 0xA5); // Binding alone writes nothing.
    const auto a = pool.prepare(request());
    assert(a.succeeded() && pool.size() == 1);
    const auto& t = *a.thread;
    assert(t.layout.threadId == 2 && t.allocation.base == c.arena.base);
    assert(t.layout.stackSize == c.defaultStackSize);
    assert(t.layout.stackBase % 0x1000U == 0);
    assert(t.layout.stackBase + t.layout.stackSize == t.allocation.base + t.allocation.size);
    assert(t.dynamicTlsAddress == t.layout.tlsAddress + c.tlsDataSize);
    assert(t.xapiStartup == request().xapiStartup && t.startRoutine == request().startRoutine);
    assert(t.startContext == 0xDEADBEEFU && t.creationFlags == 1);
    assert(read32(space, t.layout.pcrAddress) == t.layout.tlsAddress);
    assert(read32(space, t.layout.pcrAddress + 0x100U) == t.layout.threadAddress);
    assert(read32(space, t.layout.threadAddress + 0x14CU) == 2);
    assert(read32(space, t.layout.threadAddress + 0x5CU)
            == t.layout.stackBase + t.layout.stackSize);
    assert(std::memcmp(space.translate(t.layout.tlsAddress), Template.data(), Template.size()) == 0);
    filled(space, {t.layout.tlsAddress + static_cast<std::uint32_t>(Template.size()),
            c.tlsDataSize + c.tlsSlotCount * 4U - static_cast<std::uint32_t>(Template.size())}, 0);
    filled(space, {t.layout.stackBase, t.layout.stackSize}, 0);
    filled(space, {t.layout.pcrAddress + GuestThreadEnvironment::PcrSize,
            GuestThreadStoragePool::PageSize - GuestThreadEnvironment::PcrSize}, 0);
    assert(!pool.initialize(space, c));
    assert(pool.lookup(2)->allocation.base == t.allocation.base);

    auto r = request();
    r.stackSize = 0x4001U;
    r.xapiStartup = 0;
    const auto b = pool.prepare(r);
    assert(b.succeeded() && b.thread->layout.threadId == 3);
    assert(b.thread->layout.stackSize == 0x5000U && b.thread->xapiStartup == 0);
    assert(b.thread->allocation.base == t.allocation.base + t.allocation.size);
    *space.translate(t.dynamicTlsAddress) = 0x7A;
    *space.translate(t.layout.tlsAddress) = 0x7B;
    *space.translate(t.layout.stackBase) = 0x7C;
    assert(read32(space, b.thread->dynamicTlsAddress) == 0);
    assert(*space.translate(b.thread->layout.tlsAddress) == Template[0]);
    assert(std::memcmp(space.translate(c.tlsRawDataAddress), Template.data(), Template.size()) == 0);
    assert(!pool.release(0) && !pool.release(99));
    assert(pool.release(2) && !pool.release(2) && !pool.lookup(2));
    filled(space, t.allocation, 0);
    assert(pool.lookup(3));
    const auto replacement = pool.prepare(request());
    assert(replacement.succeeded() && replacement.thread->layout.threadId == 4);
    assert(replacement.thread->allocation.base == t.allocation.base); // Reuse bytes, never IDs.
    pool.reset();
    filled(space, t.allocation, 0);
    filled(space, b.thread->allocation, 0);
    assert(!pool.ready() && pool.size() == 0 && !pool.lookup(3));
    assert(*space.translate(c.arena.base - 1) == 0xA5);
    assert(*space.translate(c.arena.base + c.arena.size) == 0xA5);
    assert(pool.initialize(space, c));
    assert(pool.prepare(request()).thread->layout.threadId == 5);
}

void testRejectedConfig(GuestAddressSpace& space) {
    const auto c = config();
    std::vector<GuestThreadStorageConfig> invalid;
    const auto add = [&](auto alter) { auto value = c; alter(value); invalid.push_back(value); };
    add([](auto& v) { v.arena.base = 0; });
    add([](auto& v) { ++v.arena.base; });
    add([](auto& v) { --v.arena.size; });
    add([](auto& v) { v.arena.size = 0; });
    add([](auto& v) { v.arena.size = GuestThreadStoragePool::MaximumArenaSize + 0x1000U; });
    add([](auto& v) { v.arena.size = 0x6000U; });
    add([](auto& v) { v.arena = {0xFFFF0000U, 0x10000U}; });
    add([](auto& v) { v.arena = {v.image.base, v.image.size}; });
    add([](auto& v) { v.image.size = 0; });
    add([](auto& v) { v.image.base = 0xFFFFF000U; });
    add([](auto& v) { v.code.base = v.image.base - 4; });
    add([](auto& v) { v.code.size = v.image.size; });
    add([](auto& v) { ++v.code.base; });
    add([](auto& v) { v.code.size = 3; });
    add([](auto& v) { v.defaultStackSize = 0; });
    add([](auto& v) { v.defaultStackSize = 0xFFFFFFFFU; });
    add([](auto& v) { v.firstThreadId = 1; });
    add([](auto& v) { ++v.tlsDataSize; });
    add([](auto& v) { v.tlsDataSize = GuestThreadEnvironment::MaxStaticTlsSize + 4; });
    add([](auto& v) { v.tlsSlotCount = GuestThreadEnvironment::MaxTlsSlots + 1; });
    add([](auto& v) { v.tlsRawDataSize = v.tlsDataSize + 1; });
    add([](auto& v) { v.tlsRawDataAddress = 0; });
    add([](auto& v) { v.tlsRawDataAddress = v.image.base + v.image.size - 1; });
    add([](auto& v) { v.tlsRawDataSize = v.tlsDataSize = v.tlsSlotCount = 0; });
    std::memset(space.translate(c.arena.base), 0xA6, 0x20000U);
    for (const auto& v : invalid) {
        GuestThreadStoragePool pool;
        assert(!pool.initialize(space, v) && !pool.ready());
        assert(!pool.prepare(request()).succeeded());
        filled(space, {c.arena.base, 0x20000U}, 0xA6);
    }
    for (const auto range : std::array<GuestThreadMemoryRange, 5>{{
            c.arena, {c.arena.base - 0x1000U, 0x1001U},
            {c.arena.base + c.arena.size - 1, 0x1000U}, {0, 0}, {0xFFFFFFFFU, 4}}}) {
        GuestThreadStoragePool pool;
        assert(!pool.initialize(space, c, {range}));
    }
    GuestThreadStoragePool pool;
    assert(!pool.initialize(space, c, std::vector<GuestThreadMemoryRange>(65, {0x20000000U, 4})));
    GuestAddressSpace unavailable;
    assert(!pool.initialize(unavailable, c));
}

void testRequestsAndFailure(GuestAddressSpace& space) {
    auto c = config();
    c.arena.size = 0x7000U; // Exactly one minimum stack + three metadata pages.
    c.defaultStackSize = 1;
    GuestThreadStoragePool pool;
    assert(pool.initialize(space, c));
    std::vector<GuestThreadStorageRequest> invalid;
    for (auto flags : {2U, 0x81U, 0x03000001U, 0x40000000U, 0x80000001U, 0xFFFFFFFFU}) {
        auto r = request(); r.creationFlags = flags; invalid.push_back(r);
    }
    for (auto address : {0U, c.code.base - 4, c.code.base + c.code.size, c.code.base + 1}) {
        auto r = request(); r.startRoutine = address; invalid.push_back(r);
    }
    for (auto address : {c.code.base - 4, c.code.base + c.code.size, c.code.base + 1}) {
        auto r = request(); r.xapiStartup = address; invalid.push_back(r);
    }
    auto huge = request(); huge.stackSize = 0xFFFFFFFFU; invalid.push_back(huge);
    huge.stackSize = GuestThreadStoragePool::MaximumStackSize + 1; invalid.push_back(huge);
    std::memset(space.translate(c.arena.base), 0xA7, c.arena.size);
    for (const auto& r : invalid) {
        assert(pool.prepare(r).state == GuestThreadStorageState::InvalidRequest);
        assert(pool.size() == 0);
        filled(space, c.arena, 0xA7);
    }
    auto tooLarge = request(); tooLarge.stackSize = 0x5000U;
    assert(pool.prepare(tooLarge).state == GuestThreadStorageState::ArenaExhausted);
    filled(space, c.arena, 0xA7);

    failNextAllocation = true;
    const auto failure = pool.prepare(request());
    assert(!failNextAllocation && failure.state == GuestThreadStorageState::InitializationFailed);
    assert(!failure.thread && pool.size() == 0);
    const auto first = pool.prepare(request());
    assert(first.succeeded() && first.thread->layout.threadId == 2); // Failure consumed no ID.
    assert(first.thread->layout.stackSize == 0x4000U);
    *space.translate(first.thread->layout.stackBase) = 0xC3;
    assert(pool.prepare(request()).state == GuestThreadStorageState::ArenaExhausted);
    assert(*space.translate(first.thread->layout.stackBase) == 0xC3);
    assert(pool.release(2));
    filled(space, c.arena, 0);
    auto runnableRequest = request(); runnableRequest.creationFlags = 0;
    const auto runnable = pool.prepare(runnableRequest);
    assert(runnable.succeeded() && runnable.thread->creationFlags == 0);
    assert(pool.release(runnable.thread->layout.threadId));
}

void testCapacityAndIdExhaustion(GuestAddressSpace& space) {
    auto c = config();
    c.defaultStackSize = 0x4000U;
    GuestThreadStoragePool pool;
    assert(pool.initialize(space, c));
    for (std::uint32_t i = 0; i < GuestThreadStoragePool::MaximumThreads; ++i) {
        auto result = pool.prepare(request());
        assert(result.succeeded() && result.thread->layout.threadId == i + 2);
    }
    assert(pool.prepare(request()).state == GuestThreadStorageState::CapacityExceeded);
    assert(pool.release(3));
    assert(pool.prepare(request()).thread->layout.threadId == 66);
    pool.reset();
    c.firstThreadId = std::numeric_limits<std::uint32_t>::max();
    assert(pool.initialize(space, c));
    const auto last = pool.prepare(request());
    assert(last.succeeded() && last.thread->layout.threadId == c.firstThreadId);
    assert(pool.release(c.firstThreadId));
    assert(pool.prepare(request()).state == GuestThreadStorageState::CapacityExceeded);
    pool.reset();
    c.firstThreadId = 2;
    assert(pool.initialize(space, c));
    assert(pool.prepare(request()).state == GuestThreadStorageState::CapacityExceeded);
}

void testTlsAndDestructor(GuestAddressSpace& space) {
    auto c = config();
    c.tlsRawDataSize = c.tlsDataSize = 0;
    c.tlsSlotCount = 1;
    GuestThreadMemoryRange owned{};
    {
        GuestThreadStoragePool pool;
        assert(pool.initialize(space, c));
        auto result = pool.prepare(request());
        assert(result.succeeded());
        assert(result.thread->dynamicTlsAddress == result.thread->layout.tlsAddress);
        assert(read32(space, result.thread->dynamicTlsAddress) == 0);
        owned = result.thread->allocation;
        std::memset(space.translate(owned.base), 0xFF, owned.size);
    }
    filled(space, owned, 0);
    c = config();
    c.tlsSlotCount = 0;
    GuestThreadStoragePool pool;
    assert(pool.initialize(space, c));
    assert(pool.prepare(request()).thread->dynamicTlsAddress == 0);
}

void testFragmentReuseAndBoundaries(GuestAddressSpace& space) {
    auto c = config();
    c.arena.size = 3U * 0x7000U;
    c.defaultStackSize = 1;
    GuestThreadStoragePool pool;
    assert(pool.initialize(space, c));
    auto lastInstruction = request();
    lastInstruction.startRoutine = c.code.base + c.code.size - 4U;
    const auto a = pool.prepare(lastInstruction);
    const auto b = pool.prepare(request());
    const auto d = pool.prepare(request());
    assert(a.succeeded() && b.succeeded() && d.succeeded());
    *space.translate(d.thread->layout.stackBase) = 0xC9;
    assert(pool.release(a.thread->layout.threadId));
    assert(pool.release(b.thread->layout.threadId));
    auto larger = request();
    larger.stackSize = 0xB000U; // Two adjacent freed blocks become one allocation.
    const auto merged = pool.prepare(larger);
    assert(merged.succeeded() && merged.thread->allocation.base == c.arena.base);
    assert(merged.thread->allocation.size == 0xE000U);
    assert(*space.translate(d.thread->layout.stackBase) == 0xC9);
    pool.reset();

    c = config();
    c.arena.size = GuestThreadStoragePool::MaximumStackSize + 0x200000U;
    c.tlsDataSize = GuestThreadEnvironment::MaxStaticTlsSize;
    c.tlsSlotCount = GuestThreadEnvironment::MaxTlsSlots;
    assert(pool.initialize(space, c));
    auto maximum = request();
    maximum.stackSize = GuestThreadStoragePool::MaximumStackSize;
    const auto result = pool.prepare(maximum);
    assert(result.succeeded());
    assert(result.thread->layout.stackSize == GuestThreadStoragePool::MaximumStackSize);
    assert(result.thread->dynamicTlsAddress == result.thread->layout.tlsAddress + c.tlsDataSize);
    filled(space, {result.thread->dynamicTlsAddress, c.tlsSlotCount * 4U}, 0);
}

std::uint32_t invoke(GuestImportRegistry& registry, std::string_view name,
        std::initializer_list<std::uint32_t> arguments = {}) {
    GuestImportCall call{};
    std::copy(arguments.begin(), arguments.end(), call.arguments.begin());
    assert(registry.dispatch(name, call).handled());
    return static_cast<std::uint32_t>(call.returnValue);
}

void testRegistryTlsLifecycle(GuestAddressSpace& space) {
    const auto c = config();
    GuestImportRegistry registry;
    GuestThreadStoragePool pool;
    assert(registry.initialize(space, 0x30000000U, 0x10000U));
    assert(pool.initialize(space, c, {{0x30000000U, 0x10000U}, {0x02000000U, 0x2000U}}));
    constexpr std::uint32_t InitialToken = 0x02000000U;
    assert(registry.bindGuestThreadToken(InitialToken));
    assert(registry.bindGuestThreadTlsStorage(InitialToken, 0x02001000U, c.tlsSlotCount));
    const auto slot = invoke(registry, "__imp__KeTlsAlloc");
    assert(slot == 0);
    assert(invoke(registry, "__imp__KeTlsSetValue", {slot, 0x11U}) == 1);
    const auto a = pool.prepare(request());
    const auto b = pool.prepare(request());
    assert(a.succeeded() && b.succeeded());
    const auto aToken = a.thread->layout.threadAddress;
    const auto bToken = b.thread->layout.threadAddress;
    // Explicit synthetic rebinding exercises the existing registry contract;
    // it is not a guest context switch or a scheduler implementation.
    assert(registry.bindGuestThreadToken(aToken));
    assert(registry.bindGuestThreadTlsStorage(aToken, a.thread->dynamicTlsAddress, c.tlsSlotCount));
    assert(invoke(registry, "__imp__KeTlsGetValue", {slot}) == 0);
    assert(invoke(registry, "__imp__KeTlsSetValue", {slot, 0x22U}) == 1);
    assert(read32(space, a.thread->dynamicTlsAddress) == 0x22U);
    assert(registry.bindGuestThreadToken(bToken));
    assert(registry.bindGuestThreadTlsStorage(bToken, b.thread->dynamicTlsAddress, c.tlsSlotCount));
    assert(invoke(registry, "__imp__KeTlsGetValue", {slot}) == 0);
    assert(invoke(registry, "__imp__KeTlsSetValue", {slot, 0x33U}) == 1);
    assert(registry.bindGuestThreadToken(InitialToken));
    assert(invoke(registry, "__imp__KeTlsGetValue", {slot}) == 0x11U);
    assert(registry.bindGuestThreadToken(aToken));
    assert(invoke(registry, "__imp__KeTlsGetValue", {slot}) == 0x22U);
    assert(std::memcmp(space.translate(a.thread->layout.tlsAddress), Template.data(), Template.size()) == 0);
    const auto lock = a.thread->layout.stackBase;
    (void)invoke(registry, "__imp__RtlInitializeCriticalSection", {lock});
    (void)invoke(registry, "__imp__RtlEnterCriticalSection", {lock});
    assert(!registry.teardownGuestThread(aToken)); // Do not release storage yet.
    assert(pool.lookup(a.thread->layout.threadId));
    (void)invoke(registry, "__imp__RtlLeaveCriticalSection", {lock});
    assert(registry.teardownGuestThread(aToken));
    assert(pool.release(a.thread->layout.threadId));
    filled(space, a.thread->allocation, 0);
    assert(registry.bindGuestThreadToken(bToken));
    assert(invoke(registry, "__imp__KeTlsGetValue", {slot}) == 0x33U);
    assert(invoke(registry, "__imp__KeTlsFree", {slot}) == 1);
    assert(read32(space, b.thread->dynamicTlsAddress) == 0);
    assert(read32(space, 0x02001000U) == 0);
    assert(registry.teardownGuestThread(bToken));
    assert(pool.release(b.thread->layout.threadId));
    assert(registry.teardownGuestThread(InitialToken));
    registry.reset();
    pool.reset();
}

} // namespace

int main() {
    GuestAddressSpace space;
    assert(space.reserve());
    const auto c = config();
    std::memcpy(space.translate(c.tlsRawDataAddress), Template.data(), Template.size());
    testStorageAndIsolation(space);
    testWrappedCallbackContext(space);
    testRejectedConfig(space);
    testRequestsAndFailure(space);
    testCapacityAndIdExhaustion(space);
    testTlsAndDestructor(space);
    testFragmentReuseAndBoundaries(space);
    testRegistryTlsLifecycle(space);
    assert(std::memcmp(space.translate(c.tlsRawDataAddress), Template.data(), Template.size()) == 0);
}
