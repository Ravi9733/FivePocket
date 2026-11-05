#include "guest_import_registry.h"
#include "guest_cooperative_executor.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <vector>
using namespace recomp::bootstrap;
namespace {thread_local int allocationFailureCountdown=-1;}
void* operator new(std::size_t bytes) {
    if(allocationFailureCountdown==0) {allocationFailureCountdown=-1;throw std::bad_alloc();}
    if(allocationFailureCountdown>0)--allocationFailureCountdown;
    if(auto* p=std::malloc(bytes?bytes:1))return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) {return ::operator new(bytes);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
namespace {
constexpr std::uint32_t Parent=0x63001000,Code=0x82001000,Scratch=0x30000000;
constexpr auto Enter="__imp__RtlEnterCriticalSection",Leave="__imp__RtlLeaveCriticalSection";
void put(GuestAddressSpace& s,std::uint32_t a,std::uint32_t v) {for(unsigned i=0;i<4;++i)s.base()[a+i]=static_cast<std::uint8_t>(v>>(24-i*8));}
std::uint32_t get(GuestAddressSpace& s,std::uint32_t a) {const auto* b=s.base()+a;return (std::uint32_t{b[0]}<<24)|(std::uint32_t{b[1]}<<16)|(std::uint32_t{b[2]}<<8)|b[3];}
GuestImportDispatchResult call(GuestImportRegistry& r,const char* name,GuestImportCall& c,std::initializer_list<std::uint32_t> args) {
    c={};std::copy(args.begin(),args.end(),c.arguments.begin());c.returnValue=0xFEDCBA9876543210ULL;return r.dispatch(name,c);
}
std::uint64_t invoke(GuestImportRegistry& r,const char* name,std::initializer_list<std::uint32_t> args) {
    GuestImportCall c;const auto result=call(r,name,c,args);
    if(!result.handled())std::fprintf(stderr,"fixture import %s failed: %s\n",name,result.diagnostic.c_str());
    assert(result.handled());return c.returnValue;
}
void lockCall(GuestImportRegistry& r,const char* name,std::uint32_t at) {
    assert(invoke(r,name,{at})==0xFEDCBA9876543210ULL);
}
void setup(GuestAddressSpace& s,GuestImportRegistry& r,GuestImportRegistry::ThreadWaitHandler handler) {
    assert(!r.bindNativeCriticalSectionWaits());
    assert(r.initialize(s,0x10000000,0x10000));assert(r.bindGuestThreadToken(Parent));
    put(s,Parent+0x14C,1);assert(r.bindInitialThreadObject(Parent,1));
    assert(r.bindThreadStorageArena({{0x64000000,0x100000},{0x82000000,0x10000},{Code,0x1000},0,0,32,4,0x4000,3}, {}, [](auto a){return a==Code;}));
    assert(r.bindThreadResumeHandler([](const auto&,auto){return false;}));
    assert(r.bindThreadWaitHandler(std::move(handler)));assert(r.bindNativeCriticalSectionWaits());
    assert(!r.bindNativeCriticalSectionWaits());
}
std::uint32_t allocate(GuestAddressSpace& s,GuestImportRegistry& r,unsigned kind) {
    if(kind==0)return 0x5000;
    if(kind==2) {
        assert(r.bindPhysicalMemoryArena(0xA0000000,0x10000,0));
        return static_cast<std::uint32_t>(invoke(r,"__imp__MmAllocatePhysicalMemory",{0,4096,4}));
    }
    put(s,Scratch,0);put(s,Scratch+4,4096);
    assert(invoke(r,"__imp__NtAllocateVirtualMemory",{Scratch,Scratch+4,0x3000,4,0})==0);
    return get(s,Scratch);
}
void tryFree(GuestAddressSpace& s,GuestImportRegistry& r,unsigned kind,std::uint32_t at,bool success) {
    if(!kind)return;
    GuestImportCall c;GuestImportDispatchResult result;
    if(kind==2)result=call(r,"__imp__MmFreePhysicalMemory",c,{0,at});
    else {put(s,Scratch,at);put(s,Scratch+4,0);result=call(r,"__imp__NtFreeVirtualMemory",c,{Scratch,Scratch+4,0x8000,0});}
    assert(result.handled()==success);if(success && kind==1)assert(c.returnValue==0);
}
void fifo(GuestAddressSpace& s,unsigned kind,bool spin) {
    GuestImportRegistry r;GuestCooperativeExecutor e;
    setup(s,r,[&](const auto& ready){return e.waitUntil(ready);});
    const auto at=allocate(s,r,kind);assert(at);
    if(spin)assert(invoke(r,"__imp__RtlInitializeCriticalSectionAndSpinCount",{at,512})==0);
    else lockCall(r,"__imp__RtlInitializeCriticalSection",at);
    std::array<std::uint8_t,16> header;std::copy_n(s.base()+at,16,header.begin());
    std::array<std::uint32_t,2> tokens{};std::vector<unsigned> order;unsigned parked=0;
    const auto result=e.run(Parent,[&] {
        lockCall(r,Enter,at);lockCall(r,Enter,at);assert(get(s,at+20)==2);
        for(unsigned i=0;i<2;++i) {
            const auto worker=r.createNativeCallbackWorker(Code,0,[&,i](const auto& t) {
                tokens[i]=t.layout.threadAddress;
                return e.enqueue(tokens[i],[&,i] {
                    ++parked;lockCall(r,Enter,at);
                    assert(get(s,at+24)==tokens[i] && get(s,at+20)==1);order.push_back(i);
                    lockCall(r,Enter,at);assert(get(s,at+20)==2);
                    GuestExecutionGuard::checkpoint("owned_recursive_lock");
                    assert(get(s,at+24)==tokens[i]);lockCall(r,Leave,at);lockCall(r,Leave,at);
                    assert(r.markGuestThreadReturned(tokens[i],0));
                });
            });assert(worker);
        }
        assert(e.waitUntil([&]{return parked==2;}));assert(order.empty());
        assert(r.nativeCriticalSectionWaitStatistics()[3]==2);
        lockCall(r,Enter,at);assert(get(s,at+20)==3);lockCall(r,Leave,at);
        lockCall(r,Leave,at);assert(get(s,at+20)==1);
        GuestExecutionGuard::checkpoint("partial_release_does_not_wake");assert(order.empty());
        tryFree(s,r,kind,at,false);
        lockCall(r,Leave,at);assert(get(s,at+24)==0 && get(s,at+16)==UINT32_MAX);
        assert(invoke(r,"__imp__RtlTryEnterCriticalSection",{at})==0); // Cannot steal from queued waiters.
        GuestImportCall c;
        assert(!call(r,"__imp__RtlInitializeCriticalSection",c,{at}).handled());
        assert(!call(r,"__imp__RtlInitializeCriticalSectionAndSpinCount",c,{at,512}).handled());
        tryFree(s,r,kind,at,false); // Also pinned while owner0 but FIFO is reserved.
        // Immediate reacquisition must queue behind the two earlier waiters.
        lockCall(r,Enter,at);order.push_back(2);assert(get(s,at+24)==Parent);
        lockCall(r,Leave,at);
    },{400,2,3},[&](auto token){assert(r.bindGuestThreadToken(token));});
    assert(result.execution.completed());assert((order==std::vector<unsigned>{0,1,2}));
    assert((r.nativeCriticalSectionWaitStatistics()==std::array<std::uint64_t,4>{3,3,0,0}));
    assert(std::equal(header.begin(),header.end(),s.base()+at));
    assert(get(s,at+16)==UINT32_MAX && !get(s,at+20) && !get(s,at+24));
    tryFree(s,r,kind,at,true);r.reset();
    assert((r.nativeCriticalSectionWaitStatistics()==std::array<std::uint64_t,4>{}));
}
void injectedFailure(GuestAddressSpace& s,unsigned mode) {
    GuestImportRegistry r;std::uint32_t token=0;constexpr std::uint32_t at=0x5000;
    setup(s,r,[&](const auto& ready) {
        assert(!ready());
        if(mode>=2) {
            assert(r.bindGuestThreadToken(Parent));lockCall(r,Leave,at);
            assert(ready()); // Polling does not hand ownership to the waiter.
            assert(get(s,at+24)==0 && get(s,at+20)==0);
            if(mode==5)s.base()[at+1]=0xAB;
            if(mode==7)put(s,token+0x14C,0); // Corrupt the parked thread identity, not the lock.
            if(mode!=6)assert(r.bindGuestThreadToken(token));
        }
        if(mode==1 || mode==2)throw std::runtime_error("injected wait cancellation");
        return mode>=4;
    });
    lockCall(r,"__imp__RtlInitializeCriticalSection",at);lockCall(r,Enter,at);
    const auto worker=r.createNativeCallbackWorker(Code,0,[](const auto&){return true;});assert(worker);
    token=worker->layout.threadAddress;assert(r.bindGuestThreadToken(token));
    GuestImportCall c;bool caught=false;
    try {assert(call(r,Enter,c,{at}).state==GuestImportDispatchState::Rejected);assert(c.returnValue==0xFEDCBA9876543210ULL);}
    catch(const std::runtime_error&){caught=true;}
    assert(caught==(mode==1 || mode==2));
    assert((r.nativeCriticalSectionWaitStatistics()==std::array<std::uint64_t,4>{1,0,1,0}));
    assert(get(s,at+24)==(mode<2?Parent:0U));
    if(mode==5)s.base()[at+1]=0;
    assert(r.bindGuestThreadToken(Parent));
    if(mode<2)lockCall(r,Leave,at);
    lockCall(r,"__imp__RtlInitializeCriticalSection",at);
}
void executorCancellation(GuestAddressSpace& s,bool budget) {
    GuestImportRegistry r;GuestCooperativeExecutor e;bool parked=false;
    setup(s,r,[&](const auto& ready){return e.waitUntil(ready);});
    const auto at=allocate(s,r,1);lockCall(r,"__imp__RtlInitializeCriticalSection",at);
    const auto result=e.run(Parent,[&] {
        lockCall(r,Enter,at);
        assert(r.createNativeCallbackWorker(Code,0,[&](const auto& t) {
            return e.enqueue(t.layout.threadAddress,[&]{parked=true;lockCall(r,Enter,at);assert(false);});
        }));
        assert(e.waitUntil([&]{return parked;}));
        if(!budget)throw std::runtime_error("fixture main failure");
        for(;;)GuestExecutionGuard::checkpoint("fixture budget exhaustion");
    },{40,2,2},[&](auto token){assert(r.bindGuestThreadToken(token));});
    assert(!result.execution.completed());assert(parked);
    assert((r.nativeCriticalSectionWaitStatistics()==std::array<std::uint64_t,4>{1,0,1,0}));
    assert(get(s,at+24)==Parent && get(s,at+20)==1);
    assert(r.bindGuestThreadToken(Parent));lockCall(r,Leave,at);tryFree(s,r,1,at,true);
}
void allocationFailures(GuestAddressSpace& s) {
    unsigned failures=0;
    for(int n=0;n<24;++n) {
        GuestImportRegistry r;setup(s,r,[](const auto&){return false;});
        constexpr std::uint32_t at=0x5000;
        lockCall(r,"__imp__RtlInitializeCriticalSection",at);lockCall(r,Enter,at);
        const auto worker=r.createNativeCallbackWorker(Code,0,[](const auto&){return true;});assert(worker);
        assert(r.bindGuestThreadToken(worker->layout.threadAddress));
        std::array<std::uint8_t,28> before;std::copy_n(s.base()+at,28,before.begin());
        GuestImportCall c;allocationFailureCountdown=n;
        try {assert(call(r,Enter,c,{at}).state==GuestImportDispatchState::Rejected);}
        catch(const std::bad_alloc&) {++failures;}
        allocationFailureCountdown=-1;
        const auto stats=r.nativeCriticalSectionWaitStatistics();
        assert(stats[0]==stats[2] && stats[0]<=1 && stats[1]==0 && stats[3]==0);
        assert(std::equal(before.begin(),before.end(),s.base()+at));
        assert(r.bindGuestThreadToken(Parent));lockCall(r,Leave,at);
        lockCall(r,"__imp__RtlInitializeCriticalSection",at); // No leaked empty queue/pin.
    }
    assert(failures>=5); // Handler/string/map/vector/readiness allocation boundaries.
}
void tryEnter(GuestAddressSpace& s) {
    GuestImportRegistry r;assert(r.initialize(s,0x10000000,0x10000));
    constexpr std::uint32_t at=0x5000;constexpr auto Try="__imp__RtlTryEnterCriticalSection";
    lockCall(r,"__imp__RtlInitializeCriticalSection",at);
    GuestImportCall c;assert(!call(r,Try,c,{at}).handled() && c.returnValue==0);
    assert(r.bindGuestThreadToken(1));
    for(unsigned n=1;n<=100;++n) {assert(invoke(r,Try,{at})==1);assert(get(s,at+20)==n && get(s,at+16)==n-1 && get(s,at+24)==1);}
    assert(r.bindGuestThreadToken(2));
    std::array<std::uint8_t,28> before;std::copy_n(s.base()+at,28,before.begin());
    for(unsigned n=0;n<100;++n) {assert(invoke(r,Try,{at})==0);assert(std::equal(before.begin(),before.end(),s.base()+at));}
    assert((r.nativeCriticalSectionWaitStatistics()==std::array<std::uint64_t,4>{}));
    assert(r.bindGuestThreadToken(1));for(unsigned n=0;n<100;++n)lockCall(r,Leave,at);
    assert(r.bindGuestThreadToken(2));assert(invoke(r,Try,{at})==1);lockCall(r,Leave,at);
    // Canonical static adoption and full register-width output, no extra flag.
    const auto other=at+64;put(s,other,0x01000400);put(s,other+4,0);put(s,other+8,other+8);put(s,other+12,other+8);
    put(s,other+16,UINT32_MAX);put(s,other+20,0);put(s,other+24,0);
    c={};c.arguments[0]=0xFEDCBA9800000000ULL|other;c.returnValue=UINT64_MAX;
    assert(r.dispatch(Try,c).handled() && c.returnValue==1);assert(get(s,other+24)==2);lockCall(r,Leave,other);
    s.base()[other+1]=0xEE;assert(!call(r,Try,c,{other}).handled() && !c.returnValue);s.base()[other+1]=0;
    for(auto bad:{0U,0xFFFFFFFFU,0x7FC80000U,0x10000000U,at+1,at+128})
        assert(!call(r,Try,c,{bad}).handled() && !c.returnValue);
    assert(get(s,other+24)==0 && get(s,other+20)==0 && get(s,other+16)==UINT32_MAX);
}
void idleStorageReuse(GuestAddressSpace& s, unsigned kind, bool spin) {
    GuestImportRegistry r;
    setup(s, r, [](const auto&) { return false; });
    const auto at = allocate(s, r, kind);
    lockCall(r, "__imp__RtlInitializeCriticalSection", at);
    lockCall(r, "__imp__RtlInitializeCriticalSection", at + 32);
    lockCall(r, Enter, at + 32);
    const auto initialize = [&](std::uint32_t address) {
        GuestImportCall c;
        return call(r, spin ? "__imp__RtlInitializeCriticalSectionAndSpinCount"
                            : "__imp__RtlInitializeCriticalSection", c, {address, 256});
    };
    std::array<std::uint8_t, 64> before;
    std::copy_n(s.base() + at, before.size(), before.begin());
    // Overlaps one idle and one owned section: reject atomically, preserving both.
    assert(!initialize(at + 16).handled());
    assert(std::equal(before.begin(), before.end(), s.base() + at));
    lockCall(r, Enter, at); lockCall(r, Leave, at);
    lockCall(r, Leave, at + 32);
    assert(initialize(at + 16).handled());
    assert(s.base()[at + 17] == (spin ? 1 : 0));
    assert(get(s, at + 32) == UINT32_MAX);
    lockCall(r, Enter, at + 16);
    assert(!initialize(at + 20).handled());
    lockCall(r, Leave, at + 16);
    // A second reuse confirms retired identities cannot interfere later.
    assert(initialize(at + 20).handled());
    lockCall(r, Enter, at + 20); lockCall(r, Leave, at + 20);
    tryFree(s, r, kind, at, true);
}
}
int main() {
    GuestAddressSpace s;assert(s.reserve());
    for(unsigned kind=0;kind<3;++kind)for(bool spin:{false,true})fifo(s,kind,spin);
    for(unsigned mode:{0U,1U,2U,3U,5U,6U,7U})injectedFailure(s,mode);
    executorCancellation(s,false);executorCancellation(s,true);
    allocationFailures(s);
    tryEnter(s);
    for (unsigned kind = 0; kind < 3; ++kind)
        for (bool spin : {false, true}) idleStorageReuse(s, kind, spin);
    std::puts("CRITICAL_SECTION_WAIT_V1 fifo_cases=6 failures=9 allocation_positions=24 try_enter_checks=210 recursion=1 no_poll_acquisition=1 lifetime_pins=1");
}
