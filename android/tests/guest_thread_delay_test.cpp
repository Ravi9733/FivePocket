#include "guest_import_registry.h"
#include "guest_cooperative_executor.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <functional>
#include <stdexcept>

using namespace recomp::bootstrap;
using Clock = std::chrono::steady_clock;
namespace {
constexpr auto Name = "__imp__KeDelayExecutionThread";
constexpr std::uint32_t Parent = 0x63001000, Code = 0x82001000, Out = 0x30000000, Interval = Out + 64;
constexpr std::uint64_t Canary = 0xABCD123456789ABCULL;
void put(GuestAddressSpace& s, std::uint32_t a, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) s.base()[a+i] = static_cast<std::uint8_t>(value >> (24-i*8));
}
std::uint32_t get(GuestAddressSpace& s, std::uint32_t a) {
    auto* b = s.base()+a;
    return (std::uint32_t{b[0]}<<24)|(std::uint32_t{b[1]}<<16)|(std::uint32_t{b[2]}<<8)|b[3];
}
void interval(GuestAddressSpace& s, std::uint64_t raw, std::uint32_t address = Interval) {
    put(s, address, raw >> 32); put(s, address+4, raw);
}
GuestImportCall arguments(std::initializer_list<std::uint64_t> values) {
    GuestImportCall c; std::copy(values.begin(), values.end(), c.arguments.begin()); c.returnValue=Canary; return c;
}
struct Fixture {
    GuestAddressSpace& s;
    GuestImportRegistry r;
    GuestCooperativeExecutor e;
    std::function<void()> worker;
    std::function<bool(Clock::time_point)> delay;
    unsigned calls=0;
    explicit Fixture(GuestAddressSpace& space):s(space) {
        assert(!r.bindThreadDelayHandler([](auto){return true;}));
        assert(r.initialize(s,0x10000000,0x10000));
        assert(r.bindGuestThreadToken(Parent)); put(s,Parent+0x14C,1); assert(r.bindInitialThreadObject(Parent,1));
        assert(r.bindThreadStorageArena({{0x64000000,0x100000},{0x82000000,0x10000},{Code,0x1000},0,0,32,4,0x4000,2},{},[](auto p){return p==Code;}));
        assert(r.bindThreadResumeHandler([&](const auto& thread,auto priority){
            return e.enqueue(thread.layout.threadAddress,[&,token=thread.layout.threadAddress]{
                worker(); assert(r.markGuestThreadReturned(token,0));
            },priority);
        }));
        assert(r.bindThreadWaitHandler([&](const auto& ready){return e.waitUntil(ready);}));
        assert(!r.bindThreadDelayHandler([](auto){return true;}));
        assert(r.bindThreadTimedWaitHandler([&](const auto& ready,auto deadline){return e.waitUntil(ready,deadline);}));
        delay=[&](auto deadline){return e.delayUntil(deadline);};
        assert(r.bindThreadDelayHandler([&](auto deadline){
            ++calls; assert(r.threadDelayStatistics()[6]>0); // Reenter: registry mutex is released.
            return delay(deadline);
        }));
        assert(!r.bindThreadDelayHandler([](auto){return true;}));
    }
    void startWorker() {
        auto c=arguments({Out,0,Out+4,0,Code,0,1}); assert(r.dispatch("__imp__ExCreateThread",c).handled());
        c=arguments({get(s,Out),Out+8}); assert(r.dispatch("__imp__NtResumeThread",c).handled());
    }
    GuestCooperativeResult run(const std::function<void()>& body, std::uint64_t budget=1000) {
        return e.run(Parent,body,{budget,32,2},[&](auto token){assert(r.bindGuestThreadToken(token));});
    }
};
void timing(GuestAddressSpace& s) {
    Fixture f(s);
    auto result=f.run([&] {
        for (unsigned mode:{0U,1U}) for (std::uint64_t ticks:{0ULL,1ULL,10000ULL,30000ULL}) {
            interval(s,0-ticks);
            const auto before=Clock::now();
            auto c=arguments({mode,0,Interval});
            assert(f.r.dispatch(Name,c).handled() && c.returnValue==0);
            assert(Clock::now()-before>=std::chrono::nanoseconds(ticks*100));
            assert(get(s,Interval)==static_cast<std::uint32_t>((0-ticks)>>32));
            assert(get(s,Interval+4)==static_cast<std::uint32_t>(0-ticks));
        }
    });
    assert(result.execution.completed() && result.waitOperations==8);
    assert((f.r.threadDelayStatistics()==std::array<std::uint64_t,7>{8,8,0,2,80002,30000,0}));
    f.r.reset(); assert((f.r.threadDelayStatistics()==std::array<std::uint64_t,7>{}));
    assert(!f.e.delayUntil(Clock::now()));
}
void parallelAndSnapshot(GuestAddressSpace& s, bool zero) {
    Fixture f(s); bool other=false, returned=false;
    f.worker=[&] {
        assert(!returned); other=true;
        interval(s,0x8000000000000000ULL); // Must not extend the already snapshotted main delay.
        if (!zero) {
            interval(s,0ULL-10000,Interval+16);
            auto c=arguments({0,0,Interval+16}); assert(f.r.dispatch(Name,c).handled() && !returned);
        }
    };
    const auto result=f.run([&] {
        f.startWorker(); assert(!other);
        interval(s,zero?0:0ULL-300000);
        auto c=arguments({1,0,Interval}); const auto before=Clock::now();
        assert(f.r.dispatch(Name,c).handled() && c.returnValue==0); returned=true;
        assert(other && (zero || Clock::now()-before>=std::chrono::milliseconds(30)));
    });
    assert(result.execution.completed() && returned && other);
    const auto stats=f.r.threadDelayStatistics();
    assert(stats[0]==(zero?1:2) && stats[0]==stats[1] && !stats[2] && !stats[6]);
}
void guardsAndFailure(GuestAddressSpace& s) {
    Fixture f(s); unsigned guards=0;
    const auto reject=[&](std::uint64_t mode,std::uint64_t alert,std::uint32_t pointer) {
        auto c=arguments({mode,alert,pointer}); const auto before=f.r.threadDelayStatistics();
        assert(!f.r.dispatch(Name,c).handled() && c.returnValue==Canary);
        assert(f.r.threadDelayStatistics()==before); ++guards;
    };
    auto result=f.run([&] {
        interval(s,0);
        for(auto mode:{2ULL,0xFFFFFFFFULL}) reject(mode,0,Interval);
        for(auto alert:{1ULL,0x100ULL,0xFFFFFFFFULL}) reject(1,alert,Interval);
        for(auto pointer:{0U,Interval+1,0xFFFFFFFCU,0x7FC80000U,0x10000000U,0x64000000U}) reject(1,0,pointer);
        for(auto raw:{1ULL,0x7FFFFFFFFFFFFFFFULL,0x8000000000000000ULL,0ULL-600000001}) {
            interval(s,raw); reject(1,0,Interval);
        }
        interval(s,0); put(s,Parent+0x14C,99); reject(1,0,Interval); put(s,Parent+0x14C,1);
        // ABI consumes low32 argument bits, including pointers and mode/Boolean fields.
        auto c=arguments({0xAABBCCDD00000001ULL,0xAABBCCDD00000000ULL,0xAABBCCDD00000000ULL|Interval});
        assert(f.r.dispatch(Name,c).handled() && c.returnValue==0);
    });
    assert(result.execution.completed() && guards==16 && f.calls==1);
    for(unsigned failure=0;failure<5;++failure) {
        Fixture broken(s);
        broken.delay=[&](auto deadline) {
            if(failure==0)return false;
            if(failure==1)throw std::runtime_error("injected delay failure");
            if(failure==2)return true; // Early false success must be rejected.
            assert(broken.e.delayUntil(deadline));
            if(failure==3)put(s,Parent+0x14C,99);
            else assert(broken.r.bindGuestThreadToken(0x63003000));
            return true;
        };
        result=broken.run([&] {
            interval(s,failure==2?0ULL-600000000:0);
            auto c=arguments({1,0,Interval}); bool threw=false;
            try{assert(!broken.r.dispatch(Name,c).handled());}catch(const std::runtime_error&){threw=true;}
            assert(threw==(failure==1) && c.returnValue==Canary);
        });
        assert(result.execution.completed()); const auto stats=broken.r.threadDelayStatistics();
        assert(stats[0]==1 && !stats[1] && stats[2]==1 && !stats[6]);
    }
}
void cancellation(GuestAddressSpace& s, bool exception) {
    Fixture f(s); bool returned=false;
    f.worker=[&]{
        if(exception)throw std::runtime_error("injected peer failure");
        for(;;)GuestExecutionGuard::checkpoint("cancel_sleeping_peer");
    };
    auto c=arguments({1,0,Interval});
    const auto result=f.run([&] {
        f.startWorker(); interval(s,0ULL-600000000);
        (void)f.r.dispatch(Name,c); returned=true;
    },100);
    assert(!result.execution.completed() && !returned && c.returnValue==Canary);
    const auto stats=f.r.threadDelayStatistics(); assert(stats[0]==1 && !stats[1] && stats[2]==1 && !stats[6]);
}
}
int main() {
    GuestAddressSpace s; assert(s.reserve());
    timing(s); parallelAndSnapshot(s,false); parallelAndSnapshot(s,true);
    guardsAndFailure(s); cancellation(s,false); cancellation(s,true);
    std::puts("THREAD_DELAY_V1 timing=8 snapshot=1 zero_yield=1 guards=16 failures=5 peer_cancellation=2");
}
