#include "guest_import_registry.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace recomp::bootstrap;
namespace {
constexpr auto Name = "__imp__RtlFillMemoryUlong";
constexpr std::uint32_t Out = 0x30000000, Heap = 0x10000000, Physical = 0xA0000000;
constexpr std::uint64_t Return = 0xFEDCBA9876543210ULL;
void put(GuestAddressSpace& s, std::uint32_t a, std::uint32_t v) {
    for (unsigned b = 0; b < 4; ++b) s.translate(a)[b] = v >> (24 - b * 8);
}
std::uint32_t get(GuestAddressSpace& s, std::uint32_t a) {
    const auto* p = s.translate(a);
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}
std::uint32_t invoke(GuestImportRegistry& r, const char* name, std::initializer_list<std::uint32_t> args) {
    GuestImportCall c; std::copy(args.begin(), args.end(), c.arguments.begin());
    assert(r.dispatch(name, c).handled()); return c.returnValue;
}
void fill(GuestImportRegistry& r, GuestAddressSpace& s, std::uint32_t address,
          std::uint32_t bytes, std::uint64_t pattern, bool accepted) {
    GuestImportCall c; c.arguments = {address, bytes, pattern}; c.returnValue = Return;
    c.stackPointer = 0x640AED90;
    const auto arguments = c.arguments;
    const auto available = GuestAddressSpace::Size - address;
    const auto snapshotSize = static_cast<std::size_t>(std::min<std::uint64_t>(available, std::min(bytes, 1024U)));
    const auto* p = s.base() + address;
    const std::vector<std::uint8_t> before(p, p + snapshotSize);
    assert(r.dispatch(Name, c).handled() == accepted);
    assert(c.arguments == arguments && c.returnValue == Return && c.stackPointer == 0x640AED90);
    if (!accepted) assert(std::equal(before.begin(), before.end(), p));
    else for (std::uint32_t i = 0; i < bytes; ++i)
        assert(p[i] == static_cast<std::uint8_t>(pattern >> (24 - (i % 4) * 8)));
}
void positive(GuestImportRegistry& r, GuestAddressSpace& s, std::uint32_t address,
              std::uint32_t bytes, std::uint64_t pattern) {
    assert(address >= 4 && std::uint64_t{address} + bytes + 4 <= GuestAddressSpace::Size);
    std::fill_n(s.translate(address - 4), bytes + 8, 0xA5);
    fill(r, s, address, bytes, pattern, true);
    assert(get(s, address - 4) == 0xA5A5A5A5 && get(s, address + bytes) == 0xA5A5A5A5);
}
void ordinary(GuestAddressSpace& s) {
    GuestImportRegistry r;
    fill(r, s, Out, 4, 0, false);
    assert(r.initialize(s, Heap, 0x10000));
    const auto& descriptors = r.implementedBootImports();
    const auto descriptor = std::find_if(descriptors.begin(), descriptors.end(), [](auto d) { return d.name == Name; });
    assert(descriptor != descriptors.end() && !descriptor->writesReturnValue);
    for (auto bytes : {0U, 4U, 800U, 4096U, 1024U * 1024})
        for (auto pattern : {0ULL, 0xFFFFFFFFULL, 0x01234567ULL, 0xFFFFFFFF80000000ULL})
            positive(r, s, Out, bytes, pattern);
    for (unsigned bit = 0; bit < 32; ++bit) positive(r, s, Out, 800, 1U << bit);
    for (auto address : {0U, Out, 0xFFFFFFFCU, 0x7FC80000U}) fill(r, s, address, 0, 0, true);
    for (auto offset : {1U, 2U, 3U}) {
        fill(r, s, Out + offset, 4, 0, false);
        fill(r, s, Out + offset, 0, 0, false);
        fill(r, s, Out, 800 + offset, 0, false);
    }
    for (auto bytes : {1024U * 1024 + 4, 0xFFFFFFFCU, 0xFFFFFFFFU}) fill(r, s, Out, bytes, 0, false);
    for (auto address : {0U, 0x7FC80000U - 4, 0x7FC80000U, 0x7FC8FFFCU, 0xFFFFFFFCU})
        fill(r, s, address, 8, 0, false);
    fill(r, s, 0xFFFFFFFC, 4, 0x12345678, true); // Exact end; no uint32 wrap.
    constexpr auto Tls = 0x63002000U, Parent = 0x63001000U;
    assert(r.bindGuestThreadToken(Parent) && r.bindGuestThreadTlsStorage(Parent, Tls, 4));
    for (auto address : {Tls - 4, Tls, Tls + 12}) fill(r, s, address, 8, 0, false);
    invoke(r, "__imp__RtlInitializeCriticalSectionAndSpinCount", {Out + 1024, 10000});
    for (auto address : {Out + 1020, Out + 1024, Out + 1048}) fill(r, s, address, 8, 0, false);
    r.reset(); fill(r, s, Out, 4, 0, false);
}
void ownership(GuestAddressSpace& s) {
    GuestImportRegistry r; assert(r.initialize(s, Heap, 0x10000));
    assert(r.bindPhysicalMemoryArena(Physical, 0x10000, 0));
    for (auto address : {Heap - 4, Heap, Physical - 4, Physical}) fill(r, s, address, 8, 0, false);
    const auto ro = invoke(r, "__imp__MmAllocatePhysicalMemoryEx", {0, 4096, 2, 0, UINT32_MAX, 0});
    const auto rw = invoke(r, "__imp__MmAllocatePhysicalMemoryEx", {0, 8192, 4, 0, UINT32_MAX, 0});
    assert(ro && rw); fill(r, s, ro, 4, 0, false);
    positive(r, s, rw + 4, 800, 0x80000000);
    positive(r, s, rw + 4092, 8, 0x01234567); // Two pages, one owned allocation.
    fill(r, s, rw - 4, 8, 0, false); fill(r, s, rw + 8192 - 4, 8, 0, false);
    invoke(r, "__imp__MmFreePhysicalMemory", {0, rw}); fill(r, s, rw, 4, 0, false);
    put(s, Out, 0); put(s, Out + 4, 8192);
    assert(invoke(r, "__imp__NtAllocateVirtualMemory", {Out, Out + 4, 0x2000, 4, 0}) == 0);
    assert(get(s, Out) == Heap); fill(r, s, Heap, 4, 0, false);
    put(s, Out, Heap); put(s, Out + 4, 4096);
    assert(invoke(r, "__imp__NtAllocateVirtualMemory", {Out, Out + 4, 0x1000, 4, 0}) == 0);
    positive(r, s, Heap + 4, 800, 0x12345678);
    fill(r, s, Heap + 4092, 8, 0, false); // Reject whole request before committed prefix writes.
    const auto event = Heap + 1024;
    put(s, event, 0); put(s, event + 4, 0); put(s, event + 8, event + 8); put(s, event + 12, event + 8);
    assert(r.bindGuestThreadToken(0x63001000));
    const GuestThreadStorageConfig config{{0x64000000, 0x100000}, {0x82000000, 0x10000},
        {0x82001000, 0x1000}, 0, 0, 32, 4, 0x4000, 2};
    assert(r.bindThreadStorageArena(config, {}, [](auto a) { return a == 0x82001000; }));
    assert(r.bindThreadResumeHandler([](const auto&, std::int32_t) { return false; }));
    unsigned checks = 0;
    assert(r.bindThreadWaitHandler([&](const auto&) {
        for (auto address : {event - 4, event, event + 12}) fill(r, s, address, 8, 0, false);
        ++checks; return false; // Cancellation removes the registered wait without signaling it.
    }));
    GuestImportCall wait; wait.arguments = {event, 3, 1, 0, 0};
    assert(!r.dispatch("__imp__KeWaitForSingleObject", wait).handled() && checks == 1);
    assert(get(s, event) == 0 && get(s, event + 4) == 0 && get(s, event + 8) == event + 8);
    put(s, Out, Heap); put(s, Out + 4, 0);
    assert(invoke(r, "__imp__NtFreeVirtualMemory", {Out, Out + 4, 0x8000, 0}) == 0);
    fill(r, s, Heap, 4, 0, false);
}
void image(GuestAddressSpace& s) {
    constexpr std::uint32_t Header = 0x60000000, Base = 0x82000000, Security = 0x80, Descriptors = Security + 0x184;
    std::fill_n(s.translate(Header), 0x400, 0);
    for (auto pair : {std::array<std::uint32_t, 2>{0, 0x58455832}, {8, 0x400}, {16, Security}, {20, 1},
                     {Security, 0x184 + 2 * 24}, {Security + 4, 0x20000}, {Security + 0x110, Base},
                     {Security + 0x180, 2}, {Descriptors, 0x13}, {Descriptors + 24, 0x12}})
        put(s, Header + pair[0], pair[1]);
    GuestImportRegistry r; assert(r.initialize(s, Heap, 0x10000));
    assert(r.bindImagePageProtections(Header, 0x400, Base, 0x20000));
    fill(r, s, Base, 4, 0, false); fill(r, s, Base + 65532, 8, 0, false);
    positive(r, s, Base + 65540, 800, 0x12345678);
}
void workers(GuestAddressSpace& s) {
    constexpr std::uint32_t Parent = 0x63001000, Code = 0x82001000, Arena = 0x64000000;
    GuestImportRegistry r; assert(r.initialize(s, Heap, 0x10000));
    assert(r.bindGuestThreadToken(Parent));
    const GuestThreadStorageConfig config{{Arena, 0x100000}, {0x82000000, 0x10000},
        {Code, 0x1000}, 0, 0, 32, 4, 0x4000, 2};
    assert(r.bindThreadStorageArena(config, {}, [](auto a) { return a == Code; }));
    const auto first = r.createNativeCallbackWorker(Code, 0, [](const auto&) { return true; });
    const auto other = r.createNativeCallbackWorker(Code, 0, [](const auto&) { return true; });
    assert(first && other);
    fill(r, s, first->layout.stackBase + 192, 800, 0, false); // Parent cannot fill worker stack.
    assert(r.bindGuestThreadToken(first->layout.threadAddress));
    positive(r, s, first->layout.stackBase + 192, 800, 0xFFFFFFFF80000000ULL);
    for (auto address : {first->layout.pcrAddress, first->layout.threadAddress, first->layout.tlsAddress,
                        first->dynamicTlsAddress, other->layout.stackBase + 192, Arena + 0xF0000,
                        first->layout.stackBase - 4, first->layout.stackBase + first->layout.stackSize - 4})
        fill(r, s, address, 8, 0, false);
    assert(r.markGuestThreadReturned(first->layout.threadAddress, 0));
    fill(r, s, first->layout.stackBase + 192, 800, 0, false);
}
}
int main() {
    GuestAddressSpace space; assert(space.reserve());
    ordinary(space); ownership(space); image(space); workers(space);
    std::puts("ULONG_FILL: BE patterns/32bits, void ABI, canaries, full preflight,1MiB bound, heap/physical/image/TLS/worker/event ownership PASS");
}
