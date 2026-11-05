#include "guest_import_registry.h"

#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <algorithm>
#include <array>
#include <cstring>

using namespace recomp::bootstrap;

namespace {
GuestImportCall args(std::initializer_list<std::uint32_t> values) {
    GuestImportCall call;
    std::size_t i = 0;
    for (auto value : values) { call.arguments[i++] = value; }
    call.returnValue = 0xDEADBEEF;
    return call;
}
void put(GuestAddressSpace& space, std::uint32_t at, std::uint32_t value) {
    auto* p = space.translate(at);
    for (unsigned i = 0; i < 4; ++i) { p[i] = value >> (24 - 8 * i); }
}
std::uint32_t get(GuestAddressSpace& space, std::uint32_t at) {
    const auto* p = space.translate(at);
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16)
            | (std::uint32_t{p[2]} << 8) | p[3];
}

void testMemoryStatistics() {
    GuestAddressSpace space;
    assert(space.reserve());
    GuestImportRegistry registry;
    constexpr std::uint32_t Heap = 0x10000000, HeapSize = 0x90000;
    constexpr std::uint32_t Physical = 0xA0000000, PhysicalSize = 0x20000, Offset = 0x100000;
    constexpr std::uint32_t Out = 0x4000;
    assert(registry.initialize(space, Heap, HeapSize));
    assert(!registry.lastMemoryStatistics());
    auto noArena = args({Out});
    put(space, Out, 104);
    assert(!registry.dispatch("__imp__MmQueryStatistics", noArena).handled());
    assert(!registry.lastMemoryStatistics());
    assert(registry.bindPhysicalMemoryArena(Physical, PhysicalSize, Offset));

    const auto statistics = [&](std::uint32_t reserved, std::uint32_t committed,
                                std::uint32_t physicalPages, std::uint32_t out = Out) {
        std::fill_n(space.translate(out - 4), 112, 0xA5);
        put(space, out, 104);
        auto call = args({out});
        call.arguments[0] |= 0xFFFFFFFF00000000ULL;
        const auto inputs = call.arguments;
        const auto result = registry.dispatch("__imp__MmQueryStatistics", call);
        assert(result.handled() && call.returnValue == 0 && call.arguments == inputs);
        std::array<std::uint32_t, 26> expected{};
        expected[0] = 104;
        expected[1] = PhysicalSize / 4096;
        expected[3] = expected[1] - physicalPages;
        expected[4] = HeapSize;
        expected[5] = reserved;
        expected[6] = physicalPages;
        expected[11] = committed;
        expected[25] = (Offset + PhysicalSize) / 4096 - 1;
        for (std::size_t i = 0; i < expected.size(); ++i)
            assert(get(space, out + 4 * i) == expected[i]);
        assert(get(space, out - 4) == 0xA5A5A5A5 && get(space, out + 104) == 0xA5A5A5A5);
        assert(registry.lastMemoryStatistics() == expected);
    };
    const auto rejectedOutput = [&](std::uint32_t out, bool handled = false,
                                    std::uint32_t status = 0xDEADBEEF, std::uint32_t size = 104) {
        std::array<std::uint8_t, 104> before{};
        std::fill_n(space.translate(out), before.size(), 0xA7);
        put(space, out, size);
        std::memcpy(before.data(), space.translate(out), before.size());
        const auto last = registry.lastMemoryStatistics();
        auto call = args({out});
        assert(registry.dispatch("__imp__MmQueryStatistics", call).handled() == handled);
        assert(call.returnValue == status);
        assert(std::memcmp(before.data(), space.translate(out), before.size()) == 0);
        assert(registry.lastMemoryStatistics() == last);
    };
    statistics(0, 0, 0);
    for (const auto size : {0U, 100U, 103U, 105U, 108U, UINT32_MAX})
        rejectedOutput(Out, true, 0xC0000023, size);
    rejectedOutput(Out + 1);
    rejectedOutput(0x7FC80000);
    rejectedOutput(0x7FC80000 - 100);
    rejectedOutput(Physical); // Arena capacity is not allocated writable memory.
    rejectedOutput(Physical - 100);
    rejectedOutput(Physical + PhysicalSize - 100);
    rejectedOutput(Heap);
    rejectedOutput(Heap - 100);
    auto null = args({0});
    assert(registry.dispatch("__imp__MmQueryStatistics", null).handled());
    assert(null.returnValue == 0xC000000D);
    auto wrap = args({0xFFFFFFA0});
    assert(!registry.dispatch("__imp__MmQueryStatistics", wrap).handled());
    assert(wrap.returnValue == 0xDEADBEEF);
    assert(registry.bindGuestThreadToken(1));
    assert(registry.bindGuestThreadTlsStorage(1, 0x100000, 64));
    rejectedOutput(0x100000);
    rejectedOutput(0x100000 - 100);

    // Protection, fragmentation and release change physical counts, not virtual counts.
    auto ro = args({0, 1, 2, 0, UINT32_MAX, 0});
    assert(registry.dispatch("__imp__MmAllocatePhysicalMemoryEx", ro).handled());
    assert(ro.returnValue);
    statistics(0, 0, 1);
    rejectedOutput(static_cast<std::uint32_t>(ro.returnValue));
    auto rw = args({0, 4097, 4, 0, UINT32_MAX, 0});
    assert(registry.dispatch("__imp__MmAllocatePhysicalMemoryEx", rw).handled());
    assert(rw.returnValue);
    const auto writable = static_cast<std::uint32_t>(rw.returnValue);
    statistics(0, 0, 3, writable + 4);
    rejectedOutput(writable + 8192 - 100); // Crosses into adjacent read-only allocation.
    rejectedOutput(writable - 100); // Crosses from unowned into writable allocation.
    auto freeRo = args({0, static_cast<std::uint32_t>(ro.returnValue)});
    assert(registry.dispatch("__imp__MmFreePhysicalMemory", freeRo).handled());
    statistics(0, 0, 2);

    const auto allocateVirtual = [&](std::uint32_t address, std::uint32_t size, std::uint32_t type) {
        put(space, 0x2000, address); put(space, 0x2004, size);
        auto call = args({0x2000, 0x2004, type, 4, 0});
        assert(registry.dispatch("__imp__NtAllocateVirtualMemory", call).handled());
        return static_cast<std::uint32_t>(call.returnValue);
    };
    // A partial commit spans the 64-bit bitmap word boundary. Recommit and
    // overlapping commits must neither double-count nor erase existing data.
    assert(allocateVirtual(0, 67 * 4096, 0x2000) == 0);
    assert(get(space, 0x2000) == Heap);
    statistics(67 * 4096, 0, 2);
    rejectedOutput(Heap + 4096);
    std::fill_n(space.translate(Heap + 62 * 4096), 5 * 4096, 0xA3);
    assert(allocateVirtual(Heap + 62 * 4096, 3 * 4096, 0x1000) == 0);
    statistics(67 * 4096, 3, 2);
    for (std::uint32_t i = 0; i < 3 * 4096; ++i) assert(space.translate(Heap + 62 * 4096)[i] == 0);
    put(space, Heap + 63 * 4096, 0x12345678);
    assert(allocateVirtual(Heap + 62 * 4096, 3 * 4096, 0x1000) == 0);
    assert(get(space, Heap + 63 * 4096) == 0x12345678);
    statistics(67 * 4096, 3, 2);
    assert(allocateVirtual(Heap + 63 * 4096, 4 * 4096, 0x1000) == 0);
    assert(get(space, Heap + 63 * 4096) == 0x12345678);
    for (std::uint32_t i = 0; i < 2 * 4096; ++i) assert(space.translate(Heap + 65 * 4096)[i] == 0);
    statistics(67 * 4096, 5, 2, Heap + 64 * 4096 - 52); // Two committed pages.
    rejectedOutput(Heap + 62 * 4096 - 52); // Uncommitted/committed boundary.
    assert(allocateVirtual(Heap + 66 * 4096, 2 * 4096, 0x1000) == 0xC000000D);
    assert(allocateVirtual(Heap + 62 * 4096 + 1, 4096, 0x1000) == 0xC000000D);
    assert(allocateVirtual(0, HeapSize, 0x3000) == 0xC0000017);
    statistics(67 * 4096, 5, 2);
    put(space, 0x2000, Heap); put(space, 0x2004, 0);
    auto release = args({0x2000, 0x2004, 0x8000, 0});
    assert(registry.dispatch("__imp__NtFreeVirtualMemory", release).handled());
    assert(release.returnValue == 0);
    statistics(0, 0, 2);
    // Large-page requests are still accounted as 4-KiB statistics pages.
    assert(allocateVirtual(0, 1, 0x20003000) == 0);
    statistics(65536, 16, 2, Heap + 4);
    registry.unbindPhysicalMemoryArena();
    rejectedOutput(Out);
    registry.reset();
    assert(!registry.lastMemoryStatistics());
    assert(registry.initialize(space, Heap, HeapSize));
    assert(registry.bindPhysicalMemoryArena(Physical, PhysicalSize, Offset));
    statistics(0, 0, 0);
}

