#include "guest_cooperative_executor.h"
#include "guest_thread_entry.h"
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <memory>
using namespace recomp::bootstrap;
int main() {
    GuestPreparedThread prepared;prepared.layout.pcrAddress=0x60000000;
    prepared.layout.stackBase=0x61000000;prepared.layout.stackSize=0x4000;
    prepared.startRoutine=0x82001000;prepared.startContext=0x10002000;prepared.creationFlags=0;
    const auto interruptEntry=makeGuestCommandInterruptEntry(prepared);
    assert(interruptEntry && interruptEntry->argument0==1 && interruptEntry->argument1==prepared.startContext);
    assert(interruptEntry->stackPointer==0x61004000-176 && interruptEntry->pcrAddress==0x60000000);
    prepared.wrappedContextAddress=0x60001000;assert(!makeGuestCommandInterruptEntry(prepared));prepared.wrappedContextAddress=0;
    prepared.xapiStartup=0x82002000;assert(!makeGuestCommandInterruptEntry(prepared));prepared.xapiStartup=0;
    prepared.creationFlags=1;assert(!makeGuestCommandInterruptEntry(prepared));prepared.creationFlags=0;
    prepared.startRoutine=0;assert(!makeGuestCommandInterruptEntry(prepared));
    GuestCooperativeExecutor executor,other;
    bool called=false;
    assert(!executor.runNativeWorkerNext(2,32));
    for (bool parentWorker:{false,true}) {
        unsigned bound=0,peers=0;std::vector<unsigned> order;
        const auto parent=[&] {
            const auto parentToken=parentWorker?2u:1u;
            assert(bound==parentToken);
            assert(!executor.runNativeWorkerNext(parentToken,32) && !executor.runNativeWorkerNext(99,32));
            assert(!executor.runNativeWorkerNext(4,0) && !executor.runNativeWorkerNext(4,4097));
            assert(executor.enqueue(4,[&] {
                assert(bound==4 && !peers);order.push_back(2);
                assert(!executor.runNativeWorkerNext(3,32));
                assert(!executor.waitUntil([&] {++peers;return true;}));
                assert(!executor.delayUntil(std::chrono::steady_clock::now()));
                // Handoff itself excludes peers even before the inner IRQ scope.
                GuestExecutionGuard::checkpoint("exclusive_before_scope");
                assert(executor.runNativeNonYielding([&] {
                    for (unsigned i=0;i<8;++i) {GuestExecutionGuard::checkpoint("exclusive_irq");assert(!peers && bound==4);}
                },8));
                GuestExecutionGuard::checkpoint("exclusive_after_scope");
            }));
            order.push_back(1);assert(executor.runNativeWorkerNext(4,10));
            assert(bound==parentToken && !peers);order.push_back(3);
            assert(!executor.runNativeWorkerNext(4,10)); // Already entered/returned.
        };
        const auto result=executor.run(1,[&] {
            if (parentWorker) assert(executor.enqueue(2,parent));
            assert(executor.enqueue(3,[&] {++peers;order.push_back(4);}));
            if (parentWorker) GuestExecutionGuard::checkpoint("select_parent");else parent();
        },{64,1,6},[&](auto token) {bound=token;});
        assert(result.execution.completed() && peers==1 && (order==std::vector<unsigned>{1,2,3,4}));
    }
    {
        unsigned readyCalls=0;bool ready=false;
        const auto result=executor.run(1,[&] {
            assert(executor.enqueue(2,[&] {assert(executor.waitUntil([&] {++readyCalls;return ready;}));}));
            GuestExecutionGuard::checkpoint("park_peer");
            const auto before=readyCalls;assert(before);
            assert(executor.enqueue(3,[&] {for (unsigned i=0;i<8;++i) GuestExecutionGuard::checkpoint("no_peer_readiness");}));
            assert(executor.runNativeWorkerNext(3,8) && readyCalls==before);
            ready=true;GuestExecutionGuard::checkpoint("resume_peer");
        },{32,1,4},[](auto) {});assert(result.execution.completed() && readyCalls>1);
    }
    for (unsigned failure=0;failure<4;++failure) {
        unsigned peers=0;bool returned=false;
        const auto result=executor.run(1,[&] {
            assert(executor.enqueue(2,[&] {++peers;}));
            assert(executor.enqueue(3,[&] {
                if (failure==0) throw std::runtime_error("IRQ handoff fixture");
                if (failure==1) GuestExecutionGuard::stopForUnimplementedImport("__imp__HandoffFixtureMissing");
                for (;;) GuestExecutionGuard::checkpoint("handoff_budget");
            }));
            (void)executor.runNativeWorkerNext(3,failure==2?4:4096);returned=true;
        },{16,1,4},[](auto) {});
        assert(!returned && !peers && !result.execution.completed());
        if (failure>=2) assert(result.execution.reason==GuestExecutionStopReason::FunctionEntryBudgetExceeded &&
            result.execution.completedFunctionEntries==(failure==2?4u:16u));
        else assert(result.execution.reason==(failure?GuestExecutionStopReason::UnimplementedImport:GuestExecutionStopReason::HostException));
        const auto reused=executor.run(1,[&] {assert(executor.enqueue(2,[] {}));assert(executor.runNativeWorkerNext(2,1));},
            {8,1,2},[](auto) {});assert(reused.execution.completed());
    }
    std::puts("NATIVE_INTERRUPT_EXCLUSIVE_HANDOFF: main/worker parent,peer/readiness isolation,2entry budgets,4terminal controls,reuse PASS");
    for(bool parentWorker:{false,true}){
        unsigned bound=0,peers=0,stage=0;const unsigned parentToken=parentWorker?2:1;
        using Action=std::function<bool(unsigned)>;
        auto actionSlot=std::make_shared<const Action*>(nullptr);
        const auto parent=[&]{
            assert(bound==parentToken);
            assert(executor.enqueue(4,[&]{assert(bound==4 && !stage && !peers);stage=1;}));
            assert(executor.runNativeWorkerNext(4,1) && stage==1 && bound==parentToken);
            // Fresh job exists before accepting presentation, but cannot run
            // until the backend explicitly lends its native action callback.
            assert(executor.enqueue(5,[&,actionSlot]{
                assert(bound==5 && !peers && stage==2 && *actionSlot);
                const auto& action=**actionSlot;
                assert(action(2));
                assert(executor.runNativeNonYielding([&]{
                    GuestExecutionGuard::checkpoint("second_irq_body");assert(stage==3 && !peers);stage=4;
                },1));
                assert(action(4));
            }));
            assert(stage==1 && !*actionSlot);stage=2; // Mock present, not a frame.
            const Action action=[&](unsigned expected){assert(bound==5 && !peers && stage==expected);++stage;return true;};
            *actionSlot=&action;
            assert(executor.runNativeWorkerNext(5,1));*actionSlot=nullptr;
            assert(bound==parentToken && !peers && stage==5);stage=6; // Mock tail.
        };
        const auto result=executor.run(1,[&]{
            if(parentWorker)assert(executor.enqueue(2,parent));
            assert(executor.enqueue(3,[&]{assert(stage==6 && !*actionSlot);++peers;}));
            if(parentWorker)GuestExecutionGuard::checkpoint("select_second_swap_parent");else parent();
        },{16,1,6},[&](auto token){bound=token;});
        assert(result.execution.completed() && peers==1 && stage==6);
    }
    std::puts("NATIVE_INTERRUPT_SECOND_HANDOFF: 2parent contexts,distinct sequential workers,borrowed native action lifetime,peer exclusion,ordered mock present/IRQ/tail PASS");
    assert(!executor.runNativeNonYielding([&] {called=true;},32) && !called);
    for (bool workerScope:{false,true}) {
        std::vector<unsigned> order;unsigned bound=0,reads=0,labels=0;
        const auto body=[&] {
            assert(bound==(workerScope?2u:1u));order.push_back(1);
            assert(!executor.runNativeNonYielding({},32));
            assert(!executor.runNativeNonYielding([&] {called=true;},0));
            assert(!executor.runNativeNonYielding([&] {called=true;},4097));
            assert(!other.runNativeNonYielding([&] {called=true;},32));
            assert(!called);
            assert(executor.runNativeNonYielding([&] {
                assert(!executor.runNativeNonYielding([&] {called=true;},32));
                assert(!executor.waitUntil([&] {++reads;return true;}));
                assert(!executor.waitUntil([&] {++reads;return false;},std::chrono::steady_clock::now()));
                assert(!executor.delayUntil(std::chrono::steady_clock::now()));
                assert(executor.enqueue(3,[&] {order.push_back(3);}));
                for (unsigned i=0;i<32;++i) {
                    GuestExecutionGuard::checkpoint("native_interrupt_fixture");
                    assert(bound==(workerScope?2u:1u) && order.size()==1);
                }
                order.push_back(2);
            },32));
            // Deferred rescheduling is not lost. The next checkpoint or wait
            // must allow ordinary tasks to run after the bounded scope ends.
            GuestExecutionGuard::checkpoint("after_native_interrupt");
            assert(executor.waitUntil([&] {return order.back()==3;}));
        };
        const auto result=executor.run(1,[&] {
            if (workerScope) { assert(executor.enqueue(2,body));GuestExecutionGuard::checkpoint("start_worker"); }
            else body();
        },{100,1,4},[&](auto token) {bound=token;},[&](auto) {++labels;});
        assert(result.execution.completed() && reads==0 && !called);
        assert((order==std::vector<unsigned>{1,2,3}));
        assert(result.execution.completedFunctionEntries==labels && labels==(workerScope?34u:33u));
    }
    // Caught host exceptions release the scope; normal scheduling and a second
    // bounded scope still work. No ownership state may leak to later runs.
    const auto recovered=executor.run(1,[&] {
        try { (void)executor.runNativeNonYielding([] {throw std::runtime_error("fixture");},8);assert(false); }
        catch (const std::runtime_error&) {}
        assert(executor.runNativeNonYielding([] {GuestExecutionGuard::checkpoint("recovered");},1));
        assert(executor.waitUntil([] {return true;}));
    },{32,1,2},[](auto) {});assert(recovered.execution.completed());
    for (unsigned mode=0;mode<4;++mode) {
        unsigned peers=0;bool unwound=false;
        const auto result=executor.run(1,[&] {
            assert(executor.enqueue(2,[&] {++peers;}));
            (void)executor.runNativeNonYielding([&] {
                struct Unwind {bool& flag;~Unwind(){flag=true;}} guard{unwound};
                if (mode==2) throw std::runtime_error("uncaught interrupt fixture");
                if (mode==3) GuestExecutionGuard::stopForUnimplementedImport("__imp__InterruptFixtureMissing");
                for (;;) GuestExecutionGuard::checkpoint("interrupt_budget");
            },mode==0?4:4096);
            assert(false);
        },{16,1,2},[](auto) {});
        assert(unwound && peers==0);
        if (mode<2) {
            assert(result.execution.reason==GuestExecutionStopReason::FunctionEntryBudgetExceeded);
            assert(result.execution.completedFunctionEntries==(mode==0?4u:16u));
        } else assert(result.execution.reason==(mode==2?GuestExecutionStopReason::HostException:GuestExecutionStopReason::UnimplementedImport));
        assert(!executor.running() && !executor.runNativeNonYielding([] {},1));
        const auto reused=executor.run(1,[&] {assert(executor.runNativeNonYielding([] {},1));}, {4,1,1},[](auto) {});
        assert(reused.execution.completed());
    }
    std::puts("NATIVE_INTERRUPT_EXECUTION_SCOPE: main/worker isolation,wait refusal,nesting,local/global budgets,4terminal unwinds PASS; no game interrupt dispatched");
}
