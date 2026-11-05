#include "guest_import_registry.h"

#include <algorithm>
#include <cassert>
#include <cstring>

using namespace recomp::bootstrap;
namespace {
constexpr std::uint32_t Module = 0x30000000, Title = 0x31000000, Out = 0x32000000, Name = 0x33000000;
std::uint32_t read(GuestAddressSpace& s, std::uint32_t at) {
    const auto* p = s.translate(at);
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}
void write(GuestAddressSpace& s, std::uint32_t at, std::uint32_t value) {
    auto* p = s.translate(at);
    p[0] = value >> 24; p[1] = value >> 16; p[2] = value >> 8; p[3] = value;
}
GuestImportDispatchResult call(GuestImportRegistry& r, std::string_view name, GuestImportCall& c,
        std::initializer_list<std::uint32_t> args) {
    c = {}; c.returnValue = 0xAA;
    std::copy(args.begin(), args.end(), c.arguments.begin());
    return r.dispatch(name, c);
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
    assert(!r.bindNativeXamModule(Module, Title, {}));
    GuestThreadStorageConfig config{{0x20000000, 0x100000}, {0x100000, 0x10000}, {0x101000, 0x1000},
            0x100010, 4, 0x20, 4, 0x4000, 2};
    assert(r.bindThreadStorageArena(config, {}, [](auto entry) { return entry == 0x101000; }));
    GuestImportCall c;
    std::memcpy(space.translate(Name), "xam.xex", 8);
    assert(!call(r, "__imp__XexLoadImage", c, {Name, 9, 0, Out}).handled());
    assert(!call(r, "__imp__XamGetSystemVersion", c, {}).handled());
    assert(!call(r, "__imp__XMsgStartIORequest", c, {0xFE, 0x22005, 0, Out, 8}).handled());
    assert(!call(r, "__imp__XMsgInProcessCall", c, {0xFA, 0x70009, Out, 0}).handled());
    for (auto bad : {0U, Module + 1, 0xFFFFFFFCU, 0x10000000U, 0x20000000U, 0x100000U, Title}) {
        assert(!r.bindNativeXamModule(bad, Title, {}));
    }
    assert(!r.bindNativeXamModule(Module, Title, {{Module, 0x100}}));
    *space.translate(Module) = 1;
    assert(!r.bindNativeXamModule(Module, Title, {}));
    *space.translate(Module) = 0;
    assert(!r.bindNativeXamModule(Module, Title, {}, 0x10000000));
    assert(r.bindNativeXamModule(Module, Title, {}, 0x203F4A00) && !r.bindNativeXamModule(Module, Title, {}));
    assert(invoke(r, "__imp__XamGetSystemVersion", {}) == 0x203F4A00);
    constexpr auto ProfileRead = "__imp__XamUserReadProfileSettings";
    constexpr std::uint32_t ProfileIds = Out + 0x500, ProfileSize = Out + 0x600;
    constexpr std::uint32_t ProfileBuffer = Out + 0x700, ProfileStack = 0x34000000;
    const auto profile = [&](std::uint32_t buffer) {
        c = {}; c.returnValue = 0xAA; c.stackPointer = ProfileStack;
        c.arguments = {0xFFFE07D1,255,0,0,3,ProfileIds,ProfileSize,buffer};
        return r.dispatch(ProfileRead, c);
    };
    assert(!profile(0).handled() && c.returnValue == 0xAA);
    constexpr auto Privilege = "__imp__XamUserCheckPrivilege";
    assert(!call(r, Privilege, c, {0, 252, Out}).handled() && c.returnValue == 0xAA);
    for (const auto name : {"__imp__XamUserGetXUID", "__imp__XamUserGetSigninInfo", "__imp__XamUserGetName"})
        assert(!call(r, name, c, {0, 1, Out}).handled() && c.returnValue == 0xAA);
    assert(r.bindSignedOutUsers());
    for (auto user : {0U, 1U, 2U, 3U, 4U, UINT32_MAX}) {
        for (auto mask : {1U, 2U, 3U, 4U, 7U}) {
            std::fill_n(space.translate(Out - 8), 56, 0xA5);
            assert(invoke(r, "__imp__XamUserGetXUID", {user, mask, Out}) == (user < 4 ? 0x80070525U : 0x80070057U));
            assert(read(space, Out) == 0 && read(space, Out + 4) == 0);
            assert(read(space, Out - 4) == 0xA5A5A5A5U && read(space, Out + 8) == 0xA5A5A5A5U);
        }
        std::fill_n(space.translate(Out - 4), 48, 0xA5);
        assert(invoke(r, "__imp__XamUserGetSigninInfo", {user, 0, Out}) == 0x80070525U);
        for (unsigned i = 0; i < 40; ++i) assert(space.translate(Out)[i] == 0);
        assert(read(space, Out - 4) == 0xA5A5A5A5U && read(space, Out + 40) == 0xA5A5A5A5U);
        std::fill_n(space.translate(Out), 40, 0xA5);
        assert(invoke(r, "__imp__XamUserGetName", {user, Out, 16}) == (user < 4 ? 0x80070525U : 0x80070057U));
        for (unsigned i = 0; i < 40; ++i) assert(space.translate(Out)[i] == 0xA5);
    }
    assert(invoke(r, "__imp__XamUserGetXUID", {0, 1, 0}) == 0x80070057U);
    assert(invoke(r, "__imp__XamUserGetSigninInfo", {0, 0, 0}) == 0x80070057U);
    assert(!call(r, "__imp__XamUserGetXUID", c, {0, 5, Out}).handled());
    for (auto bad : {Out + 1, Module, Title, 0x7FC80000U}) {
        assert(!call(r, "__imp__XamUserGetXUID", c, {0, 1, bad}).handled());
        assert(!call(r, "__imp__XamUserGetSigninInfo", c, {0, 0, bad}).handled());
    }
    for (auto user : {0U, 1U, 2U, 3U, 4U, UINT32_MAX}) {
        std::fill_n(space.translate(Out - 4), 12, 0xA5);
        assert(invoke(r, Privilege, {user, 252, Out}) == (user < 4 ? 1317U : 87U));
        assert(read(space, Out) == 0xA5A5A5A5U && read(space, Out - 4) == 0xA5A5A5A5U
                && read(space, Out + 4) == 0xA5A5A5A5U);
    }
    assert(!call(r, Privilege, c, {255, 252, Out}).handled());
    for (auto bad : {0U, Out + 1, Module, Title, 0x7FC80000U})
        assert(!call(r, Privilege, c, {0, 252, bad}).handled() && c.returnValue == 0xAA);
    constexpr auto DeviceContext = "__imp__XamUserGetDeviceContext";
    constexpr auto VoiceActive = "__imp__XamVoiceIsActiveProcess";
    assert(!call(r, DeviceContext, c, {0, 1, Out}).handled() && c.returnValue == 0xAA);
    assert(!call(r, VoiceActive, c, {}).handled() && c.returnValue == 0xAA);
    assert(r.bindDisconnectedInput());
    for (auto user : {0U, 1U, 3U, 255U, UINT32_MAX}) {
        std::fill_n(space.translate(Out - 4), 12, 0xA5);
        assert(invoke(r, DeviceContext, {user, 1, Out}) == 0x8007048FU);
        assert(read(space, Out) == 0 && read(space, Out - 4) == 0xA5A5A5A5 && read(space, Out + 4) == 0xA5A5A5A5);
    }
    for (auto bad : {0U, Out + 1, UINT32_MAX, Module, Title, 0x7FC80000U, 0x10000000U})
        assert(!call(r, DeviceContext, c, {0, 1, bad}).handled() && c.returnValue == 0xAA);
    assert(invoke(r, VoiceActive, {}) == 0);
    write(space, ProfileIds, 0x10040003);
    write(space, ProfileIds + 4, 0x40080001);
    write(space, ProfileIds + 8, 0x600C0002);
    std::fill_n(space.translate(ProfileSize - 4), 12, 0xA5);
    write(space, ProfileSize, 0);
    assert(profile(0).handled() && c.returnValue == 122 && read(space, ProfileSize) == 148);
    assert(read(space, ProfileSize - 4) == 0xA5A5A5A5 && read(space, ProfileSize + 4) == 0xA5A5A5A5);
    assert(profile(0).handled() && c.returnValue == 87); // Nonzero size, null buffer.
    std::fill_n(space.translate(ProfileBuffer), 256, 0xA5);
    write(space, ProfileSize, 4);
    assert(profile(ProfileBuffer).handled() && c.returnValue == 122 && read(space, ProfileSize) == 4);
    write(space, ProfileSize, 148); write(space, ProfileStack + 0x54, 0);
    assert(profile(ProfileBuffer).handled() && c.returnValue == 1317);
    for (unsigned i = 0; i < 256; ++i) assert(space.translate(ProfileBuffer)[i] == 0xA5);
    write(space, ProfileStack + 0x54, Out + 0x900);
    assert(!profile(ProfileBuffer).handled() && c.returnValue == 0xAA);
    c.arguments[6] = ProfileIds;
    assert(!r.dispatch(ProfileRead, c).handled());
    c.arguments[6] = Module;
    assert(!r.dispatch(ProfileRead, c).handled());
    c.arguments[6] = ProfileSize; c.arguments[4] = 33;
    assert(r.dispatch(ProfileRead, c).handled() && c.returnValue == 87);
    c.arguments[4] = 3; c.arguments[2] = 1; c.returnValue = 0xAA;
    assert(!r.dispatch(ProfileRead, c).handled() && c.returnValue == 0xAA);
    constexpr auto MusicStatus = "__imp__XMsgInProcessCall";
    const auto musicFill = [&] {
        write(space, Out + 0x300, 2);
        write(space, Out + 0x304, Out + 0x400);
        std::fill_n(space.translate(Out + 0x3FC), 12, 0xA5);
    };
    for (auto length : {0U, 8U}) {
        musicFill();
        assert(invoke(r, MusicStatus, {0xFA, 0x70009, Out + 0x300, length}) == 0);
        assert(read(space, Out + 0x300) == 2 && read(space, Out + 0x304) == Out + 0x400);
        assert(read(space, Out + 0x400) == 0);
        assert(read(space, Out + 0x3FC) == 0xA5A5A5A5 && read(space, Out + 0x404) == 0xA5A5A5A5);
    }
    for (const auto& args : {std::array<std::uint32_t,4>{0xFB,0x70009,Out+0x300,0},
            {0xFA,0x70008,Out+0x300,0}, {0xFA,0x70009,Out+0x300,4},
            {0xFA,0x70009,Out+0x301,0}, {0xFA,0x70009,0,0},
            {0xFA,0x70009,0xFFFFFFFCU,0}}) {
        musicFill();
        assert(!call(r, MusicStatus, c, {args[0],args[1],args[2],args[3]}).handled());
        assert(c.returnValue == 0xAA && read(space, Out + 0x400) == 0xA5A5A5A5);
    }
    for (auto output : {0U, Out+0x401, Out+0x300, Out+0x304, Module, Title, 0x7FC80000U, 0xFFFFFFFEU}) {
        musicFill(); write(space, Out + 0x304, output);
        assert(!call(r, MusicStatus, c, {0xFA,0x70009,Out+0x300,0}).handled());
        assert(c.returnValue == 0xAA && read(space, Out + 0x400) == 0xA5A5A5A5);
    }
    musicFill(); write(space, Out + 0x300, 0);
    assert(!call(r, MusicStatus, c, {0xFA,0x70009,Out+0x300,0}).handled());
    assert(c.returnValue == 0xAA && read(space, Out + 0x400) == 0xA5A5A5A5);
    write(space, Out + 0x100, Out + 0x200);
    write(space, Out + 0x104, 0);
    write(space, Out + 0x200, 0xA5A5A5A5);
    assert(invoke(r, "__imp__XMsgStartIORequest", {0xFE, 0x22005, 0, Out + 0x100, 8}) == 0x80004005U);
    assert(read(space, Out + 0x200) == 0xA5A5A5A5);
    for (const auto& args : {std::array<std::uint32_t, 5>{0xFB, 0x22005, 0, Out + 0x100, 8},
            {0xFE, 0x22006, 0, Out + 0x100, 8}, {0xFE, 0x22005, Out, Out + 0x100, 8},
            {0xFE, 0x22005, 0, Out + 0x101, 8}, {0xFE, 0x22005, 0, Out + 0x100, 4}}) {
        assert(!call(r, "__imp__XMsgStartIORequest", c, {args[0], args[1], args[2], args[3], args[4]}).handled());
        assert(c.returnValue == 0xAA && read(space, Out + 0x200) == 0xA5A5A5A5);
    }
    for (const auto output : {0U, Out + 0x201, Module, Title, Out + 0x100, 0xFFFFFFFEU}) {
        write(space, Out + 0x100, output);
        assert(!call(r, "__imp__XMsgStartIORequest", c, {0xFE, 0x22005, 0, Out + 0x100, 8}).handled());
    }
    write(space, Out + 0x100, Out + 0x200);
    write(space, Out + 0x104, Out);
    assert(!call(r, "__imp__XMsgStartIORequest", c, {0xFE, 0x22005, 0, Out + 0x100, 8}).handled());
    assert(invoke(r, "__imp__XexLoadImage", {Name, 9, 0, Out}) == 0 && read(space, Out) == Module);
    assert(space.translate(Module)[0x41] == 2);
    std::memcpy(space.translate(Name), "XAM.XEX", 8);
    assert(invoke(r, "__imp__XexGetModuleHandle", {Name, Out}) == 0 && read(space, Out) == Module);
    assert(invoke(r, "__imp__XexGetModuleHandle", {0, Out}) == 0 && read(space, Out) == Title);
    assert(space.translate(Module)[0x41] == 2); // Lookup does not add a load.
    for (auto ordinal : {0xAFFU, 0xB00U, 0xB0BU, 0xB10U, 0x305U, 0x30BU, 0x50U, 0x24U,
                         0x318U, 0x31AU, 0x48CU}) {
        std::fill_n(space.translate(Out - 4), 12, 0xA5);
        assert(invoke(r, "__imp__XexGetProcedureAddress", {Module, ordinal, Out}) == 0xC0000263U);
        assert(invoke(r, "__imp__RtlNtStatusToDosError", {0xC0000263}) == 127);
        assert(read(space, Out) == 0);
        assert(read(space, Out - 4) == 0xA5A5A5A5 && read(space, Out + 4) == 0xA5A5A5A5);
    }
    assert(invoke(r, "__imp__RtlNtStatusToDosError", {0}) == 0);
    assert(invoke(r, "__imp__RtlNtStatusToDosError", {0xC0000008}) == 6);
    assert(invoke(r, "__imp__RtlNtStatusToDosError", {0xC000000D}) == 87);
    assert(invoke(r, "__imp__RtlNtStatusToDosError", {0xC000000F}) == 2);
    assert(!call(r, "__imp__RtlNtStatusToDosError", c, {0xDEADBEEF}).handled());
    for (auto ordinal : {0U, 1U, Name}) {
        assert(!call(r, "__imp__XexGetProcedureAddress", c, {Module, ordinal, Out}).handled());
    }
    assert(!call(r, "__imp__XexGetProcedureAddress", c, {Title, 1, Out}).handled());
    assert(invoke(r, "__imp__XexGetProcedureAddress", {0x1234, 1, Out}) == 0xC0000008U);
    for (auto bad : {Out + 1, Module, Title, Module + 0x40, 0U}) {
        assert(!call(r, "__imp__XexLoadImage", c, {Name, 9, 0, bad}).handled());
        assert(!call(r, "__imp__XexGetProcedureAddress", c, {Module, 0xAFF, bad}).handled());
    }
    assert(!call(r, "__imp__XexLoadImage", c, {Name, 0, 0, Out}).handled());
    assert(!call(r, "__imp__XexLoadImage", c, {Name, 9, 1, Out}).handled());
    std::memcpy(space.translate(Name), "other.xex", 10);
    assert(!call(r, "__imp__XexLoadImage", c, {Name, 9, 0, Out}).handled());
    assert(!call(r, "__imp__XexGetModuleHandle", c, {Name, Out}).handled());
    std::memcpy(space.translate(Name), "xam.xex", 8);
    assert(invoke(r, "__imp__XexUnloadImage", {Module}) == 0 && space.translate(Module)[0x41] == 2);
    assert(invoke(r, "__imp__XexUnloadImage", {0x1234}) == 0xC0000008U);
    *space.translate(Module + 0x40) = 1;
    assert(!call(r, "__imp__XexLoadImage", c, {Name, 9, 0, Out}).handled());
    *space.translate(Module + 0x40) = 0;
    for (unsigned count = 2; count < UINT16_MAX; ++count) { assert(invoke(r, "__imp__XexLoadImage", {Name, 9, 0, Out}) == 0); }
    assert(!call(r, "__imp__XexLoadImage", c, {Name, 9, 0, Out}).handled());
    r.reset();
    for (unsigned i = 0; i < GuestImportRegistry::NativeModuleRecordSize; ++i) { assert(space.translate(Module)[i] == 0); }
}