void testImagePageProtections() {
    constexpr std::uint32_t Base = 0x82000000, Bytes = 7 * 65536, Header = 0x60000000;
    constexpr std::uint32_t Security = 0x80, Descriptors = Security + 0x184;
    std::array<std::uint8_t, 0x400> header{};
    const auto word = [&](std::uint32_t at, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) header[at + i] = value >> (24 - 8 * i);
    };
    word(0, 0x58455832); word(8, header.size()); word(16, Security); word(20, 1);
    word(Security, 0x184 + 4 * 24); word(Security + 4, Bytes);
    word(Security + 0x110, Base); word(Security + 0x180, 4);
    word(Descriptors, 0x13); word(Descriptors + 24, 0x21);
    word(Descriptors + 48, 0x32); word(Descriptors + 72, 0x13);
    const auto original = header;
    GuestImageProtections pages;
    assert(!pages.ready() && !pages.query(Base));
    assert(pages.initialize(header.data(), header.size(), Base, Bytes));
    const auto assertMap = [&] {
        for (std::uint32_t p = 0; p < 7; ++p) {
            const auto expected = p >= 3 && p < 6 ? 4U : 2U;
            for (const auto offset : {0U, 1U, 32768U, 65535U})
                assert(pages.query(Base + p * 65536 + offset) == expected);
        }
        assert(!pages.query(Base - 1) && !pages.query(Base + Bytes) && !pages.query(0));
    };
    assertMap();
    assert(pages.writable(Base + 3 * 65536, 3 * 65536));
    assert(!pages.writable(Base + 3 * 65536 - 4, 8));
    assert(!pages.writable(Base + 6 * 65536 - 4, 8));
    assert(!pages.writable(Base + Bytes - 4, 8));
    assert(!pages.writable(Base + 3 * 65536, 0));
    // Complete-table validation and transactional replacement, never partial binding.
    for (const auto& mutation : std::initializer_list<std::pair<std::uint32_t, std::uint32_t>>{
            {0, 0}, {8, 0x3FF}, {16, 0}, {16, 0x81}, {16, 0xFFFFFFFC},
            {20, 4097}, {20, 16}, {Security, 0x184}, {Security, 0xFFFFFFFC},
            {Security + 4, Bytes + 65536}, {Security + 0x110, Base + 65536},
            {Security + 0x180, 0}, {Security + 0x180, 4097},
            {Descriptors, 0x10}, {Descriptors, 0x14}, {Descriptors, 3},
            {Descriptors + 72, 0xFFFFFFF3}, {Descriptors + 72, 0x23}, {Descriptors + 48, 0x22}}) {
        header = original;
        word(mutation.first, mutation.second);
        assert(!pages.initialize(header.data(), header.size(), Base, Bytes));
        assertMap();
    }
    header = original;
    for (const auto size : {0U, 23U, 0x1000001U})
        assert(!pages.initialize(header.data(), size, Base, Bytes));
    assert(!pages.initialize(nullptr, header.size(), Base, Bytes));
    for (const auto base : {0U, Base + 1, 0x90000000U, 0xFFFF0000U})
        assert(!pages.initialize(header.data(), header.size(), base, Bytes));
    // Every truncated prefix with a matching declared header length must fail.
    for (std::uint32_t size = 24; size < Security + 0x184 + 4 * 24; ++size) {
        word(8, size);
        assert(!pages.initialize(header.data(), size, Base, Bytes));
    }
    header = original;
    GuestAddressSpace space;
    assert(space.reserve());
    std::memcpy(space.translate(Header), header.data(), header.size());
    GuestImportRegistry registry;
    assert(!registry.copySerializedTitleImage(Base,4));
    assert(!registry.bindImagePageProtections(Header, header.size(), Base, Bytes));
    assert(registry.initialize(space, 0x10000000, 0x10000));
    assert(registry.bindPhysicalMemoryArena(Base, Bytes, 0));
    assert(!registry.bindImagePageProtections(Header, header.size(), Base, Bytes));
    registry.unbindPhysicalMemoryArena();
    assert(!registry.bindImagePageProtections(Header + 1, header.size(), Base, Bytes));
    assert(!registry.bindImagePageProtections(0x7FC80000, header.size(), Base, Bytes));
    assert(registry.bindImagePageProtections(Header, header.size(), Base, Bytes));
    assert(!registry.bindImagePageProtections(Header, header.size(), Base, Bytes));
    // Current image bytes, independent returned copy, complete owned extent.
    put(space,Base+3*65536,0x12345678);
    auto liveImage=registry.copySerializedTitleImage(Base+3*65536,4);
    assert(liveImage && *liveImage==std::vector<std::uint8_t>({0x12,0x34,0x56,0x78}));
    put(space,Base+3*65536,0x89ABCDEF);
    assert((*liveImage)[0]==0x12 && registry.copySerializedTitleImage(Base+3*65536,4)->at(0)==0x89);
    for(auto at:{0u,Base-4,Base+1,Base+Bytes,0xFFFFFFFCu})assert(!registry.copySerializedTitleImage(at,4));
    for(auto n:{0u,65537u,UINT32_MAX})assert(!registry.copySerializedTitleImage(Base,n));
    assert(!registry.copySerializedTitleImage(Base+Bytes-4,8));
    assert(registry.copySerializedTitleImage(Base+Bytes-4,4));
    std::puts("SERIALIZED_TITLE_IMAGE_READER_PASS live_bytes=1 independent_copy=1 validated_extent=1 missing_rejected=1 guest_writes=0");
    assert(!registry.bindPhysicalMemoryArena(Base, Bytes, 0));
    for (std::uint32_t p = 0; p < 7; ++p) {
        auto query = args({Base + p * 65536 + 100});
        assert(registry.dispatch("__imp__MmQueryAddressProtect", query).handled());
        assert(query.returnValue == (p >= 3 && p < 6 ? 4U : 2U));
    }
    // Guest mutation of the raw header cannot change the host's snapshot.
    std::fill_n(space.translate(Header), header.size(), 0);
    auto query = args({Base + 3 * 65536});
    assert(registry.dispatch("__imp__MmQueryAddressProtect", query).handled() && query.returnValue == 4);
    assert(registry.bindPhysicalMemoryArena(0xA0000000, 0x10000, 0));
    for (const auto out : {Base, Base + 3 * 65536 - 100, Base + 6 * 65536 - 100, Base + Bytes - 100}) {
        put(space, out, 104);
        auto stats = args({out});
        assert(!registry.dispatch("__imp__MmQueryStatistics", stats).handled());
        assert(get(space, out) == 104 && stats.returnValue == 0xDEADBEEF);
    }
    put(space, Base + 3 * 65536, 104);
    auto stats = args({Base + 3 * 65536});
    assert(registry.dispatch("__imp__MmQueryStatistics", stats).handled() && stats.returnValue == 0);
    assert(get(space, Base + 3 * 65536 + 4) == 16);
    registry.reset();
    assert(registry.initialize(space, 0x10000000, 0x10000));
    query = args({Base});
    assert(!registry.dispatch("__imp__MmQueryAddressProtect", query).handled());
}
}

