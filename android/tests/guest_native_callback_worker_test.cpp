#include "guest_import_registry.h"
#include "guest_cooperative_executor.h"
#include "guest_thread_entry.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <stdexcept>

using namespace recomp::bootstrap;
namespace {
constexpr std::uint32_t Parent=0x63001000,Code=0x82001000,Arena=0x64000000;
const GuestThreadStorageConfig Config{{Arena,0x100000},{0x82000000,0x10000},{Code,0x1000},
    0x82000010,4,32,4,0x4000,2};
std::uint32_t get(const GuestAddressSpace& s,std::uint32_t a) {
    const auto* b=s.base()+a;return (std::uint32_t{b[0]}<<24)|(std::uint32_t{b[1]}<<16)|(std::uint32_t{b[2]}<<8)|b[3];
}
std::uint32_t invoke(GuestImportRegistry& r,std::string_view name,std::initializer_list<std::uint32_t> args) {
    GuestImportCall call;std::copy(args.begin(),args.end(),call.arguments.begin());
    assert(r.dispatch(name,call).handled());return static_cast<std::uint32_t>(call.returnValue);
}
void setup(GuestImportRegistry& r,GuestAddressSpace& s) {
    assert(r.initialize(s,0x10000000,0x10000));assert(r.bindGuestThreadToken(Parent));
    assert(r.bindGuestThreadTlsStorage(Parent,0x63002000,4));
    assert(r.bindThreadStorageArena(Config,{},[](auto address) {return address==Code;}));
}
}
int main() {
    GuestAddressSpace s;assert(s.reserve());GuestImportRegistry r;
    s.base()[Config.tlsRawDataAddress]=0xAB;
    assert(!r.createNativeCallbackWorker(Code,0,[](const auto&) {return false;}));
    setup(r,s);
    const auto slot=invoke(r,"__imp__KeTlsAlloc",{});assert(invoke(r,"__imp__KeTlsSetValue",{slot,0x11223344})==1);
    assert(!r.createNativeCallbackWorker(Code,0,{}));
    assert(!r.createNativeCallbackWorker(Code+4,0,[](const auto&) {assert(false);return false;}));
    for(bool throws:{false,true}) {
        bool caught=false;GuestPreparedThread observed;
        try {
            assert(!r.createNativeCallbackWorker(Code,0xDEADBEEF,[&](const auto& thread)->bool {
                observed=thread;assert(thread.wrappedContextAddress==thread.startContext);
                assert(get(s,thread.startContext)==0xDEADBEEF);
                if(throws) throw std::runtime_error("fixture enqueue failure");return false;
            }));
        } catch(const std::runtime_error&) {caught=true;}
        assert(caught==throws && r.createdThreadCount()==0);
        assert(std::all_of(s.base()+observed.allocation.base,s.base()+observed.allocation.base+observed.allocation.size,
                           [](auto byte) {return byte==0;}));
        assert(invoke(r,"__imp__KeTlsGetValue",{slot})==0x11223344);
    }
    GuestCooperativeExecutor executor;unsigned calls=0;std::optional<GuestPreparedThread> worker;
    const auto result=executor.run(Parent,[&] {
        worker=r.createNativeCallbackWorker(Code,0xDEADBEEF,[&](const auto& thread) {
            const auto entry=makeGuestThreadEntry(thread);assert(entry && entry->argument0==thread.wrappedContextAddress);
            assert(entry->argument1==0 && entry->address==Code && entry->directEntry);
            return executor.enqueue(thread.layout.threadAddress,[&,thread,entry=*entry] {
                assert(get(s,entry.argument0)==0xDEADBEEF && s.base()[thread.layout.tlsAddress]==0xAB);
                assert(invoke(r,"__imp__KeTlsGetValue",{slot})==0);
                assert(invoke(r,"__imp__KeTlsSetValue",{slot,0x55667788})==1);
                ++calls;assert(r.markGuestThreadReturned(thread.layout.threadAddress,0));
            });
        });
        assert(worker && calls==0); // Enqueue must never invoke inline.
        assert(executor.waitUntil([&] {return calls==1;}));
        assert(invoke(r,"__imp__KeTlsGetValue",{slot})==0x11223344);
    },{100,1,2},[&](auto token) {assert(r.bindGuestThreadToken(token));});
    assert(result.execution.completed() && result.workersEntered==1 && calls==1);
    assert(r.createdThreadCount()==1 && get(s,worker->dynamicTlsAddress)==0x55667788);
    const auto allocation=worker->allocation;r.reset();
    assert(std::all_of(s.base()+allocation.base,s.base()+allocation.base+allocation.size,[](auto byte){return byte==0;}));
    // Budget/host-exception stops join and unwind native workers before their
    // guest storage is scrubbed. A stopped callback must never survive reset.
    for(bool throws:{false,true}) {
        setup(r,s);GuestCooperativeExecutor stopped;bool entered=false,unwound=false;
        std::optional<GuestPreparedThread> cancelled;
        const auto outcome=stopped.run(Parent,[&] {
            cancelled=r.createNativeCallbackWorker(Code,0xAABBCCDD,[&](const auto& thread) {
                return stopped.enqueue(thread.layout.threadAddress,[&] {
                    struct OnExit {bool& value;~OnExit(){value=true;}} onExit{unwound};
                    entered=true;
                    if(throws) throw std::runtime_error("fixture callback exception");
                    for(;;) GuestExecutionGuard::checkpoint("native_callback_budget");
                });
            });
            assert(cancelled);(void)stopped.waitUntil([] {return false;});
        },{8,1,2},[&](auto token) {assert(r.bindGuestThreadToken(token));});
        assert(outcome.execution.reason==(throws?GuestExecutionStopReason::HostException:
                                               GuestExecutionStopReason::FunctionEntryBudgetExceeded));
        assert(entered && unwound && get(s,cancelled->startContext)==0xAABBCCDD);
        const auto owned=cancelled->allocation;r.reset();
        assert(std::all_of(s.base()+owned.base,s.base()+owned.base+owned.size,[](auto byte){return byte==0;}));
    }
    std::puts("NATIVE_CALLBACK_WORKER_V1 serialized_worker=1 rollback=2 independent_tls=1 wrapped_context=1");
}
