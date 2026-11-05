#include "guest_import_registry.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <vector>

using namespace recomp::bootstrap;
namespace {
constexpr std::uint32_t Out = 0x30000000, Timeout = Out + 16;
GuestImportDispatchResult call(GuestImportRegistry& r, std::string_view name, GuestImportCall& c,
        std::initializer_list<std::uint64_t> args) {
    c = {}; c.returnValue = 0xABCD;
    std::copy(args.begin(), args.end(), c.arguments.begin());
    return r.dispatch(name, c);
}
std::uint32_t invoke(GuestImportRegistry& r, std::string_view name, std::initializer_list<std::uint64_t> args) {
    GuestImportCall c;
    assert(call(r, name, c, args).handled());
    return static_cast<std::uint32_t>(c.returnValue);
}
std::uint32_t read(GuestAddressSpace& s, std::uint32_t at) {
    const auto* p = s.translate(at);
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}
}
int main() {
    GuestAddressSpace space;
    assert(space.reserve());
    GuestImportRegistry registry;
    assert(!registry.bindNotificationService() && !registry.postNotification(9, 0));
    assert(registry.initialize(space, 0x10000000, 0x10000));
    assert(registry.bindGuestThreadToken(0x40000000));
    GuestImportCall c;
    assert(!call(registry, "__imp__XamNotifyCreateListener", c, {1, 10}).handled());
    assert(!registry.notificationUiPosition());
    assert(!call(registry, "__imp__XNotifyPositionUI", c, {9}).handled());
    assert(registry.bindNotificationService() && registry.bindNotificationService());
    assert(call(registry, "__imp__XNotifyPositionUI", c, {9}).handled());
    assert(c.returnValue == 0xABCD && registry.notificationUiPosition() == 9);
    assert(call(registry, "__imp__XNotifyPositionUI", c, {2}).handled());
    assert(c.returnValue == 0xABCD && registry.notificationUiPosition() == 2);
    assert(!call(registry, "__imp__XamNotifyCreateListener", c, {1, 11}).handled());
    const auto a = invoke(registry, "__imp__XamNotifyCreateListener", {1, 10});
    const auto b = invoke(registry, "__imp__XamNotifyCreateListener", {1, 0});
    const auto high = invoke(registry, "__imp__XamNotifyCreateListener", {std::uint64_t{1} << 63, 10});
    assert(a && b && high && a != b);
    std::memset(space.translate(Timeout), 0, 8);
    const auto wait = [&](auto handle) { return invoke(registry, "__imp__NtWaitForSingleObjectEx", {handle, 1, 0, Timeout}); };
    const auto next = [&](std::uint32_t handle, std::uint32_t match = 0U) {
        return invoke(registry, "__imp__XNotifyGetNext", {handle, match, Out, Out + 4});
    };
    assert(wait(a) == 0x102 && next(a) == 0 && read(space, Out) == 0); // No invented startup events.
    assert(registry.postNotification(9, 100));
    assert(registry.postNotification(0x0001000A, 200)); // Version 1, filtered out of b.
    assert(registry.postNotification(0x7E00000B, 300)); // Mask bit 63 must not be truncated.
    assert(!registry.postNotification(0x80000009, 1));
    assert(wait(a) == 0 && wait(a) == 0 && wait(high) == 0);
    assert(invoke(registry, "__imp__NtSetEvent", {a, 0}) == 0xC0000024);
    assert(invoke(registry, "__imp__NtReleaseSemaphore", {a, 1, 0}) == 0xC0000024);
    assert(next(a, 0x0001000A) == 1 && read(space, Out + 4) == 200);
    assert(wait(a) == 0 && next(a, 1234) == 0 && wait(a) == 0);
    assert(next(a) == 1 && read(space, Out) == 9 && read(space, Out + 4) == 100);
    assert(wait(a) == 0x102);
    assert(next(b) == 1 && next(b) == 0);
    assert(next(high) == 1 && read(space, Out) == 0x7E00000B);
    assert(registry.postNotification(9, 7));
    std::memset(space.translate(Out), 0xA5, 8);
    assert(!call(registry, "__imp__XNotifyGetNext", c, {a, 0, Out + 1, Out + 4}).handled());
    assert(read(space, Out) == 0xA5A5A5A5 && wait(a) == 0);
    assert(!call(registry, "__imp__XNotifyGetNext", c, {a, 0, Out, Out}).handled());
    assert(invoke(registry, "__imp__XNotifyGetNext", {a, 0, 0, Out + 4}) == 0 && wait(a) == 0);
    assert(invoke(registry, "__imp__XNotifyGetNext", {a, 0, Out, 0}) == 1 && read(space, Out) == 9);
    assert(next(b) == 1);
    for (std::size_t i = 0; i < GuestImportRegistry::MaximumQueuedNotifications; ++i) { assert(registry.postNotification(9, i)); }
    assert(next(b) == 1);
    assert(!registry.postNotification(9, 999)); // a is full: no partial broadcast into b.
    for (std::size_t i = 0; i < GuestImportRegistry::MaximumQueuedNotifications; ++i) {
        assert(next(a) == 1 && read(space, Out + 4) == i);
    }
    for (std::size_t i = 1; i < GuestImportRegistry::MaximumQueuedNotifications; ++i) {
        assert(next(b) == 1 && read(space, Out + 4) == i);
    }
    assert(next(b) == 0 && wait(b) == 0x102);
    assert(invoke(registry, "__imp__NtClose", {a}) == 0 && next(a) == 0);
    std::vector<std::uint32_t> handles{b, high};
    for (std::size_t i = handles.size(); i < GuestImportRegistry::MaximumNotificationListeners; ++i) {
        const auto handle = invoke(registry, "__imp__XamNotifyCreateListener", {0, 10});
        assert(handle != 0 && handle != a); handles.push_back(handle);
    }
    assert(invoke(registry, "__imp__XamNotifyCreateListener", {1, 10}) == 0);
    for (const auto handle : handles) { assert(invoke(registry, "__imp__NtClose", {handle}) == 0); }
    assert(invoke(registry, "__imp__XamNotifyCreateListener", {1, 10}) != 0);
    registry.reset();
    assert(!registry.notificationUiPosition());
    assert(!registry.postNotification(9, 0) && !registry.bindNotificationService());
}