int main() {
    testMemoryStatistics();
    testImagePageProtections();
    GuestAddressSpace space;
    assert(space.reserve());
    GuestImportRegistry registry;
    constexpr std::uint32_t Heap = 0x10000000, HeapSize = 0x20000;
    constexpr std::uint32_t Physical = 0xA0000000, PhysicalSize = 0x2000000;
    assert(registry.initialize(space, Heap, HeapSize));
    auto query = [&](std::uint32_t at, std::uint32_t expected, bool known = true) {
        auto call = args({at});
        const auto result = registry.dispatch("__imp__MmQueryAddressProtect", call);
        assert(result.handled() == known);
        assert(call.returnValue == (known ? expected : 0xDEADBEEF));
    };
    const auto querySize = [&](std::uint32_t at, std::uint32_t expected, bool known = true) {
        auto call = args({at});
        // Xenon pointers use the low word; untouched args and guest canaries
        // also catch an accidental pointer/output ABI interpretation.
        call.arguments[0] |= 0xFFFFFFFF00000000ULL;
        const auto inputs = call.arguments;
        put(space, 0x3000, 0xABCD1234);
        const auto result = registry.dispatch("__imp__MmQueryAllocationSize", call);
        assert(result.handled() == known && call.returnValue == (known ? expected : 0xDEADBEEF));
        assert(call.arguments == inputs && get(space, 0x3000) == 0xABCD1234);
    };
    for (auto address : {0u,UINT32_MAX,Physical,Heap-1,Heap+HeapSize,0x82000000u,0x7FC80714u})
        querySize(address,0,false);
    querySize(Heap,0); querySize(Heap+HeapSize-1,0);
    query(0, 0, false); query(0xFFFFFFFF, 0, false);
    query(Physical, 0, false); query(Heap - 1, 0, false);
    query(Heap, 0); query(Heap + HeapSize - 1, 0);
    query(Heap + HeapSize, 0, false);
    assert(registry.bindPhysicalMemoryArena(Physical, PhysicalSize, 0));
    query(Physical, 0); query(Physical + PhysicalSize - 1, 0);
    query(Physical + PhysicalSize, 0, false);
    querySize(Physical,0); querySize(Physical+PhysicalSize-1,0);
    querySize(Physical+PhysicalSize,0,false);

    // Both base permissions, all supported cache combinations and page sizes.
    for (const std::uint32_t access : {2U, 4U}) {
        for (const std::uint32_t cache : {0U, 0x200U, 0x400U, 0x600U}) {
            for (const std::uint32_t modifier : {0U, 0x20000000U, 0x80000000U}) {
                const auto size = modifier == 0 ? 0x1000U
                        : modifier == 0x20000000U ? 0x10000U : 0x1000000U;
                auto call = args({0, 1, access | cache | modifier, 0, 0xFFFFFFFF, 0});
                assert(registry.dispatch("__imp__MmAllocatePhysicalMemoryEx", call).handled());
                const auto base = static_cast<std::uint32_t>(call.returnValue);
                assert(base == Physical + PhysicalSize - size);
                query(base, access | cache); query(base + size / 2, access | cache);
                query(base + size - 1, access | cache); query(base - 1, 0);
                for (auto offset : {0u,1u,size/2,size-1}) querySize(base+offset,size);
                querySize(base-1,0); querySize(base+size,0,false);
                auto release = args({0, base});
                assert(registry.dispatch("__imp__MmFreePhysicalMemory", release).handled());
                query(base, 0); query(base + size - 1, 0);
                querySize(base,0); querySize(base+size-1,0);
            }
        }
    }
    for (const std::uint32_t type : {0x2000U, 0x3000U}) {
        put(space, 0x2000, 0); put(space, 0x2004, 0x1001);
        auto allocate = args({0x2000, 0x2004, type, 4, 0});
        assert(registry.dispatch("__imp__NtAllocateVirtualMemory", allocate).handled());
        assert(allocate.returnValue == 0 && get(space, 0x2000) == Heap);
        query(Heap, 4); query(Heap + 0x1FFF, 4); query(Heap + 0x2000, 0);
        querySize(Heap,0x2000); querySize(Heap+0x1FFF,0x2000); querySize(Heap+0x2000,0);
        put(space, 0x2004, 0x1000);
        auto commit = args({0x2000, 0x2004, 0x1000, 4, 0});
        assert(registry.dispatch("__imp__NtAllocateVirtualMemory", commit).handled());
        assert(commit.returnValue == 0);
        query(Heap, 4); query(Heap + 0x1FFF, 4);
        querySize(Heap+1,0x2000); querySize(Heap+0x1000,0x2000);
        put(space, 0x2004, 0);
        auto release = args({0x2000, 0x2004, 0x8000, 0});
        assert(registry.dispatch("__imp__NtFreeVirtualMemory", release).handled());
        assert(release.returnValue == 0); query(Heap, 0); query(Heap + 0x1FFF, 0);
        querySize(Heap,0); querySize(Heap+0x1FFF,0);
    }
    registry.unbindPhysicalMemoryArena(); query(Physical, 0, false);
    querySize(Physical,0,false);
    registry.reset();
    assert(registry.initialize(space, Heap, HeapSize));
    query(Heap, 0); query(Physical, 0, false);
    querySize(Heap,0); querySize(Physical,0,false);

    // Startup requires these two blocks simultaneously. The old 256 MiB
    // trace configuration exhausted its arena; returning null is correct,
    // but the title subsequently constructed its main heap from that null.
    for (const std::uint32_t arenaSize : {0x10000000U, 0x20000000U}) {
        assert(registry.bindPhysicalMemoryArena(Physical, arenaSize, 0));
        auto first = args({0, 0x940000, 0x20000004, 0, 0xFFFFFFFF, 0});
        assert(registry.dispatch("__imp__MmAllocatePhysicalMemoryEx", first).handled());
        assert(first.returnValue == Physical + arenaSize - 0x940000);
        querySize(static_cast<std::uint32_t>(first.returnValue),0x940000);
        auto main = args({0, 0x17000000, 0x20000004, 0, 0xFFFFFFFF, 0});
        assert(registry.dispatch("__imp__MmAllocatePhysicalMemoryEx", main).handled());
        if (arenaSize == 0x10000000) {
            assert(main.returnValue == 0);
        } else {
            assert(main.returnValue == Physical + arenaSize - 0x940000 - 0x17000000);
            query(static_cast<std::uint32_t>(main.returnValue), 4);
            query(static_cast<std::uint32_t>(first.returnValue) - 1, 4);
            querySize(static_cast<std::uint32_t>(main.returnValue),0x17000000);
            querySize(static_cast<std::uint32_t>(first.returnValue)-1,0x17000000);
            querySize(static_cast<std::uint32_t>(first.returnValue),0x940000);
        }
        registry.unbindPhysicalMemoryArena();
        query(Physical, 0, false);
        querySize(Physical,0,false);
    }
}
