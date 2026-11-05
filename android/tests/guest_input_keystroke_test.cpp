#include "guest_import_registry.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>

using namespace recomp::bootstrap;

int main() {
    GuestAddressSpace space;
    assert(space.reserve());
    GuestImportRegistry registry;
    assert(!registry.bindDisconnectedInput());
    GuestImportCall unavailable{};
    assert(registry.dispatch("__imp__XamInputGetKeystrokeEx", unavailable).state ==
           GuestImportDispatchState::RegistryUnavailable);
    assert(registry.initialize(space,0x10000000,0x10000));
    std::array<std::uint8_t,64> expected;
    expected.fill(0xA5);
    std::copy(expected.begin(),expected.end(),space.base()+0x1000);
    std::uint64_t checks=0;
    for (std::uint32_t low=0;low<256;++low) {
        for (std::uint32_t high:{0U,0x40000000U,0x80000100U,0xFFFFFF00U}) {
            for (std::uint32_t user:{0U,0x1000U,0xFFFFFFFFU}) {
                for (std::uint32_t output:{0U,0x1010U,0xFFFFFFFFU}) {
                    GuestImportCall call{};
                    // Upper register halves never change the Xbox32 ABI.
                    call.arguments={0xFEDCBA9800000000ULL|user,
                                    0xABCDEF0100000000ULL|high|low,
                                    0x1234567800000000ULL|output,3,4,5,6,7};
                    const auto arguments=call.arguments;
                    call.stackPointer=0x70000000;
                    call.returnValue=0x1122334455667788ULL;
                    const auto result=registry.dispatch("__imp__XamInputGetKeystrokeEx",call);
                    if (!output) {
                        assert(result.handled() && call.returnValue==0xA0);
                    } else if (low && !(low&1)) {
                        assert(result.handled() && call.returnValue==0x48F);
                    } else {
                        assert(result.state==GuestImportDispatchState::Rejected);
                        assert(call.returnValue==0x1122334455667788ULL);
                    }
                    assert(call.arguments==arguments && call.stackPointer==0x70000000);
                    assert(std::equal(expected.begin(),expected.end(),space.base()+0x1000));
                    ++checks;
                }
            }
        }
    }
    // Zero/border pointers above are never dereferenced on these early exits.
    GuestImportCall state{};
    state.returnValue=0x1122334455667788ULL;
    assert(registry.dispatch("__imp__XamInputGetState",state).state == GuestImportDispatchState::Rejected);
    assert(state.returnValue==0x1122334455667788ULL);
    assert(registry.bindDisconnectedInput());
    assert(!registry.bindDisconnectedInput());
    std::uint64_t stateChecks=0;
    for (std::uint32_t user:{0U,1U,2U,3U,4U,255U,0xFFFFFFFFU}) {
        for (std::uint32_t flags:{0U,1U,2U,0x40000001U,0xFFFFFFFFU}) {
            for (std::uint32_t output:{0U,0x1010U,0xFFFFFFFFU}) {
                state.arguments={0xFEDCBA9800000000ULL|user,0xABCDEF0100000000ULL|flags,
                                 0x1234567800000000ULL|output,3,4,5,6,7};
                const auto arguments=state.arguments;
                assert(registry.dispatch("__imp__XamInputGetState",state).handled());
                assert(state.returnValue==0x48F && state.arguments==arguments);
                assert(std::equal(expected.begin(),expected.end(),space.base()+0x1000));
                ++stateChecks;
            }
        }
    }
    registry.reset();
    assert(!registry.bindDisconnectedInput());
    assert(registry.initialize(space,0x10000000,0x10000));
    assert(registry.dispatch("__imp__XamInputGetState",state).state == GuestImportDispatchState::Rejected);
    registry.reset();
    assert(registry.dispatch("__imp__XamInputGetKeystrokeEx",unavailable).state ==
           GuestImportDispatchState::RegistryUnavailable);
    assert(registry.initialize(space,0x10000000,0x10000));
    bool connected=true;
    const auto reader=[&](std::uint32_t user,recomp::input::GamepadState& out){
        if(user || !connected)return 0;
        out={0x12345678,0x1021,128,255,-32768,32767,-1,1};return 1;
    };
    assert(registry.bindNativeGamepad(reader,reader));
    assert(!registry.bindDisconnectedInput());
    state.arguments={0,0,0x1010};
    assert(registry.dispatch("__imp__XamInputGetState",state).handled() && state.returnValue==0);
    const std::array<std::uint8_t,16> actualState{0x12,0x34,0x56,0x78,0x10,0x21,128,255,128,0,127,255,255,255,0,1};
    assert(std::equal(actualState.begin(),actualState.end(),space.base()+0x1010));
    state.arguments[2]=0;assert(registry.dispatch("__imp__XamInputGetState",state).handled() && state.returnValue==0);
    state.arguments[2]=0xFFFFFFFF;assert(registry.dispatch("__imp__XamInputGetState",state).state==GuestImportDispatchState::GuestMemoryFault);
    state.arguments={0,0,0x1020};
    assert(registry.dispatch("__imp__XamInputGetCapabilities",state).handled() && state.returnValue==0);
    assert(space.base()[0x1020]==1 && space.base()[0x1021]==1 && space.base()[0x1022]==0 && space.base()[0x1023]==0);
    assert(std::equal(actualState.begin()+4,actualState.end(),space.base()+0x1024));
    assert(space.base()[0x1030]==0 && space.base()[0x1033]==0);
    state.arguments[2]=0x1030;
    assert(registry.dispatch("__imp__XamInputSetState",state).handled() && state.returnValue==0);
    space.base()[0x1030]=1;
    assert(registry.dispatch("__imp__XamInputSetState",state).handled() && state.returnValue==50);
    connected=false;state.arguments[2]=0x1010;
    assert(registry.dispatch("__imp__XamInputGetState",state).handled() && state.returnValue==0x48F);
    assert(std::equal(actualState.begin(),actualState.end(),space.base()+0x1010));
    registry.reset();
    std::puts("NATIVE_INPUT_STATE_CAPABILITIES_PASS");
    std::printf("INPUT_DISCONNECTED_V1 filter_checks=%llu state_checks=%llu guest_writes=0 connected_devices=0\n",
                static_cast<unsigned long long>(checks),static_cast<unsigned long long>(stateChecks));
}
