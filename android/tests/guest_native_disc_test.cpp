#include "guest_import_registry.h"
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace recomp::bootstrap;
int main() {
    GuestAddressSpace space; assert(space.reserve());
    GuestImportRegistry registry;
    constexpr std::uint32_t Header = 0x20000000, Out = 0x3000;
    auto put = [&](std::uint32_t at, std::uint32_t value) {
        auto* p = space.translate(at);
        for (unsigned i = 0; i < 4; ++i) p[i] = value >> (24 - 8 * i);
    };
    auto get = [&](std::uint32_t at) {
        const auto* p = space.translate(at);
        return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16)
            | (std::uint32_t{p[2]} << 8) | p[3];
    };
    assert(!registry.bindMountedDisc(Header));
    assert(registry.initialize(space, 0x10000000, 0x10000));
    assert(registry.bindGuestThreadToken(0x5000));
    assert(!registry.referenceNativeEvent(256));
    assert(!registry.bindMountedDisc(Header));
    std::memcpy(space.translate(Header), "XEX2", 4);
    put(Header + 8, 0x200); put(Header + 20, 1);
    put(Header + 24, 0x40006); put(Header + 28, 0x100);
    put(Header + 0x10C, 0x12345678);
    put(Header + 0x110, 0x00000201);
    assert(!registry.bindMountedDisc(Header));
    put(Header + 0x110, 0x00000202);
    assert(registry.bindMountedDisc(Header));
    assert(!registry.bindMountedDisc(Header));
    auto call = [&](const char* name, std::array<std::uint64_t, 8> args,
                    bool expected, std::uint64_t status = 0) {
        GuestImportCall c; c.arguments = args; c.returnValue = 0xABCDEF0123456789;
        const auto result = registry.dispatch(name, c);
        if (result.handled() != expected)
            std::fprintf(stderr, "Unexpected handling: %s state=%u\n", name, unsigned(result.state));
        assert(result.handled() == expected);
        if (expected) assert(c.returnValue == status);
    };
    call("__imp__NtCreateEvent", {Out, 0, 0, 0}, true);
    const auto handle = get(Out);
    const auto reference = registry.referenceNativeEvent(handle);
    assert(reference && *reference && *reference != handle);
    assert(registry.referenceNativeEvent(handle) == reference);
    assert(registry.referenceNativeEvent(0xDEAD) == 0U);
    call("__imp__XamSwapDisc", {1, *reference, 0}, false);
    call("__imp__XamSwapDisc", {2, *reference, 0}, true);
    call("__imp__NtWaitForSingleObjectEx", {handle, 1, 0, 0}, true);
    call("__imp__XamSwapDisc", {2, *reference, 0}, true);
    call("__imp__XamSwapDisc", {2, *reference, 0}, false);
    assert((registry.nativeDiscStatistics() == std::array<std::uint32_t, 3>{2, 2, 2}));
    assert(registry.referenceNativeEvent(handle) == reference);
    assert(registry.referenceNativeEvent(handle) == reference);
    call("__imp__NtClose", {handle}, true);
    assert(registry.referenceNativeEvent(handle) == 0U);
    call("__imp__XamSwapDisc", {2, *reference, 0}, true);
    call("__imp__ObDereferenceObject", {*reference}, true, 0xABCDEF0123456789);
    call("__imp__ObDereferenceObject", {*reference}, false);
    put(Out, *reference & ~0xFFFU); put(Out + 4, 0);
    call("__imp__NtFreeVirtualMemory", {Out, Out + 4, 0x8000, 0}, false);
    assert(registry.bindNoLaunchData());
    call("__imp__XamLoaderGetLaunchDataSize", {*reference}, false);
    registry.reset();
    assert(registry.initialize(space, 0x10000000, 0x10000));
    assert(!registry.referenceNativeEvent(handle));
    call("__imp__XamSwapDisc", {2, *reference, 0}, false);
}
