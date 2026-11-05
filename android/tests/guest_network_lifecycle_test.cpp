#include "guest_import_registry.h"

#include <algorithm>
#include <cassert>
#include <cstring>

using namespace recomp::bootstrap;
namespace {
constexpr std::uint32_t Thread = 0x30000000, Params = 0x31000000, Out = 0x32000000, Size = 0x33000000;
void put(GuestAddressSpace& s, std::uint32_t at, std::uint32_t value) {
    auto* p = s.translate(at);
    p[0] = value >> 24; p[1] = value >> 16; p[2] = value >> 8; p[3] = value;
}
std::uint32_t get(GuestAddressSpace& s, std::uint32_t at) {
    const auto* p = s.translate(at);
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}
GuestImportDispatchResult call(GuestImportRegistry& r, std::string_view name, GuestImportCall& c,
        std::initializer_list<std::uint32_t> args) {
    c = {}; c.returnValue = 0xABCD;
    std::copy(args.begin(), args.end(), c.arguments.begin()); return r.dispatch(name, c);
}
std::uint32_t invoke(GuestImportRegistry& r, std::string_view name, std::initializer_list<std::uint32_t> args) {
    GuestImportCall c; assert(call(r, name, c, args).handled()); return static_cast<std::uint32_t>(c.returnValue);
}
}
int main() {
    GuestAddressSpace space;
    assert(space.reserve());
    GuestImportRegistry r;
    assert(r.initialize(space, 0x10000000, 0x10000));
    assert(!r.bindNetworkLifecycle());
    GuestImportCall link;
    assert(!call(r, "__imp__NetDll_XNetGetEthernetLinkStatus", link, {1}).handled());
    assert(!call(r, "__imp__NetDll_XNetGetTitleXnAddr", link, {1, Out}).handled());
    assert(r.bindGuestThreadToken(Thread));
    put(space, Thread + 0x14C, 1);
    assert(r.bindInitialThreadObject(Thread, 1) && r.bindNetworkLifecycle());
    assert(invoke(r, "__imp__NetDll_XNetGetEthernetLinkStatus", {1}) == 0);
    assert(!call(r, "__imp__NetDll_XNetGetEthernetLinkStatus", link, {0}).handled());
    assert(link.returnValue == 0xABCD);
    assert(invoke(r, "__imp__NetDll_WSACleanup", {1}) == 0xFFFFFFFFU);
    assert(invoke(r, "__imp__NetDll_WSAGetLastError", {}) == 10093);
    auto* parameters = space.translate(Params);
    std::memset(parameters, 0, 13); parameters[0] = 13; parameters[1] = 1;
    parameters[6] = parameters[7] = 64;
    GuestImportCall c;
    assert(!call(r, "__imp__NetDll_XNetStartup", c, {0, Params}).handled());
    assert(!call(r, "__imp__NetDll_XNetStartup", c, {1, 0}).handled());
    parameters[0] = 12;
    assert(!call(r, "__imp__NetDll_XNetStartup", c, {1, Params}).handled());
    parameters[0] = 13; parameters[1] = 2;
    assert(!call(r, "__imp__NetDll_XNetStartup", c, {1, Params}).handled());
    parameters[1] = 1;
    assert(invoke(r, "__imp__NetDll_XNetStartup", {1, Params}) == 0);
    assert(invoke(r, "__imp__NetDll_XNetStartup", {1, Params}) == 0);
    std::memset(space.translate(Out - 4), 0xA5, 44);
    put(space, Thread + 0x160, 1234);
    assert(invoke(r, "__imp__NetDll_XNetGetTitleXnAddr", {1, Out}) == 1);
    for (unsigned i = 0; i < 36; ++i) assert(space.translate(Out)[i] == 0);
    assert(get(space, Out - 4) == 0xA5A5A5A5U && get(space, Out + 36) == 0xA5A5A5A5U);
    assert(get(space, Thread + 0x160) == 1234); // No last-error side effect.
    for (const auto address : {0U, Out + 1, 0xFFFFFFFCU, Thread}) {
        assert(!call(r, "__imp__NetDll_XNetGetTitleXnAddr", c, {1, address}).handled());
        assert(c.returnValue == 0xABCD);
    }
    assert(!call(r, "__imp__NetDll_XNetGetTitleXnAddr", c, {0, Out}).handled());
    parameters[6] = 32;
    assert(!call(r, "__imp__NetDll_XNetStartup", c, {1, Params}).handled());
    parameters[6] = 64;
    put(space, Size, 0);
    assert(invoke(r, "__imp__NetDll_XNetGetOpt", {1, 1, 0, Size}) == 10040 && get(space, Size) == 13);
    assert(invoke(r, "__imp__NetDll_XNetGetOpt", {1, 1, Out, Size}) == 0);
    assert(std::memcmp(space.translate(Out), parameters, 13) == 0);
    assert(!call(r, "__imp__NetDll_XNetGetOpt", c, {1, 1, Size, Size}).handled());
    assert(!call(r, "__imp__NetDll_XNetGetOpt", c, {1, 2, Out, Size}).handled());
    put(space, Size, 100);
    assert(invoke(r, "__imp__NetDll_XNetGetOpt", {1, 1, Out + 1, Size}) == 0 && get(space, Size) == 100);
    assert(invoke(r, "__imp__NetDll_XNetCleanup", {1}) == 0);
    assert(!call(r, "__imp__NetDll_XNetGetOpt", c, {1, 1, Out, Size}).handled());
    assert(invoke(r, "__imp__NetDll_XNetGetTitleXnAddr", {1, Out}) == 1);
    std::memset(space.translate(Out), 0xA5, 0x198);
    for (const auto badVersion : {0U, 1U, 0x101U, 0x302U}) {
        assert(invoke(r, "__imp__NetDll_WSAStartup", {1, badVersion, Out}) == 10092);
        assert(get(space, Out) == 0xA5A5A5A5U);
    }
    assert(!call(r, "__imp__NetDll_WSAStartup", c, {1, 2, Out + 1}).handled());
    for (const auto version : {2U, 0x102U, 0x202U}) {
        assert(invoke(r, "__imp__NetDll_WSAStartup", {1, version, Out}) == 0);
        assert(get(space, Out) == ((version << 16) | 0x202));
        assert(get(space, Out + 0x18C) == 0xA5A5A5A5U && get(space, Out + 0x190) == 0xA5A5A5A5U);
    }
    for (int i = 0; i < 3; ++i) { assert(invoke(r, "__imp__NetDll_WSACleanup", {1}) == 0); }
    assert(invoke(r, "__imp__NetDll_WSACleanup", {1}) == 0xFFFFFFFFU);
    assert(invoke(r, "__imp__NetDll_WSAGetLastError", {}) == 10093);
    for (unsigned count = 0; count < UINT16_MAX; ++count) { assert(invoke(r, "__imp__NetDll_WSAStartup", {1, 2, Out}) == 0); }
    assert(!call(r, "__imp__NetDll_WSAStartup", c, {1, 2, Out}).handled());
    r.reset();
    assert(r.initialize(space, 0x10000000, 0x10000));
    assert(!call(r, "__imp__NetDll_WSACleanup", c, {1}).handled());
    assert(!call(r, "__imp__NetDll_XNetGetEthernetLinkStatus", link, {1}).handled());
}
