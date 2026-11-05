#pragma once
#include "pooled_host.h" // v240
#include "guest_cooperative_executor.h"
#include "guest_import_registry.h"
#include "native_tile_command_interrupt.h"
#include "diag_timeline.h" // v218: handoff phase marks
#include <algorithm>
#include <chrono>
#include <thread>
#include <memory>
#include <stdexcept>
#include <android/log.h> // v186 diagnostic line on a failed scene IRQ

extern "C" void recomp_v279_scene_inline(bool on); // v279 (guest_import_registry.cpp)
namespace recomp::bootstrap {
// Shared by the live runner and original-worker diagnostic. The
// original waiter is not replaced: a distinct registered CPU2 IRQ signals it,
// then returns exclusively to the submitter before any ordinary waiter runs.
// Caller owns the serialized executor token and current command allocations.
// Execute must run the real ordered native packet consumer (host fixtures must
// explicitly identify their authored prefix). No ring retirement is supplied.
struct NativeTileSceneHandoffResult {
    bool admitted=false,returned=false;
    unsigned worker=0,scratchWrites=0,scratchWaits=0,guestReturns=0;
};
using NativeTileScenePacketExecute=std::function<bool(
    const gpu::NativeTileCommandInterrupt::Memory&,const gpu::NativeTileCommandInterrupt::Guest&)>;
using NativeTileSceneGuestInvoke=std::function<bool(const GuestPreparedThread&,const GuestThreadEntryState&)>;
inline NativeTileSceneHandoffResult executeNativeTileSceneHandoff(
        GuestImportRegistry& registry,GuestCooperativeExecutor& executor,unsigned device,
        const gpu::Gta5TileCommandInterrupt& request,const NativeTileScenePacketExecute& execute,
        const NativeTileSceneGuestInvoke& invoke) {
    using Plan=gpu::Gta5TileCommandInterrupt;using Kind=Plan::Kind;
    struct State {
        Plan request;NativeTileScenePacketExecute execute;NativeTileSceneGuestInvoke invoke;
        std::optional<GuestImportRegistry::StoppedPhysicalOwnership> scratchOwner,contextOwner;
        NativeTileSceneHandoffResult result;bool armed=false;
    };
    // A predicated continuation may use this only when its real engine selects
    // the IRQ. A skipped IRQ must not allocate a worker or enter this helper.
    if(!execute || !invoke)return {};
    auto s=std::make_shared<State>(State{request,execute,invoke,{},{},{},false});
    const auto same=[](const auto& a,const auto& b) {
        return a && b && a->physicalBase==b->physicalBase && a->virtualBase==b->virtualBase &&
            a->byteCount==b->byteCount && a->protection==b->protection && a->writable==b->writable;
    };
    // This checks that we actually own an executor scope; a naked callback may
    // not allocate/run an IRQ while unrelated guest writers remain runnable.
    const bool admitted=executor.runNativeNonYielding([&] {
        if(registry.snapshotNativeInterruptDeviceRoot(device)!=std::optional{true})return;
        s->scratchOwner=registry.stoppedGraphicsPhysicalOwnership(request.scratchPhysical(),32);
        s->contextOwner=registry.stoppedGraphicsPhysicalOwnership(request.callbackContext()-0xC0000000,4);
        if(!s->scratchOwner || !s->scratchOwner->writable || !s->contextOwner ||
           !registry.copySerializedGraphicsData(request.scratchPhysical(),32) ||
           !registry.copySerializedGraphicsData(request.callbackContext()-0xC0000000,4))return;
        const auto worker=registry.createNativeTileSceneInterruptWorker(device,request.callbackContext(),
            [&,s,same,device](const auto& thread,const auto& entry) {
                enqueueNextOnPooledHost(); // v240
                return executor.enqueue(thread.layout.threadAddress,[&,s,same,thread,entry,device] {
                    recomp::diag::timeline::mark(recomp::diag::timeline::HandoffWorkerStart,0,thread.layout.threadAddress); // v218
                    if(!s->armed)throw std::runtime_error("Scene IRQ entered without exclusive native handoff");
                    const auto owned=[&] {
                        return same(s->scratchOwner,registry.stoppedGraphicsPhysicalOwnership(s->request.scratchPhysical(),32)) &&
                            same(s->contextOwner,registry.stoppedGraphicsPhysicalOwnership(s->request.callbackContext()-0xC0000000,4)) &&
                            registry.snapshotNativeInterruptDeviceRoot(device)==std::optional{true};
                    };
                    // v187 DIAGNOSTIC: every refusal of the scene IRQ's memory / guest steps names its condition.
                    const auto why=[&](const char* step,const char* cond,unsigned a=0,unsigned b=0){
                        __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_SCENE_IRQ_STEP step=%s cond=%s a=%08x b=%08x worker=%08x",
                            step,cond,a,b,unsigned(s->result.worker));return false;};
                    const gpu::NativeTileCommandInterrupt::Memory memory=[&](const auto& op) {
                        if(!owned())return why("memory","owned");
                        if(op.target>=8 || (op.kind!=Kind::ScratchWrite && op.kind!=Kind::WaitScratch))return why("memory","kind",op.target,unsigned(op.kind));
                        if(std::find(s->request.operations().begin(),s->request.operations().end(),op)==s->request.operations().end())return why("memory","not-in-request",op.target,op.value);
                        const auto physical=s->request.scratchPhysical()+op.target*4;
                        const auto before=registry.copySerializedGraphicsData(physical,4);
                        if(!before || before->size()!=4)return why("memory","read",physical);
                        const unsigned value=(unsigned((*before)[0])<<24)|(unsigned((*before)[1])<<16)|
                            (unsigned((*before)[2])<<8)|(*before)[3];
                        if(op.kind==Kind::WaitScratch) {
                            if(value!=op.value)return why("memory","wait-value",op.value,value);++s->result.scratchWaits;return true;
                        }
                        std::vector<std::uint8_t> after(4);
                        for(unsigned b=0;b<4;++b)after[b]=op.value>>(24-b*8);
                        const auto address=s->scratchOwner->virtualBase+(physical-s->scratchOwner->physicalBase);
                        if(!registry.publishSerializedGraphicsData(address,*before,after))return why("memory","publish",address,op.value);
                        ++s->result.scratchWrites;return true;
                    };
                    const gpu::NativeTileCommandInterrupt::Guest guest=[&](const auto& actual) {
                        if(actual!=s->request)return why("guest","request");
                        if(s->result.guestReturns)return why("guest","returns",s->result.guestReturns);
                        if(!owned())return why("guest","owned-before");
                        bool originalReturned=false;
                        if(!registry.runNativeTileSceneInterruptScope(device,s->request.callbackContext(),
                            [&](const auto& f){return executor.runNativeNonYielding(f,4096);},
                            [&]{originalReturned=s->invoke(thread,entry);}))return why("guest","scope",unsigned(originalReturned));
                        if(!originalReturned)return why("guest","original-returned");
                        if(!owned())return why("guest","owned-after");
                        const auto status=registry.nativeTileSceneInterruptStatus();
                        if(!status.returned || !status.balanced || status.failed || status.held ||
                           status.worker!=s->result.worker || status.lockPairs!=1 || status.eventSignals!=1)
                            return why("guest","status",(unsigned(status.returned)<<4)|(unsigned(status.balanced)<<3)|(unsigned(status.failed)<<2)|unsigned(status.held),
                                (status.lockPairs<<8)|status.eventSignals);
                        ++s->result.guestReturns;return true;
                    };
                    const bool ownedBefore=owned();const bool executed=ownedBefore && s->execute(memory,guest);
                    const bool ownedAfter=executed && owned();
                    if(!ownedBefore || !executed || s->result.guestReturns!=1 ||
                       s->result.scratchWrites!=4 || s->result.scratchWaits!=4 || !ownedAfter) {
                        // v186 DIAGNOSTIC: the failing condition and the scope's last status (logged once per failure).
                        const auto status=registry.nativeTileSceneInterruptStatus();
                        __android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
                            "RENDER_DIAG_SCENE_IRQ_FAILED owned_before=%d executed=%d owned_after=%d guest_returns=%u scratch_writes=%u "
                            "scratch_waits=%u status_returned=%d balanced=%d failed=%d held=%d pairs=%u events=%u worker=%08x",
                            int(ownedBefore),int(executed),int(ownedAfter),unsigned(s->result.guestReturns),unsigned(s->result.scratchWrites),
                            unsigned(s->result.scratchWaits),int(status.returned),int(status.balanced),int(status.failed),int(status.held),
                            unsigned(status.lockPairs),unsigned(status.eventSignals),unsigned(s->result.worker));
                        throw std::runtime_error("Scene native packets/original IRQ did not complete; ring not retired");
                    }
                    if(!registry.markGuestThreadReturned(s->result.worker,0))
                        throw std::runtime_error("Scene IRQ owned worker return rejected");
                    s->result.returned=true;
                    recomp::diag::timeline::mark(recomp::diag::timeline::HandoffWorkerEnd,0,s->result.worker); // v218
                });
            });
        if(worker){s->result.worker=worker->layout.threadAddress;s->result.admitted=true;}
    },1);
    if(!admitted || !s->result.admitted)return s->result;
    recomp::diag::timeline::mark(recomp::diag::timeline::HandoffWorkerCreated,0,s->result.worker); // v218
    s->armed=true;
    // Deliberately OUTSIDE the non-yielding scope; this is the executor's
    // exclusive worker handoff, not an ordinary yield admitting other writers.
    const bool returned=executor.runNativeWorkerNext(s->result.worker,4096);
    s->armed=false;s->result.returned=returned && s->result.returned;
    recomp::diag::timeline::mark(recomp::diag::timeline::HandoffReturned,s->result.returned?1:0,s->result.worker); // v218
    return s->result;
}
// v279: the same scene IRQ without a worker thread and without running the guest ISR. Inside the same exclusive span and
// the same admission scope (ownership, packets, scratch writes/waits, counts as executeNativeTileSceneHandoff), the
// delivering task enters the registry's scene-IRQ scope itself and replays, natively and in the same order, exactly what
// the guest ISR sub_836B1768(source 1, device) and its registered callback sub_836BE1C8(context) do on CPU2
// (local-analysis corpus, read 2026-09-29): callback: slot = device+11576+2*108; entry (q&3) at slot+92 = context,
// slot+84 = q+1, KeSetEvent(slot+60, 1, 0); then KeAcquireSpinLockAtRaisedIrql(device+11032), scratch mask &= ~(1<<2) (&0x3F),
// KeReleaseSpinLockFromRaisedIrql(device+11032). The imports go through the registry's dispatch as the guest's would (the
// scope routes them to the current task), and the scope's own checks verify the result as they verify the guest ISR's:
// entry preconditions (mask 4, callback 0x836BE1C8, argument = context, CPU2 slot root/cpu, queue empty, event clear, lock
// free), the whole 21944-byte device equal to the expected image at every import and at the end, the PCR unchanged, the
// scratch mask cleared and the rest unchanged, one lock pair, one event. KeGetCurrentProcessType (the callback's device
// selection) has no effect; the scope's device is the one the guest callback's global selects (every validated run).
} // namespace recomp::bootstrap
#include "native_tile_scene_handoff_body.h" // v279 (the replay body; host tests include it alone)
namespace recomp::bootstrap {
inline NativeTileSceneHandoffResult executeNativeTileSceneInline(
        GuestImportRegistry& registry,GuestCooperativeExecutor& executor,unsigned device,
        const gpu::Gta5TileCommandInterrupt& request,const NativeTileScenePacketExecute& execute,std::uint8_t* base,std::uint32_t token) {
    using Kind=gpu::Gta5TileCommandInterrupt::Kind;
    NativeTileSceneHandoffResult result;
    if(!execute || !base)return result;
    const auto same=[](const auto& a,const auto& b) {
        return a && b && a->physicalBase==b->physicalBase && a->virtualBase==b->virtualBase &&
            a->byteCount==b->byteCount && a->protection==b->protection && a->writable==b->writable;
    };
    bool failed=false;
    const bool admitted=executor.runNativeNonYielding([&] {
        if(registry.snapshotNativeInterruptDeviceRoot(device)!=std::optional{true})return;
        const auto scratchOwner=registry.stoppedGraphicsPhysicalOwnership(request.scratchPhysical(),32);
        const auto contextOwner=registry.stoppedGraphicsPhysicalOwnership(request.callbackContext()-0xC0000000,4);
        if(!scratchOwner || !scratchOwner->writable || !contextOwner ||
           !registry.copySerializedGraphicsData(request.scratchPhysical(),32) ||
           !registry.copySerializedGraphicsData(request.callbackContext()-0xC0000000,4))return;
        result.admitted=true;result.worker=token; // (the delivering task stands where the worker stood)
        recomp::diag::timeline::mark(recomp::diag::timeline::HandoffWorkerCreated,1,token); // (arg 1: inline)
        recomp::diag::timeline::mark(recomp::diag::timeline::HandoffWorkerStart,1,token);
        const auto owned=[&] {
            return same(scratchOwner,registry.stoppedGraphicsPhysicalOwnership(request.scratchPhysical(),32)) &&
                same(contextOwner,registry.stoppedGraphicsPhysicalOwnership(request.callbackContext()-0xC0000000,4)) &&
                registry.snapshotNativeInterruptDeviceRoot(device)==std::optional{true};
        };
        const auto why=[&](const char* step,const char* cond,unsigned a=0,unsigned b=0){
            __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_SCENE_IRQ_STEP step=%s cond=%s a=%08x b=%08x worker=%08x inline=1",
                step,cond,a,b,token);return false;};
        const gpu::NativeTileCommandInterrupt::Memory memory=[&](const auto& op) { // (executeNativeTileSceneHandoff's, unchanged)
            if(!owned())return why("memory","owned");
            if(op.target>=8 || (op.kind!=Kind::ScratchWrite && op.kind!=Kind::WaitScratch))return why("memory","kind",op.target,unsigned(op.kind));
            if(std::find(request.operations().begin(),request.operations().end(),op)==request.operations().end())return why("memory","not-in-request",op.target,op.value);
            const auto physical=request.scratchPhysical()+op.target*4;
            const auto before=registry.copySerializedGraphicsData(physical,4);
            if(!before || before->size()!=4)return why("memory","read",physical);
            const unsigned value=(unsigned((*before)[0])<<24)|(unsigned((*before)[1])<<16)|(unsigned((*before)[2])<<8)|(*before)[3];
            if(op.kind==Kind::WaitScratch) {
                if(value!=op.value)return why("memory","wait-value",op.value,value);++result.scratchWaits;return true;
            }
            std::vector<std::uint8_t> after(4);
            for(unsigned b=0;b<4;++b)after[b]=op.value>>(24-b*8);
            const auto address=scratchOwner->virtualBase+(physical-scratchOwner->physicalBase);
            if(!registry.publishSerializedGraphicsData(address,*before,after))return why("memory","publish",address,op.value);
            ++result.scratchWrites;return true;
        };
        const gpu::NativeTileCommandInterrupt::Guest guest=[&](const auto& actual) {
            if(actual!=request)return why("guest","request");
            if(result.guestReturns)return why("guest","returns",result.guestReturns);
            if(!owned())return why("guest","owned-before");
            bool bodyReturned=false;
            recomp_v279_scene_inline(true); // (the registry's scene scope validates the delivering task, see there)
            struct InlineOff {~InlineOff(){recomp_v279_scene_inline(false);}} inlineOff;
            // (the admission scope is this task's non-yielding scope already: the registry's gate runs the body directly)
            if(!registry.runNativeTileSceneInterruptScope(device,request.callbackContext(),
                [](const std::function<void()>& f){f();return true;},
                [&]{bodyReturned=nativeSceneInterruptBody(registry,base,device,request.callbackContext());}))
                return why("guest","scope",unsigned(bodyReturned));
            if(!bodyReturned)return why("guest","body-returned");
            if(!owned())return why("guest","owned-after");
            const auto status=registry.nativeTileSceneInterruptStatus();
            if(!status.returned || !status.balanced || status.failed || status.held ||
               status.worker!=token || status.lockPairs!=1 || status.eventSignals!=1)
                return why("guest","status",(unsigned(status.returned)<<4)|(unsigned(status.balanced)<<3)|(unsigned(status.failed)<<2)|unsigned(status.held),
                    (status.lockPairs<<8)|status.eventSignals);
            ++result.guestReturns;return true;
        };
        const bool ownedBefore=owned();const bool executed=ownedBefore && execute(memory,guest);
        const bool ownedAfter=executed && owned();
        if(!ownedBefore || !executed || result.guestReturns!=1 || result.scratchWrites!=4 || result.scratchWaits!=4 || !ownedAfter) {
            const auto status=registry.nativeTileSceneInterruptStatus();
            __android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
                "RENDER_DIAG_SCENE_IRQ_FAILED owned_before=%d executed=%d owned_after=%d guest_returns=%u scratch_writes=%u "
                "scratch_waits=%u status_returned=%d balanced=%d failed=%d held=%d pairs=%u events=%u worker=%08x inline=1",
                int(ownedBefore),int(executed),int(ownedAfter),unsigned(result.guestReturns),unsigned(result.scratchWrites),
                unsigned(result.scratchWaits),int(status.returned),int(status.balanced),int(status.failed),int(status.held),
                unsigned(status.lockPairs),unsigned(status.eventSignals),token);
            failed=true;return;
        }
        result.returned=true;
        recomp::diag::timeline::mark(recomp::diag::timeline::HandoffWorkerEnd,1,token);
    },1);
    if(failed)throw std::runtime_error("Scene native packets/inline IRQ did not complete; ring not retired");
    if(admitted && result.admitted)recomp::diag::timeline::mark(recomp::diag::timeline::HandoffReturned,result.returned?1:0,token);
    return result;
}
// v282: the scene IRQ with no world stop. The caller holds the renderer gate (runNativeLockedGate: native packets and every
// D3D hook body serialized, as the exclusive span serialized them) but no exclusive scope: other guest threads keep
// running. Ownership and the scratch writes/waits are executeNativeTileSceneInline's (each under the registry lock);
// the ISR's effect is one registry-lock section (__recomp_v282_scene_irq in guest_import_registry.cpp), retried while the
// raised-IRQL lock +11032 is held by a running guest thread (the console's ISR spins on it), for at most 20 ms (then a
// loud failure, never a hang). Not admitted (device root / ownership refused): nothing ran, the caller may use another
// path.
inline NativeTileSceneHandoffResult executeNativeTileSceneConcurrent(
        GuestImportRegistry& registry,unsigned device,const gpu::Gta5TileCommandInterrupt& request,
        const NativeTileScenePacketExecute& execute,std::uint32_t token,unsigned& busyRetries) {
    using Kind=gpu::Gta5TileCommandInterrupt::Kind;
    NativeTileSceneHandoffResult result;
    if(!execute || registry.snapshotNativeInterruptDeviceRoot(device)!=std::optional{true})return result;
    const auto same=[](const auto& a,const auto& b) {
        return a && b && a->physicalBase==b->physicalBase && a->virtualBase==b->virtualBase &&
            a->byteCount==b->byteCount && a->protection==b->protection && a->writable==b->writable;
    };
    const auto scratchOwner=registry.stoppedGraphicsPhysicalOwnership(request.scratchPhysical(),32);
    const auto contextOwner=registry.stoppedGraphicsPhysicalOwnership(request.callbackContext()-0xC0000000,4);
    if(!scratchOwner || !scratchOwner->writable || !contextOwner ||
       !registry.copySerializedGraphicsData(request.scratchPhysical(),32) ||
       !registry.copySerializedGraphicsData(request.callbackContext()-0xC0000000,4))return result;
    result.admitted=true;result.worker=token;
    recomp::diag::timeline::mark(recomp::diag::timeline::HandoffWorkerCreated,2,token); // (arg 2: concurrent)
    recomp::diag::timeline::mark(recomp::diag::timeline::HandoffWorkerStart,2,token);
    const auto owned=[&] {
        return same(scratchOwner,registry.stoppedGraphicsPhysicalOwnership(request.scratchPhysical(),32)) &&
            same(contextOwner,registry.stoppedGraphicsPhysicalOwnership(request.callbackContext()-0xC0000000,4)) &&
            registry.snapshotNativeInterruptDeviceRoot(device)==std::optional{true};
    };
    const auto why=[&](const char* step,const char* cond,unsigned a=0,unsigned b=0){
        __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_SCENE_IRQ_STEP step=%s cond=%s a=%08x b=%08x worker=%08x inline=2",
            step,cond,a,b,token);return false;};
    const gpu::NativeTileCommandInterrupt::Memory memory=[&](const auto& op) { // (executeNativeTileSceneHandoff's, unchanged)
        if(!owned())return why("memory","owned");
        if(op.target>=8 || (op.kind!=Kind::ScratchWrite && op.kind!=Kind::WaitScratch))return why("memory","kind",op.target,unsigned(op.kind));
        if(std::find(request.operations().begin(),request.operations().end(),op)==request.operations().end())return why("memory","not-in-request",op.target,op.value);
        const auto physical=request.scratchPhysical()+op.target*4;
        const auto before=registry.copySerializedGraphicsData(physical,4);
        if(!before || before->size()!=4)return why("memory","read",physical);
        const unsigned value=(unsigned((*before)[0])<<24)|(unsigned((*before)[1])<<16)|(unsigned((*before)[2])<<8)|(*before)[3];
        if(op.kind==Kind::WaitScratch) {
            if(value!=op.value)return why("memory","wait-value",op.value,value);++result.scratchWaits;return true;
        }
        std::vector<std::uint8_t> after(4);
        for(unsigned b=0;b<4;++b)after[b]=op.value>>(24-b*8);
        const auto address=scratchOwner->virtualBase+(physical-scratchOwner->physicalBase);
        if(!registry.publishSerializedGraphicsData(address,*before,after))return why("memory","publish",address,op.value);
        ++result.scratchWrites;return true;
    };
    const gpu::NativeTileCommandInterrupt::Guest guest=[&](const auto& actual) {
        if(actual!=request)return why("guest","request");
        if(result.guestReturns)return why("guest","returns",result.guestReturns);
        if(!owned())return why("guest","owned-before");
        const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(20);
        for(;;) {
            GuestImportCall c;c.arguments[0]=device;c.arguments[1]=request.callbackContext();
            const auto r=registry.dispatch("__recomp_v282_scene_irq",c);
            if(!r.handled())return why("guest","dispatch");
            if(c.returnValue==0)break;
            if(c.returnValue==1 && std::chrono::steady_clock::now()<until){++busyRetries;std::this_thread::yield();continue;}
            return why("guest","v282",unsigned(c.returnValue));
        }
        if(!owned())return why("guest","owned-after");
        ++result.guestReturns;return true;
    };
    const bool executed=execute(memory,guest);
    if(!executed || result.guestReturns!=1 || result.scratchWrites!=4 || result.scratchWaits!=4 || !owned()) {
        __android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
            "RENDER_DIAG_SCENE_IRQ_FAILED executed=%d guest_returns=%u scratch_writes=%u scratch_waits=%u worker=%08x inline=2",
            int(executed),unsigned(result.guestReturns),unsigned(result.scratchWrites),unsigned(result.scratchWaits),token);
        throw std::runtime_error("Scene native packets/concurrent IRQ did not complete; ring not retired");
    }
    result.returned=true;
    recomp::diag::timeline::mark(recomp::diag::timeline::HandoffWorkerEnd,2,token);
    recomp::diag::timeline::mark(recomp::diag::timeline::HandoffReturned,1,token);
    return result;
}
}
