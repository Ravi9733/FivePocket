#include "guest_import_registry.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <initializer_list>

using namespace recomp::bootstrap;
namespace {
constexpr std::uint32_t Heap = 0x10000000, Parent = 0x31000000, Other = 0x32000000;
constexpr std::uint32_t Out = 0x30000000, Timeout = Out + 8, Code = 0x101000;
void put(GuestAddressSpace& s, std::uint32_t at, std::uint32_t value) {
    auto* p = s.translate(at);
    for (unsigned i = 0; i < 4; ++i) { p[i] = value >> (24 - 8 * i); }
}
std::uint32_t get(GuestAddressSpace& s, std::uint32_t at) {
    const auto* p = s.translate(at);
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16)
            | (std::uint32_t{p[2]} << 8) | p[3];
}
GuestImportDispatchResult call(GuestImportRegistry& r, const char* name, GuestImportCall& c,
        std::initializer_list<std::uint32_t> args) {
    c = {}; c.returnValue = 0xDEADBEEF;
    std::copy(args.begin(), args.end(), c.arguments.begin());
    return r.dispatch(name, c);
}
std::uint32_t invoke(GuestImportRegistry& r, const char* name, std::initializer_list<std::uint32_t> args) {
    GuestImportCall c;
    assert(call(r, name, c, args).handled());
    return static_cast<std::uint32_t>(c.returnValue);
}
std::uint32_t create(GuestImportRegistry& r, GuestAddressSpace& s, bool owned) {
    assert(invoke(r, "__imp__NtCreateMutant", {Out, 0, owned ? 1U : 0U}) == 0);
    return get(s, Out);
}
void poll(GuestImportRegistry& r, std::uint32_t h, std::uint32_t status) {
    assert(invoke(r, "__imp__NtWaitForSingleObjectEx", {h, 1, 0, Timeout}) == status);
}
void release(GuestImportRegistry& r, std::uint32_t h, std::uint32_t status = 0) {
    assert(invoke(r, "__imp__NtReleaseMutant", {h, 0}) == status);
}
void initialize(GuestImportRegistry& r, GuestAddressSpace& s) {
    assert(r.initialize(s, Heap, 0x10000));
    assert(r.bindGuestThreadToken(Parent));
    put(s, Timeout, 0); put(s, Timeout + 4, 0);
}
void ownership(GuestAddressSpace& s) {
    GuestImportRegistry r; initialize(r, s);
    auto h = create(r, s, false);
    release(r, h, 0xC0000046);
    poll(r, h, 0); poll(r, h, 0); // Recursive acquisition by the owner.
    assert(!r.teardownGuestThread(Parent));
    assert(r.bindGuestThreadToken(Other));
    poll(r, h, 0x102); release(r, h, 0xC0000046);
    assert(r.bindGuestThreadToken(Parent)); release(r, h);
    assert(r.bindGuestThreadToken(Other)); poll(r, h, 0x102);
    assert(r.bindGuestThreadToken(Parent)); release(r, h);
    assert(r.bindGuestThreadToken(Other)); poll(r, h, 0); release(r, h);
    assert(invoke(r, "__imp__NtClose", {h}) == 0);
    release(r, h, 0xC0000008); poll(r, h, 0xC0000008);
    auto replacement = create(r, s, true); assert(replacement != h);
    release(r, replacement);

    for (std::uint32_t i = 0; i < GuestImportRegistry::MaximumMutantRecursion; ++i) { poll(r, replacement, 0); }
    GuestImportCall c;
    assert(!call(r, "__imp__NtWaitForSingleObjectEx", c, {replacement, 1, 0, Timeout}).handled());
    for (std::uint32_t i = 0; i < GuestImportRegistry::MaximumMutantRecursion; ++i) { release(r, replacement); }
    release(r, replacement, 0xC0000046);
    assert(invoke(r, "__imp__NtCreateEvent", {Out, 0, 0, 0}) == 0);
    release(r, get(s, Out), 0xC0000024);
    assert(invoke(r, "__imp__NtSetEvent", {replacement, 0}) == 0xC0000024);
    assert(invoke(r, "__imp__NtReleaseSemaphore", {replacement, 1, 0}) == 0xC0000024);
    put(s, Out, 0xA5A5A5A5);
    for (auto output : {0U, Out + 1, 0xFFFFFFFEU}) {
        assert(!call(r, "__imp__NtCreateMutant", c, {output, 0, 0}).handled());
    }
    assert(!call(r, "__imp__NtCreateMutant", c, {Out, 1, 0}).handled());
    assert(!call(r, "__imp__NtCreateMutant", c, {Out, 0, 2}).handled());
    assert(get(s, Out) == 0xA5A5A5A5);
    assert(!call(r, "__imp__NtReleaseMutant", c, {replacement, 1}).handled());
    r.unbindGuestThreadToken();
    assert(!call(r, "__imp__NtCreateMutant", c, {Out, 0, 0}).handled());
    r.reset(); initialize(r, s); poll(r, replacement, 0xC0000008);
    for (std::size_t i = 0; i < GuestImportRegistry::MaximumKernelHandles; ++i) { (void)create(r, s, false); }
    put(s, Out, 0xA5A5A5A5);
    assert(invoke(r, "__imp__NtCreateMutant", {Out, 0, 0}) == 0xC0000017);
    assert(get(s, Out) == 0xA5A5A5A5);
}
void parked(GuestAddressSpace& s) {
    GuestImportRegistry r; initialize(r, s);
    const auto bindWaitStorage = [&] {
        GuestThreadStorageConfig config{{0x20000000, 0x100000}, {0x100000, 0x10000}, {Code, 0x1000},
                0x100010, 4, 0x20, 4, 0x4000, 2};
        assert(r.bindThreadStorageArena(config, {{Parent, 0x2000}, {Other, 0x2000}, {Out, 16}},
                [](auto at) { return at == Code; }));
        assert(r.bindThreadResumeHandler([](const auto&, std::int32_t) { return false; }));
    };
    bindWaitStorage();
    const auto h = create(r, s, true);
    assert(r.bindGuestThreadToken(Other));
    assert(r.bindThreadWaitHandler([&](const auto& ready) {
        assert(!ready());
        assert(r.bindGuestThreadToken(Parent)); release(r, h);
        // Readiness is checked by the releasing task, but acquisition must
        // belong to the original waiter, not the currently bound registry task.
        assert(ready()); assert(ready()); release(r, h, 0xC0000046);
        assert(r.bindGuestThreadToken(Other));
        return true;
    }));
    assert(invoke(r, "__imp__NtWaitForSingleObjectEx", {h, 1, 0, 0}) == 0);
    release(r, h); release(r, h, 0xC0000046); // Readiness consumed exactly once.
    poll(r, h, 0);
    assert(r.bindGuestThreadToken(Parent));
    assert(r.bindThreadTimedWaitHandler([&](const auto& ready, const auto& deadline) {
        assert(!ready());
        assert(r.bindGuestThreadToken(Other)); release(r, h);
        assert(std::chrono::steady_clock::now() < deadline);
        assert(ready()); assert(r.bindGuestThreadToken(Parent)); return true;
    }));
    put(s, Timeout, 0xFFFFFFFF); put(s, Timeout + 4, 0xFF676980); // -1 second.
    assert(invoke(r, "__imp__NtWaitForSingleObjectEx", {h, 1, 0, Timeout}) == 0);
    release(r, h);
    r.reset(); initialize(r, s);
    bindWaitStorage();
    const auto closed = create(r, s, true);
    assert(r.bindGuestThreadToken(Other));
    assert(r.bindThreadWaitHandler([&](const auto& ready) {
        assert(!ready()); assert(invoke(r, "__imp__NtClose", {closed}) == 0);
        assert(ready()); return true;
    }));
    GuestImportCall c;
    assert(!call(r, "__imp__NtWaitForSingleObjectEx", c, {closed, 1, 0, 0}).handled());
}
void abandoned(GuestAddressSpace& s) {
    GuestImportRegistry r; initialize(r, s);
    assert(r.bindGuestThreadTlsStorage(Parent, Parent + 0x1000, 4));
    GuestThreadStorageConfig config{{0x20000000, 0x100000}, {0x100000, 0x10000}, {Code, 0x1000},
            0x100010, 4, 0x20, 4, 0x4000, 2};
    assert(r.bindThreadStorageArena(config, {{Parent, 0x2000}, {Out, 16}},
            [](auto at) { return at == Code; }));
    assert(invoke(r, "__imp__ExCreateThread", {Out, 0, Out + 4, 0, Code, 0, 1}) == 0);
    const auto handle = get(s, Out), token = r.lookupSuspendedThread(handle)->layout.threadAddress;
    assert(r.bindThreadObjectType(Out + 0x100));
    assert(r.bindThreadResumeHandler([](const auto&, std::int32_t) { return true; }));
    assert(invoke(r, "__imp__NtResumeThread", {handle, Out}) == 0);
    assert(r.bindGuestThreadToken(token));
    const auto h = create(r, s, true); poll(r, h, 0);
    assert(r.markGuestThreadReturned(token, 0));
    assert(!r.markGuestThreadReturned(token, 0));
    assert(r.bindGuestThreadToken(Parent)); poll(r, h, 0x80); poll(r, h, 0);
    release(r, h); release(r, h);
    assert(r.bindGuestThreadToken(Other)); poll(r, h, 0); release(r, h);
    r.reset();
}
}
int main() {
    GuestAddressSpace s; assert(s.reserve());
    ownership(s); parked(s); abandoned(s);
}
