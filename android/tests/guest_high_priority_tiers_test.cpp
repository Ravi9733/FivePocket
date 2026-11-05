#include "guest_import_registry.h"
#include "guest_cooperative_executor.h"
#include "guest_thread_priority.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <stdexcept>
using namespace recomp::bootstrap;
namespace {
constexpr std::uint32_t Parent=0x63001000,Code=0x82001000,Out=0x30000000;
constexpr auto Set="__imp__KeSetBasePriorityThread",Query="__imp__KeQueryBasePriorityThread";
void put(GuestAddressSpace& s,std::uint32_t a,std::uint32_t v){for(unsigned i=0;i<4;++i)s.base()[a+i]=static_cast<std::uint8_t>(v>>(24-i*8));}
std::uint32_t get(GuestAddressSpace& s,std::uint32_t a){auto* b=s.base()+a;return (std::uint32_t{b[0]}<<24)|(std::uint32_t{b[1]}<<16)|(std::uint32_t{b[2]}<<8)|b[3];}
GuestImportDispatchResult call(GuestImportRegistry& r,const char* name,GuestImportCall& c,std::initializer_list<std::uint32_t> args){
    c={};std::copy(args.begin(),args.end(),c.arguments.begin());c.returnValue=0xABCD12345678ULL;return r.dispatch(name,c);
}
std::uint64_t invoke(GuestImportRegistry& r,const char* name,std::initializer_list<std::uint32_t> args){
    GuestImportCall c;assert(call(r,name,c,args).handled());return c.returnValue;
}
void registry(GuestAddressSpace& s,unsigned failure,std::uint32_t high) {
    GuestImportRegistry r;GuestCooperativeExecutor e;
    assert(!r.bindNativeHighPriorityTiers());assert(r.initialize(s,0x10000000,0x10000));
    assert(r.bindGuestThreadToken(Parent));put(s,Parent+0x14C,1);assert(r.bindInitialThreadObject(Parent,1));
    assert(r.bindThreadStorageArena({{0x64000000,0x100000},{0x82000000,0x10000},{Code,0x1000},0,0,32,4,0x4000,2},{},[](auto p){return p==Code;}));
    bool ran=false;std::int32_t resumedPriority=0;unsigned batches=0,mode=failure;
    assert(r.bindThreadResumeHandler([&](const auto& thread,auto priority){
        resumedPriority=priority;return e.enqueue(thread.layout.threadAddress,[&,token=thread.layout.threadAddress]{
            assert(invoke(r,Query,{token})==high);ran=true;assert(r.markGuestThreadReturned(token,0));
        },priority);
    }));
    assert(r.bindThreadWaitHandler([&](const auto& ready){return e.waitUntil(ready);}));
    assert(!r.bindNativeHighPriorityTiers());
    assert(r.bindThreadPriorityHandler([&](auto token,auto priority){
        if(priority==3 || priority==4){++batches;if(mode==1)return false;if(mode==2)throw std::runtime_error("injected priority rejection");}
        return e.setPriority(token,priority);
    }));
    GuestImportCall c;assert(!call(r,Set,c,{Parent,high}).handled()); // Default remains rejected.
    assert(r.bindNativeHighPriorityTiers());assert(!r.bindNativeHighPriorityTiers());
    const auto result=e.run(Parent,[&] {
        assert(invoke(r,Set,{Parent,2})==0);
        if(mode) {
            bool caught=false;try{assert(!call(r,Set,c,{Parent,high}).handled());}catch(const std::runtime_error&){caught=true;}
            assert(caught==(mode==2) && c.returnValue==0xABCD12345678ULL);
            assert(invoke(r,Query,{Parent})==2 && !r.nativeHighPriorityTiersRequests());mode=0;
        }
        assert(invoke(r,Set,{Parent,high})==2);assert(invoke(r,Query,{Parent})==high);
        const auto accepted=batches;assert(invoke(r,Set,{Parent,high})==high);assert(batches==accepted);
        assert(r.nativeHighPriorityTiersRequests()==2);assert(invoke(r,Set,{Parent,2})==high);
        c={};c.arguments[0]=Parent;c.arguments[1]=0xFEDCBA9800000000ULL|high;
        assert(r.dispatch(Set,c).handled() && c.returnValue==2 && r.nativeHighPriorityTiersRequests()==3);
        for(auto bad:{5U,15U,0xFFFFFFFDU,0xFFFFFFF1U}){
            assert(!call(r,Set,c,{Parent,bad}).handled());assert(invoke(r,Query,{Parent})==high);
        }
        assert(invoke(r,Set,{Parent,17})==high);assert(invoke(r,Query,{Parent})==16);
        assert(invoke(r,Set,{Parent,0})==16);
        assert(invoke(r,"__imp__ExCreateThread",{Out,0,Out+4,0,Code,0,1})==0);
        const auto handle=get(s,Out);const auto worker=r.lookupSuspendedThread(handle);assert(worker);
        const auto token=worker->layout.threadAddress;
        const auto before=batches;assert(invoke(r,Set,{token,high})==0);assert(batches==before);
        assert(invoke(r,Query,{token})==high && r.nativeHighPriorityTiersRequests()==4);
        assert(invoke(r,"__imp__NtResumeThread",{handle,Out+8})==0 && resumedPriority==static_cast<int>(high) && !ran);
        assert(e.waitUntil([&]{return ran;}));
        assert(!call(r,Set,c,{token,high}).handled() && r.nativeHighPriorityTiersRequests()==4);
        put(s,Parent+0x14C,99);assert(!call(r,Set,c,{Parent,high}).handled());put(s,Parent+0x14C,1);
        assert(invoke(r,Set,{Parent,3})==0);assert(invoke(r,Set,{Parent,4})==3);
        assert(invoke(r,Query,{Parent})==4);assert(invoke(r,Set,{Parent,3})==4);
        assert(r.nativeHighPriorityTiersRequests()==7); // Equal native weights retain distinct guest metadata.
    },{200,4,2,true},[&](auto token){assert(r.bindGuestThreadToken(token));});
    assert(result.execution.completed() && ran);r.reset();assert(!r.nativeHighPriorityTiersRequests());
}
void scheduling() {
    GuestCooperativeExecutor e;
    for(int high:{3,4})for(bool workerHigh:{false,true}) {
        std::array<int,2> counts{};
        const auto result=e.run(1,[&] {
            assert(e.setPriority(1,workerHigh?0:high));
            assert(e.enqueue(2,[&]{for(;;)GuestExecutionGuard::checkpoint("worker");},workerHigh?high:0));
            for(;;){assert(e.setPriority(1,workerHigh?0:high));GuestExecutionGuard::checkpoint("main");}
        },{4001,4,2,true},[](auto){},[&](auto label){++counts[label=="main"?0:1];});
        assert(result.execution.reason==GuestExecutionStopReason::FunctionEntryBudgetExceeded);
        const auto error=workerHigh?counts[1]-counts[0]*8:counts[0]-counts[1]*8;
        assert(error>=-160 && error<=160 && counts[0]>0 && counts[1]>0);
    }
    const auto reset=e.run(1,[&]{for(int high:{3,4}){assert(!e.setPriority(1,high));assert(!e.enqueue(2,[]{},high));}},{100,4,2},[](auto){});
    assert(reset.execution.completed()); // Opt-in does not leak into the next run.
}
void boostCap(int high) {
    GuestCooperativeExecutor e;bool parked=false,ready=false,resumed=false;unsigned steps=0;
    std::vector<unsigned> seen;const std::uint32_t token=2;
    const auto result=e.run(1,[&]{
        assert(e.enqueue(token,[&]{
            parked=true;assert(e.waitUntil([&]{return ready;}));resumed=true;
            for(unsigned i=0;i<96;++i){GuestExecutionGuard::checkpoint("boosted_plus3");++steps;}
        },high));
        GuestExecutionGuard::checkpoint("enqueued");assert(e.waitUntil([&]{return parked;}));
        assert(e.boostEventWaiters(&token,1));ready=true;
        while(steps<96){GuestExecutionGuard::checkpoint("parent");if(resumed)seen.push_back(steps);}
    },{1000,4,2,true},[](auto){});
    assert(result.execution.completed() && !seen.empty() && seen.front()==31);
    std::vector<unsigned> rotations;for(auto value:seen)if(rotations.empty() || rotations.back()!=value)rotations.push_back(value);
    assert(rotations.size()>2 && rotations[1]-rotations[0]==32); // Both boosted and decayed slices stay32, not64.
}
}
int main(){
    for(int raw=-64;raw<=64;++raw){
        const auto baseline=normalizePortableThreadPriority(raw),extended=normalizePortableThreadPriority(raw,true);
        if(raw==3 || raw==4)assert(!baseline && extended==raw);else assert(baseline==extended);
        assert(validPortableSchedulerPriority(raw,true)==(validPortableSchedulerPriority(raw)||raw==3||raw==4));
    }
    assert(portablePrioritySliceShift(3)==3 && portablePrioritySliceShift(4)==3);
    GuestAddressSpace s;assert(s.reserve());for(unsigned high:{3U,4U})for(unsigned failure=0;failure<3;++failure)registry(s,failure,high);
    scheduling();boostCap(3);boostCap(4);std::puts("HIGH_PRIORITY_TIERS_V1 registry_cases=6 weighted_fairness=4 boost_cap=2 suspended_resume=1 failure_atomic=1 default_unchanged=1");
}
