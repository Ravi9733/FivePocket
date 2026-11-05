#include "guest_import_registry.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>

using namespace recomp::bootstrap;

namespace {
constexpr std::uint32_t Initial = 0x30000000, Output = 0x31000000;
constexpr std::uint32_t Record = 0x32000000, Text = 0x33000000, Code = 0x101000;
constexpr std::uint64_t Sentinel = 0x123456789ABCDEF0ULL;
void put(GuestAddressSpace& space, std::uint32_t at, std::uint32_t word) {
    auto* p = space.translate(at);
    p[0] = word >> 24; p[1] = word >> 16; p[2] = word >> 8; p[3] = word;
}
GuestImportDispatchResult call(GuestImportRegistry& registry, std::string_view name,
        GuestImportCall& c, std::initializer_list<std::uint32_t> args) {
    c = {}; c.returnValue = Sentinel;
    std::copy(args.begin(), args.end(), c.arguments.begin());
    return registry.dispatch(name, c);
}
void stage(GuestAddressSpace& space, std::uint32_t thread = 0xFFFFFFFFU) {
    const std::array<std::uint32_t, 9> words{0x406D1388, 0, 0, Code, 4, 0x1000, Text, thread, 0};
    for (std::uint32_t i = 0; i < words.size(); ++i) { put(space, Record + i * 4, words[i]); }
}
} // namespace

int main() {
    GuestAddressSpace space;
    assert(space.reserve());
    GuestImportRegistry registry;
    assert(!registry.bindThreadDebugNaming());
    assert(registry.initialize(space, 0x10000000, 0x10000, [](auto) {
        assert(false && "Guest thread names must never reach the logging sink");
    }));
    assert(registry.bindGuestThreadToken(Initial));
    put(space, Initial + 0x14C, 1);
    assert(registry.bindInitialThreadObject(Initial, 1));
    GuestImportCall c;
    stage(space);
    std::memcpy(space.translate(Text), "synthetic main", 15);
    assert(!call(registry, "__imp__RtlRaiseException", c, {Record}).handled());
    assert(registry.bindThreadDebugNaming() && registry.bindThreadDebugNaming());
    assert(call(registry, "__imp__RtlRaiseException", c, {Record}).handled());
    assert(c.returnValue == Sentinel && registry.guestThreadDebugName(Initial) == "synthetic main");
    for (const auto& descriptor : GuestImportRegistry::implementedBootImports()) {
        if (descriptor.name == "__imp__RtlRaiseException") { assert(!descriptor.writesReturnValue); }
    }
    // Actual faults, C++ exceptions, malformed notifications, unknown IDs, and
    // noncontinuable/chained records are not swallowed or treated as names.
    for (const auto& [offset, value] : std::array<std::pair<std::uint32_t, std::uint32_t>, 10>{{
            {0, 0xC0000005}, {0, 0xE06D7363}, {4, 1}, {8, Record},
            {16, 3}, {16, 5}, {20, 0}, {28, 0}, {28, 12345}, {32, 1}}}) {
        stage(space); put(space, Record + offset, value);
        assert(!call(registry, "__imp__RtlRaiseException", c, {Record}).handled());
        assert(c.returnValue == Sentinel && registry.guestThreadDebugName(Initial) == "synthetic main");
    }
    stage(space, 1);
    for (const auto invalidRecord : {0U, Record + 1, 0xFFFFFFF0U}) {
        assert(!call(registry, "__imp__RtlRaiseException", c, {invalidRecord}).handled());
    }
    put(space, Record + 24, 0);
    assert(!call(registry, "__imp__RtlRaiseException", c, {Record}).handled());
    put(space, Record + 24, 0xFFFFFFFF);
    *space.translate(0xFFFFFFFF) = 'X';
    assert(!call(registry, "__imp__RtlRaiseException", c, {Record}).handled());
    stage(space);
    std::memset(space.translate(Text), 'x', 256);
    assert(!call(registry, "__imp__RtlRaiseException", c, {Record}).handled());
    *space.translate(Text + 255) = 0;
    assert(call(registry, "__imp__RtlRaiseException", c, {Record}).handled());
    assert(registry.guestThreadDebugName(Initial)->size() == 255);
    *space.translate(Text) = 0;
    assert(call(registry, "__imp__RtlRaiseException", c, {Record}).handled());
    assert(registry.guestThreadDebugName(Initial) == "");

    GuestThreadStorageConfig config{{0x20000000, 0x100000}, {0x100000, 0x10000}, {Code, 0x1000},
            0x100010, 4, 0x20, 4, 0x4000, 2};
    assert(registry.bindThreadStorageArena(config, {{Initial, 0x2000}},
            [](auto entry) { return entry == Code; }));
    assert(call(registry, "__imp__ExCreateThread", c, {Output, 0, Output + 4, 0, Code, 0, 1}).handled());
    assert(c.returnValue == 0);
    const auto* bytes = space.translate(Output);
    const auto handle = (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16)
            | (std::uint32_t{bytes[2]} << 8) | bytes[3];
    const auto worker = registry.lookupSuspendedThread(handle);
    assert(worker);
    stage(space, worker->layout.threadId);
    std::memcpy(space.translate(Text), "synthetic worker", 17);
    assert(call(registry, "__imp__RtlRaiseException", c, {Record}).handled());
    assert(registry.guestThreadDebugName(worker->layout.threadAddress) == "synthetic worker");
    put(space, worker->layout.threadAddress + 0x14C, 999);
    assert(!call(registry, "__imp__RtlRaiseException", c, {Record}).handled());
    put(space, worker->layout.threadAddress + 0x14C, worker->layout.threadId);
    assert(registry.bindGuestThreadToken(worker->layout.threadAddress));
    stage(space);
    assert(call(registry, "__imp__RtlRaiseException", c, {Record}).handled());
    assert(c.returnValue == Sentinel);
    registry.reset();
    assert(!registry.guestThreadDebugName(Initial) && !registry.guestThreadDebugName(worker->layout.threadAddress));
    assert(!registry.bindThreadDebugNaming());
}
