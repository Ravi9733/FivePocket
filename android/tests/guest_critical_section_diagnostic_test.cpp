#include "guest_import_registry.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
using namespace recomp::bootstrap;
int main() {
    GuestAddressSpace s;assert(s.reserve());GuestImportRegistry r;assert(r.initialize(s,0x10000000,0x10000));
    constexpr std::uint32_t Lock=0x5000;
    const auto call=[&](const char* name,std::uint32_t address) {
        GuestImportCall c;c.arguments[0]=0xFEDCBA9800000000ULL|address;c.returnValue=0x123456789ABCDEF0ULL;
        const auto result=r.dispatch(name,c);assert(c.returnValue==0x123456789ABCDEF0ULL);return result;
    };
    assert(!r.lastCriticalSectionRejection());assert(call("__imp__RtlInitializeCriticalSection",Lock).handled());
    const auto reject=[&](std::uint32_t address,unsigned reason) {
        std::array<std::uint8_t,28> before;std::copy_n(s.base()+Lock,28,before.begin());
        assert(call("__imp__RtlEnterCriticalSection",address).state==GuestImportDispatchState::Rejected);
        const auto state=r.lastCriticalSectionRejection();assert(state && (*state)[0]==reason && (*state)[1]==address);
        assert(std::equal(before.begin(),before.end(),s.base()+Lock));return *state;
    };
    auto state=reject(Lock,1);assert(state[2]==0 && state[3]==1 && state[6]==1 && state[7]==0x01000000 && state[11]==UINT32_MAX && state[15]==1);
    assert(r.bindGuestThreadToken(1));state=reject(Lock+128,2);assert(!state[3] && state[6] && state[14] && !state[15]);
    assert(call("__imp__RtlEnterCriticalSection",Lock).handled());assert(!r.lastCriticalSectionRejection());
    assert(r.bindGuestThreadToken(2));state=reject(Lock,4);
    assert(state[2]==2 && state[3]==1 && state[4]==1 && state[5]==1 && state[15]);
    assert(state[11]==0 && state[12]==1 && state[13]==1 && !state[14]);
    s.base()[Lock+1]=0xAA;state=reject(Lock,3);assert(state[7]==0x01AA0000 && !state[15]);
    for(auto bad:{0U,0xFFFFFFFFU,0x7FC80000U}) {state=reject(bad,2);assert(!state[6]);
        assert(std::all_of(state.begin()+7,state.end(),[](auto v){return v==0;}));}
    r.reset();assert(!r.lastCriticalSectionRejection());
    std::puts("CRITICAL_SECTION_DIAGNOSTIC_V1 reasons=4 no_guest_writes=1 bounded_snapshot=1");
}
