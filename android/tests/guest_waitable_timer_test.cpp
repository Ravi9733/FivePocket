#include "guest_import_registry.h"
#include "guest_cooperative_executor.h"

#include <algorithm>
#include <cassert>
#include <cstring>

using namespace recomp::bootstrap;
using Clock = GuestWaitableTimer::Clock;
using namespace std::chrono_literals;
namespace {
constexpr std::uint32_t Parent=0x30000000, Out=0x30010000, Due=Out+8, Timeout=Out+24;
void put64(GuestAddressSpace& s, std::uint32_t at, std::uint64_t value) {
    for (unsigned i=0;i<8;++i) s.translate(at)[i]=static_cast<std::uint8_t>(value>>(56-8*i));
}
std::uint32_t get(GuestAddressSpace& s,std::uint32_t at) {
    const auto* p=s.translate(at);
    return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];
}
struct Fixture {
    GuestAddressSpace& s;
    GuestImportRegistry r;
    GuestCooperativeExecutor e;
    explicit Fixture(GuestAddressSpace& memory):s(memory) {
        assert(r.initialize(s,0x10000000,0x10000));
        assert(r.bindGuestThreadToken(Parent));
        assert(r.bindThreadStorageArena({{0x64000000,0x100000},{0x82000000,0x10000},
            {0x82001000,0x1000},0,0,32,4,0x4000,2},{},[](auto at){return at==0x82001000;}));
        assert(r.bindThreadResumeHandler([&](const auto& t,auto p){return e.enqueue(t.layout.threadAddress,[]{},p);}));
        assert(r.bindThreadWaitHandler([&](const auto& ready){return e.waitUntil(ready);}));
        assert(r.bindThreadTimedWaitHandler([&](const auto& ready,auto due){return e.waitUntil(ready,due);}));
        assert(r.bindClockSources([]{return std::optional<std::uint64_t>(100000000);},50000000));
        std::memset(s.translate(Out),0xA5,64);
        put64(s,Timeout,0);
    }
    GuestImportDispatchResult dispatch(const char* name,std::initializer_list<std::uint32_t> args,GuestImportCall& call) {
        call={};std::copy(args.begin(),args.end(),call.arguments.begin());
        return r.dispatch(name,call);
    }
    std::uint32_t invoke(const char* name,std::initializer_list<std::uint32_t> args) {
        GuestImportCall call;assert(dispatch(name,args,call).handled());return call.returnValue;
    }
    std::uint32_t create(unsigned type) {
        assert(invoke("__imp__NtCreateTimer",{Out,0,type})==0);return get(s,Out);
    }
    void arm(std::uint32_t timer,std::uint64_t due,unsigned period=0) {
        put64(s,Due,due);
        assert(invoke("__imp__NtSetTimerEx",{timer,Due,0,1,0,0,period,0})==0);
    }
    std::uint32_t wait(std::uint32_t timer,std::uint32_t timeout=0) {
        return invoke("__imp__NtWaitForSingleObjectEx",{timer,0,0,timeout});
    }
    void run(const std::function<void()>& body) {
        const auto result=e.run(Parent,body,{1000,32,4},[&](auto token){assert(r.bindGuestThreadToken(token));});
        assert(result.execution.completed());
    }
};
void periodicState() {
    const auto zero=Clock::time_point{}+100s;
    GuestWaitableTimer t{zero+10ms,10ms};
    assert(!t.refresh(zero+9ms));
    assert(t.refresh(zero+10ms) && t.due==zero+20ms);
    assert(t.refresh(zero+45ms) && t.due==zero+50ms);
    assert(!t.refresh(zero+45ms));
    t.period=Clock::duration::zero();
    assert(t.refresh(zero+50ms) && !t.due);
    assert(!t.refresh(zero+100ms));
}
void lifecycle(GuestAddressSpace& s) {
    Fixture f(s);
    f.run([&]{
        for (unsigned type:{0U,1U}) {
            const auto timer=f.create(type);
            assert(f.wait(timer,Timeout)==0x102);
            f.arm(timer,0);
            assert(f.wait(timer,Timeout)==0);
            assert(f.wait(timer,Timeout)==(type?0x102U:0U));
            assert(f.invoke("__imp__NtCancelTimer",{timer,Out+4})==0);
            assert(get(s,Out+4)==(type?0U:1U) && s.translate(Out)[3]==(timer&255));
            assert(f.wait(timer,Timeout)==(type?0x102U:0U)); // Cancel does not reset signal.
            const auto before=Clock::now();
            f.arm(timer,0ULL-20000ULL);
            assert(f.wait(timer)==0 && Clock::now()-before>=2ms); // Signal, not timeout.
            f.arm(timer,0ULL-20000ULL);
            assert(f.invoke("__imp__NtCancelTimer",{timer,Out+4})==0 && get(s,Out+4)==0);
            put64(s,Timeout,0ULL-40000ULL);
            assert(f.wait(timer,Timeout)==0x102);
            put64(s,Timeout,0);
            // Absolute past time fires immediately using the bound clock source.
            f.arm(timer,99999999ULL);assert(f.wait(timer,Timeout)==0);
            assert(f.invoke("__imp__NtClose",{timer})==0);
            assert(f.wait(timer,Timeout)==0xC0000008U);
        }
        const auto timer=f.create(1);
        f.arm(timer,0ULL-10000ULL,2);
        assert(f.wait(timer)==0 && f.wait(timer)==0);
        assert(f.invoke("__imp__NtCancelTimer",{timer,0})==0);
        assert(f.wait(timer,Timeout)==0x102);
        GuestImportCall call;
        assert(!f.dispatch("__imp__NtCreateTimer",{0,0,0},call).handled());
        assert(!f.dispatch("__imp__NtCreateTimer",{Out,1,0},call).handled());
        assert(f.invoke("__imp__NtCreateTimer",{Out,0,2})==0xC000000DU);
        for (unsigned index:{2U,3U,4U,5U,7U}) {
            call={};call.arguments={timer,Due,0,1,0,0,0,0};call.arguments[index]^=1;
            assert(!f.r.dispatch("__imp__NtSetTimerEx",call).handled());
        }
        assert(!f.dispatch("__imp__NtSetTimerEx",{timer,Due+1,0,1,0,0,0,0},call).handled());
        put64(s,Due,0x8000000000000000ULL);
        assert(!f.dispatch("__imp__NtSetTimerEx",{timer,Due,0,1,0,0,0,0},call).handled());
        assert(!f.dispatch("__imp__NtCancelTimer",{timer,Out+1},call).handled());
        assert(f.invoke("__imp__NtCreateEvent",{Out,0,0,0})==0);
        assert(f.invoke("__imp__NtCancelTimer",{get(s,Out),0})==0xC0000024U);
    });
}
void rearmWhileParked(GuestAddressSpace& s) {
    for (bool cancel:{false,true}) {
        Fixture f(s);
        f.run([&]{
            const auto timer=f.create(1);
            f.arm(timer,0ULL-10000000ULL); // Original expiration is one second away.
            assert(f.e.enqueue(Parent+4,[&]{
                if (cancel) assert(f.invoke("__imp__NtCancelTimer",{timer,0})==0);
                else f.arm(timer,0ULL-20000ULL);
            }));
            put64(s,Timeout,0ULL-100000ULL);
            assert(f.wait(timer,Timeout)==(cancel?0x102U:0U));
            // Earlier rearm must be a timer signal; cancellation a caller timeout.
        });
    }
}
}
int main() {
    periodicState();GuestAddressSpace space;assert(space.reserve());
    lifecycle(space);rearmWhileParked(space);
}
