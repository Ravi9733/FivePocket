#include "guest_import_registry.h"
#include "guest_cooperative_executor.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace recomp::bootstrap;
namespace {
constexpr std::uint32_t Parent=0x63001000,Code=0x82001000,Scratch=0x30000000;
void put(GuestAddressSpace& s,std::uint32_t a,std::uint32_t v) {for(unsigned i=0;i<4;++i)s.base()[a+i]=static_cast<std::uint8_t>(v>>(24-i*8));}
std::uint32_t get(GuestAddressSpace& s,std::uint32_t a) {const auto* b=s.base()+a;return (std::uint32_t{b[0]}<<24)|(std::uint32_t{b[1]}<<16)|(std::uint32_t{b[2]}<<8)|b[3];}
GuestImportDispatchResult call(GuestImportRegistry& r,const char* name,GuestImportCall& c,std::initializer_list<std::uint32_t> args) {
    c={};std::copy(args.begin(),args.end(),c.arguments.begin());c.returnValue=0xDEADBEEF;return r.dispatch(name,c);
}
std::uint32_t invoke(GuestImportRegistry& r,const char* name,std::initializer_list<std::uint32_t> args) {
    GuestImportCall c;assert(call(r,name,c,args).handled());return static_cast<std::uint32_t>(c.returnValue);
}
void testRegistry(GuestAddressSpace& s,bool automatic,bool multiple,bool disabled,unsigned failMode) {
    GuestImportRegistry r;GuestCooperativeExecutor e;
    assert(!r.bindNativeEventWakeBoostHandler([](auto,auto){return true;}));
    assert(r.initialize(s,0x10000000,0x10000));assert(r.bindGuestThreadToken(Parent));
    assert(r.bindThreadStorageArena({{0x64000000,0x100000},{0x82000000,0x10000},{Code,0x1000},0,0,32,4,0x4000,2}, {}, [](auto a){return a==Code;}));
    assert(r.bindThreadResumeHandler([](const auto&,auto){return false;}));
    assert(r.bindThreadWaitHandler([&](const auto& ready){return e.waitUntil(ready);}));
    put(s,Scratch,0);put(s,Scratch+4,4096);
    assert(invoke(r,"__imp__NtAllocateVirtualMemory",{Scratch,Scratch+4,0x3000,4,0})==0);
    const auto event=get(s,Scratch),other=event+32;
    for(auto a:{event,other}) {put(s,a,(automatic?0x01000000:0)|0x88050);put(s,a+4,0);put(s,a+8,a+8);put(s,a+12,a+8);}
    put(s,Scratch+16,other);put(s,Scratch+20,event);
    GuestImportCall c;
    assert(!call(r,"__imp__KeSetEvent",c,{event,1,0}).handled());
    bool reject=failMode!=0;unsigned batches=0;std::vector<std::uint32_t> boosted;
    assert(r.bindNativeEventWakeBoostHandler([&](const auto* tokens,auto count) {
        ++batches;if(reject) {if(failMode==2)throw std::runtime_error("fixture boost failure");return false;}
        // Fixed production callback is allocation-free; this recording fixture
        // deliberately records before the atomic executor operation.
        boosted.assign(tokens,tokens+count);return e.boostEventWaiters(tokens,count);
    }));
    assert(!r.bindNativeEventWakeBoostHandler([](auto,auto){return true;}));
    for(auto inc:{2U,16U,UINT32_MAX}) assert(!call(r,"__imp__KeSetEvent",c,{event,inc,0}).handled());
    assert(!call(r,"__imp__KeSetEvent",c,{event,1,1}).handled());
    // No queued waiter means no scheduler boost; repeated signaled Set stays1.
    assert(invoke(r,"__imp__KeSetEvent",{event,1,0})==0);
    assert(invoke(r,"__imp__KeSetEvent",{event,1,0})==1);assert(batches==0);
    assert(invoke(r,"__imp__KeResetEvent",{event})==1);
    unsigned parked=0,woke=0;std::array<std::uint32_t,2> tokens{};
    const auto outcome=e.run(Parent,[&] {
        for(unsigned i=0;i<2;++i) {
            const auto worker=r.createNativeCallbackWorker(Code,0,[&,i](const auto& thread) {
                tokens[i]=thread.layout.threadAddress;
                return e.enqueue(tokens[i],[&,token=tokens[i]] {
                    if(disabled) assert(invoke(r,"__imp__KeSetDisableBoostThread",{token,1})==0);
                    ++parked;
                    if(multiple) assert(invoke(r,"__imp__KeWaitForMultipleObjects",{2,Scratch+16,1,3,1,0,0,0})==1);
                    else assert(invoke(r,"__imp__KeWaitForSingleObject",{event,3,1,0,0})==0);
                    ++woke;assert(r.markGuestThreadReturned(token,0));
                });
            });assert(worker);
        }
        assert(e.waitUntil([&]{return parked==2;}));assert(woke==0);
        if(reject && !disabled) {
            bool caught=false;
            try {assert(call(r,"__imp__KeSetEvent",c,{event,1,0}).state==GuestImportDispatchState::Rejected);}
            catch(const std::runtime_error&) {caught=true;}
            assert(caught==(failMode==2) && get(s,event+4)==0 && woke==0);
            assert(r.nativeEventWakeBoostStatistics().signalRequests==2);
            reject=false;
        }
        assert(invoke(r,"__imp__KeSetEvent",{event,1,0})==0);
        assert(invoke(r,"__imp__KeResetEvent",{event})==(automatic?0U:1U));
        assert(e.waitUntil([&]{return woke==(automatic?1U:2U);}));
        if(automatic) {assert(invoke(r,"__imp__KeSetEvent",{event,1,0})==0);assert(e.waitUntil([&]{return woke==2;}));}
    },{200,4,3},[&](auto token){assert(r.bindGuestThreadToken(token));});
    assert(outcome.execution.completed() && woke==2);
    const auto stats=r.nativeEventWakeBoostStatistics();
    assert(stats.signalRequests==(automatic?4U:3U));
    assert(stats.threadsBoosted==(disabled?0U:2U));assert(stats.threadsSuppressed==(disabled?2U:0U));
    assert(batches==(disabled?0U:(automatic?2U:1U)+(failMode?1U:0U)));
    if(!disabled) {assert(boosted.size()==(automatic?1U:2U));assert(boosted.back()==tokens[1]);}
    r.reset();assert(r.nativeEventWakeBoostStatistics().signalRequests==0);
}
void testSlice(bool valid) {
    GuestCooperativeExecutor e;std::uint32_t token=2;
    assert(!e.boostEventWaiters(&token,1));
    bool parked=false,ready=false,resumed=false;unsigned workerSteps=0;
    std::vector<unsigned> seen;
    const auto result=e.run(1,[&] {
        assert(e.enqueue(2,[&] {
            parked=true;assert(e.waitUntil([&]{return ready;}));resumed=true;
            for(unsigned i=0;i<40;++i) {GuestExecutionGuard::checkpoint("boost_worker");++workerSteps;}
        }));
        GuestExecutionGuard::checkpoint("after_enqueue"); // Consume forced reschedule.
        assert(e.waitUntil([&]{return parked;}));
        assert(!e.boostEventWaiters(nullptr,1));assert(!e.boostEventWaiters(&token,0));
        const std::array<std::uint32_t,2> missing{2,999},duplicate{2,2};
        assert(!e.boostEventWaiters(missing.data(),2));assert(!e.boostEventWaiters(duplicate.data(),2));
        if(valid) assert(e.boostEventWaiters(&token,1));
        ready=true;
        while(workerSteps<40) {GuestExecutionGuard::checkpoint("boost_main");if(resumed)seen.push_back(workerSteps);}
    },{400,4,2},[](auto){});
    assert(result.execution.completed() && seen.size()>8);
    // Worker increments occur after its checkpoint returns, so first rotation
    // is one below the full slice. Later slices decay to normal four entries.
    assert(seen.front()==(valid?7U:3U));
    std::vector<unsigned> rotations;for(auto n:seen)if(rotations.empty()||rotations.back()!=n)rotations.push_back(n);
    assert(rotations.size()>3 && rotations[1]-rotations[0]==4 && rotations[2]-rotations[1]==4);
}
void testReparkClearsBoost() {
    GuestCooperativeExecutor e;bool parked=false,first=false,reparked=false,second=false;unsigned steps=0;
    std::vector<unsigned> seen;const std::uint32_t token=2;
    const auto result=e.run(1,[&] {
        assert(e.enqueue(2,[&] {
            parked=true;assert(e.waitUntil([&]{return first;}));
            reparked=true;assert(e.waitUntil([&]{return second;}));
            for(unsigned i=0;i<16;++i) {GuestExecutionGuard::checkpoint("repark_worker");++steps;}
        }));
        GuestExecutionGuard::checkpoint("enqueue_repark_worker");
        assert(e.waitUntil([&]{return parked;}));assert(e.boostEventWaiters(&token,1));first=true;
        assert(e.waitUntil([&]{return reparked;}));second=true;
        while(steps<16) {GuestExecutionGuard::checkpoint("repark_main");seen.push_back(steps);}
    },{200,4,2},[](auto){});
    assert(result.execution.completed() && seen.front()==0);
    const auto nonzero=std::find_if(seen.begin(),seen.end(),[](auto n){return n!=0;});
    assert(nonzero!=seen.end() && *nonzero==3); // New wait clears previous boost.
}
}
int main() {
    GuestAddressSpace s;assert(s.reserve());
    for(bool automatic:{false,true})for(bool multiple:{false,true})for(bool disabled:{false,true})
        for(unsigned fail=0;fail<3;++fail)testRegistry(s,automatic,multiple,disabled,fail);
    testSlice(false);testSlice(true);testReparkClearsBoost();
    std::puts("EVENT_WAKE_BOOST_V1 registry_cases=24 atomic_batch=1 decay=1 disabled_honored=1");
}
