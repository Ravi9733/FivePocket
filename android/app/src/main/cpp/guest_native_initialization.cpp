// v247: post-output colour precheck (see submitExperimentalPostOutputColorBatch): A/B switch and counters.
#include <atomic>
extern "C" __attribute__((weak)) std::atomic<bool> recomp_v247_baseline; // (generic_renderer.cpp switch poll)
std::atomic<unsigned long long> v247Prechecks{0},v247Refused{0};
extern "C" unsigned long long recomp_v247_prechecks() noexcept { return v247Prechecks.load(std::memory_order_relaxed); }
extern "C" unsigned long long recomp_v247_refused() noexcept { return v247Refused.load(std::memory_order_relaxed); }
#include "cs_range_query.h" // v219
#include <atomic>
#include <cstddef>
#include <cstdint>
// v232: the published-copy continuity checks compare runs of active words (NativeTiledCopyPublication::
// matchesPublishedOwnedMemoryRuns, same result; android/tests/native_tiled_copy_publication_runs_test.cpp). v226 house
// profile: matchesPublishedOwnedMemory 2.2% of the render thread. diag-v232-baseline (applied by the renderer every 60
// presents): the word-by-word compare.
extern "C" __attribute__((weak)) std::atomic<bool> recomp_v232_baseline; // (defined in generic_renderer.cpp; absent in host tests)
namespace {
template<class Copy> bool v232Matches(const Copy& copy,std::uint32_t physicalBase,const std::uint8_t* destination,std::size_t bytes) {
    return (&recomp_v232_baseline && recomp_v232_baseline.load(std::memory_order_relaxed))?copy.matchesPublishedOwnedMemory(physicalBase,destination,bytes):
        copy.matchesPublishedOwnedMemoryRuns(physicalBase,destination,bytes);
}
}
#include "guest_import_registry.h"
#include "xenos_native_initialization.h"
#include "xenos_native_state_batch.h"
#include "xenos_maintenance_point_plan.h"
#include "xenos_side_effect_free_batch.h"
#include "native_gpu_output_publication.h"
#include "native_color_resolved_memory.h"
#include "native_color_state_commit.h"
#include "native_color_output_publication.h"
#include "native_swap_image.h"
#include "native_deferred_color_write_guard.h"
#include "native_deferred_color_state_commit.h"
#include "gta5_native_tile_initial_queue.h"
#include "gta5_native_tile_first_scene.h"
#include "gta5_native_tile_final_scene.h"
#include <type_traits>
#include <cstdlib>
#include <cstring>
#include "guest_registry_concurrency.h" // v42, last include: every R holder is a registry section.

namespace recomp::bootstrap {
// Defined by guest_import_registry.cpp; available in cooperative and threaded builds.
void noteNativeSubmissionMayCommitAt(unsigned site) noexcept;
}

// The guest thread that called the current registry member. Parallel builds (RECOMP_GUEST_THREADS
// > 0, with the attempt's parallel guest_import_registry.cpp) resolve it exactly as the registry's
// own RECOMP_CURRENT_GUEST_TOKEN: the calling host thread's binding while it executes guest code.
// The registry-wide member is rebound by every task resume in every permit domain, so in lanes mode
// it names whichever task another domain resumed last (lanes audit L1, v22 VdSwap rejection).
// The default (cooperative) build keeps reading the member, byte for byte as before.
#if defined(RECOMP_GUEST_THREADS) && RECOMP_GUEST_THREADS > 0
namespace recomp::bootstrap {
// v183: calls the graphics-submission handler's barrier hook (guest_import_registry.cpp) when a native submission
// handler has passed its read-only admission checks and may publish completion (fences, the read pointer).
void noteNativeSubmissionMayCommit() noexcept;
void noteNativeSubmissionMayCommitAt(unsigned site) noexcept;
std::uint32_t guestImportRegistryCallerToken(const GuestImportRegistry* registry, std::uint32_t member) noexcept;
}
#define RECOMP_REGISTRY_CALLER_TOKEN guestImportRegistryCallerToken(this, boundGuestThreadToken_)
#else
#define RECOMP_REGISTRY_CALLER_TOKEN boundGuestThreadToken_
#endif

namespace recomp::bootstrap {
bool GuestImportRegistry::retireNativeTileInitialQueue(const recomp::gpu::Gta5NativeTileInitialQueue& initial,
        const NativeSerializedGate& gate) {
    if(!gate || !initial.packetsCompleted())return false;
    return retireNativeTileReturnedQueue(initial.queue(),initial.interrupt(),gate);
}
bool GuestImportRegistry::retireNativeTileFirstScene(const recomp::gpu::Gta5NativeTileFirstScene& scene,
        const NativeSerializedGate& gate) {
    if(!gate || !scene.packetsCompleted())return false;
    return retireNativeTileReturnedQueue(scene.queue(),scene.interrupt(),gate);
}
bool GuestImportRegistry::retireNativeTileFinalScene(const recomp::gpu::Gta5NativeTileFinalScene& scene,
        const NativeSerializedGate& gate) {
    if(!gate || !scene.packetsCompleted() || !scene.deferred())return false;
    return retireNativeTileReturnedQueue(scene.queue(),scene.interrupt(),gate,&*scene.deferred());
}
bool GuestImportRegistry::retireNativeTileReturnedQueue(const recomp::gpu::Gta5NativeTileSubmissionCapture& q,
        const recomp::gpu::Gta5TileCommandInterrupt& request,const NativeSerializedGate& gate,
        const recomp::gpu::Gta5TileDeferredTail* deferred) {
    bool entered=false,completed=false;
    const bool gated=gate([&] {
        if(entered)return;
        entered=true;
        RECOMP_REGISTRY_LOCK(lock);
        if(!addressSpace_ || !graphicsInitialized_ || !graphicsBackend_ || !graphicsBackend_->ready() ||
           !graphicsRingConfiguration_ || !graphicsRingAllocation_ || !graphicsWriteBackAllocation_ ||
           !nativeInterruptDeviceRoot_ || !nativeTileSceneWorker_ || nativeTileSceneInterruptScope_ ||
           nativeInterruptScope_ || nativeTileLockScope_)return;
        const auto& irq=nativeTileSceneInterruptStatus_;const auto& worker=*nativeTileSceneWorker_;
        const auto* issuer=findThreadObject(RECOMP_REGISTRY_CALLER_TOKEN);
        const auto* returned=findThreadObject(worker.token);
        if(!issuer || issuer->returned || issuer->suspendCount || RECOMP_REGISTRY_CALLER_TOKEN==worker.token ||
           !returned || !returned->returned || !irq.attempted || !irq.returned || !irq.balanced || irq.failed || irq.held ||
           irq.lockPairs!=1 || irq.eventSignals!=1 || irq.worker!=worker.token || irq.device!=q.device() ||
           worker.device!=q.device() || worker.context!=request.callbackContext())return;
        if(deferred) {
            // The second scene skips its IRQ: it must follow the first scene's
            // already consumed, real returned worker, never invent another IRQ.
            if(retiredNativeTileInitialWorker_!=worker.token || retiredNativeTileFirstWorker_!=worker.token ||
               retiredNativeTileFinalWorker_==worker.token)return;
        } else if(retiredNativeTileInitialWorker_==worker.token)return;
        auto& ring=*graphicsRingConfiguration_;
        if(!ring.writeBack || ring.virtualAddress!=q.ringVirtual() || ring.bytes!=q.ringBytes() ||
           ring.physicalAddress!=q.ringVirtual()-0xA0000000 || ring.readWord!=q.entryReadWord() ||
           ring.writeBack->virtualAddress!=q.readVirtual() ||
           ring.writeBack->physicalAddress!=q.readVirtual()-0xA0000000)return;
        const auto ownedRing=resolvePhysicalRange(ring.physicalAddress,ring.bytes,false);
        const auto ownedRead=resolvePhysicalRange(ring.writeBack->physicalAddress,4,true);
        if(!ownedRing || ownedRing->allocationBase!=*graphicsRingAllocation_ || ownedRing->virtualAddress!=ring.virtualAddress ||
           !ownedRead || ownedRead->allocationBase!=*graphicsWriteBackAllocation_ || ownedRead->virtualAddress!=q.readVirtual())return;
        try {
            if(!nativeInterruptDeviceRoot_(q.device()))return;
            // Readers run under this same allocation lock, not the public
            // locking readers. All generated guest writers remain parked.
            const recomp::gpu::Gta5ResourceReader heap=[&](unsigned at,unsigned n)->std::optional<std::vector<std::uint8_t>> {
                if(!n || n>65536 || at<heapBase_ || std::uint64_t{at}+n>std::uint64_t{heapBase_}+heapSize_ ||
                   !validGuestDataBuffer(at,n,false))return std::nullopt;
                const auto* p=addressSpace_->translate(at);return std::vector<std::uint8_t>(p,p+n);
            };
            const recomp::gpu::Gta5ResourceReader graphics=[&](unsigned at,unsigned n)->std::optional<std::vector<std::uint8_t>> {
                if(!n || n>65536 || (at&3))return std::nullopt;
                if(at>=0xA0000000 && std::uint64_t{at}+n<=0xC0000000)at-=0xA0000000;
                if(std::uint64_t{at}+n>0x20000000)return std::nullopt;
                const auto range=resolvePhysicalRange(at,n,false);if(!range)return std::nullopt;
                const auto* p=addressSpace_->translate(range->virtualAddress);return std::vector<std::uint8_t>(p,p+n);
            };
            if(!q.matchesCurrent(heap,graphics))return;
            unsigned scratch=0,ack=1,poison=0,context=0;
            const auto ownedScratch=resolvePhysicalRange(request.scratchPhysical(),32,true);
            if(!ownedScratch || !readU32(q.device()+11028,scratch) || scratch!=ownedScratch->virtualAddress ||
               !resolvePhysicalRange(request.callbackContext()-0xC0000000,4,false) ||
               !readU32(scratch,ack) || ack!=(deferred?4u:0u) || !readU32(scratch+16,poison) || poison!=0x0BADF00D ||
               !readU32(scratch+20,context) || context!=request.callbackContext())return;
            if(deferred) {
                const auto state=heap(q.device(),21944);
                const auto ownedFence=resolvePhysicalRange(deferred->fencePhysical(),8,true);
                unsigned value=0,cursor=0;
                if(!state || !ownedFence || ownedFence->allocationBase!=*graphicsWriteBackAllocation_ ||
                   ownedFence->virtualAddress+60!=q.readVirtual() ||
                   !deferred->matchesProducedState(*state,ownedFence->virtualAddress,deferred->fencePhysical()) ||
                   !readU32(ownedFence->virtualAddress,value) || value!=deferred->completionValue() ||
                   !readU32(ownedFence->virtualAddress+4,cursor) || cursor!=deferred->commandCursor())return;
            }
            // This is the last fallible action. It does not signal a scene
            // fence/event or modify any specialized state-batch history.
            if(!writeU32(q.readVirtual(),q.lastWriteWord()))return;
            ring.readWord=q.lastWriteWord();
            if(deferred)retiredNativeTileFinalWorker_=worker.token;
            else {
                retiredNativeTileInitialWorker_=worker.token;
                if(request.predicated())retiredNativeTileFirstWorker_=worker.token;
            }
            completed=true;
        } catch (...) { return; }
    });
    return gated && entered && completed;
}
bool GuestImportRegistry::bindExperimentalOwnedColorBatch() {
    RECOMP_REGISTRY_LOCK(lock);
    if(!nativeStateBatchesBound_ || graphicsInitialized_ || experimentalOwnedColorBound_)return false;
    experimentalOwnedColorBound_=true;return true;
}
std::optional<GuestImportRegistry::NativeOwnedColorCompletion> GuestImportRegistry::completedNativeOwnedColorBatch() const {
    RECOMP_REGISTRY_LOCK(lock);return completedOwnedColor_;
}
bool GuestImportRegistry::bindNativeNoSystemOverlay() {
    RECOMP_REGISTRY_LOCK(lock);
    if(!experimentalOwnedColorBound_ || graphicsInitialized_ || nativeNoSystemOverlayBound_)return false;
    nativeNoSystemOverlayBound_=true;return true;
}
std::optional<GuestImportRegistry::NativeSystemContext> GuestImportRegistry::nativeSystemContext() const {
    RECOMP_REGISTRY_LOCK(lock);return nativeSystemContext_;
}
std::optional<GuestImportRegistry::NativeSystemContext> GuestImportRegistry::nativePostOutputSystemContext() const {
    RECOMP_REGISTRY_LOCK(lock);return postOutputSystemContext_;
}
std::optional<GuestImportRegistry::NativeSystemContext> GuestImportRegistry::nativePostSwapSystemContext() const {
    RECOMP_REGISTRY_LOCK(lock);return postSwapSystemContext_;
}
bool GuestImportRegistry::issueNativePostOutputSystemContextLocked(const GuestImportCall& call,bool afterSwap) {
    using Plan=recomp::gpu::NativeSwapImagePlan;
    auto& context=afterSwap?postSwapSystemContext_:postOutputSystemContext_;
    if(!nativeNoSystemOverlayBound_ || !nativeSystemContext_ || context || !nativeSwapConsumer_ ||
        !completedWrappedSwap_ || pendingNativeSwap_ || nativeSwapStatus_.pending || !completedPostOutputColor_ ||
        !pendingPostOutputColorImage_ || !graphicsSystemIdentifier_ || !validateNativeScalerStorageLocked() ||
        !nativeDisplayModeStatus_.selected || nativeDisplayModeStatus_.queries!=1 ||
        RECOMP_REGISTRY_CALLER_TOKEN!=nativeSystemContextThread_)return false;
    const auto& ring=*graphicsRingConfiguration_;const auto& copy=*pendingPostOutputColorImage_;
    if(afterSwap){
        if(!postOutputSystemContext_ || !validateNativePostOutputScanoutLocked() || call.arguments[0]>UINT32_MAX ||
            call.arguments[1]>UINT32_MAX)return false;
    }else if(ring.readWord!=completedPostOutputColor_->color.readWord || copy.readWord!=ring.readWord)return false;
    std::uint32_t read=0;if(!readU32(ring.writeBack->virtualAddress,read) || read!=ring.readWord)return false;
    const auto mapped=resolvePhysicalRange(copy.physicalBase,Plan::Capacity,true);
    const auto allocation=physicalAllocations_.find(copy.allocationBase);
    if(!mapped || mapped->allocationBase!=copy.allocationBase || mapped->virtualAddress!=copy.virtualBase ||
        allocation==physicalAllocations_.end() || allocation->second.size!=copy.allocationBytes ||
        allocation->second.protection!=copy.protection ||
        !v232Matches(copy.copy,copy.physicalBase,addressSpace_->translate(copy.virtualBase),Plan::Capacity))return false;
    const auto sp=std::uint64_t{call.stackPointer};const auto buffer=call.argument32(0),identifier=call.argument32(1);
    if(sp!=nativeSystemContext_->stackPointer || !sp || (sp&15) || sp+480>GuestAddressSpace::Size ||
        sp+208!=buffer || sp+116!=identifier || !validGuestDataBuffer(call.stackPointer,480,true,16) ||
        imageProtections_.overlaps(call.stackPointer,480) ||
        (sp<std::uint64_t{heapBase_}+heapSize_ && sp+480>heapBase_) ||
        (physicalMemoryArenaBound_ && sp<std::uint64_t{physicalArenaBase_}+physicalArenaSize_ && sp+480>physicalArenaBase_))return false;
    const auto* thread=findThreadObject(RECOMP_REGISTRY_CALLER_TOKEN);
    if(!thread || thread->returned || thread->suspendCount)return false;
    if(!thread->initial){const auto stack=threadStorage_.lookup(thread->threadId);
        if(!stack || sp<stack->layout.stackBase || sp+480>std::uint64_t{stack->layout.stackBase}+stack->layout.stackSize)return false;}
    for(const auto& cs:criticalSections_)
        if(sp<std::uint64_t{cs.first}+28 && cs.first<sp+480)return false;
    if(hasDispatcherWaitInRange(call.stackPointer,480))return false;
    const NativeSystemContext next{call.stackPointer,buffer,identifier,read,1};
    // All fallible work precedes the two exact outputs. This does not modify
    // the original issuance receipt, GPU identifier, copy, scanout or RPTR.
    std::memset(addressSpace_->translate(buffer),0,148);
    (void)writeU32(identifier,0);
    context=next;return true;
}
std::optional<GuestImportRegistry::StoppedNativeSwapRequest> GuestImportRegistry::inspectStoppedNativeSwapRequest(
        const GuestImportCall& call) const {
    RECOMP_REGISTRY_LOCK(lock);
    return inspectNativeSwapRequestLocked(call);
}
std::optional<GuestImportRegistry::StoppedNativeSwapRequest> GuestImportRegistry::inspectStoppedPostOutputSwapRequest(
        const GuestImportCall& call) const {
    RECOMP_REGISTRY_LOCK(lock);
    return inspectNativeSwapRequestLocked(call,true);
}
std::optional<GuestImportRegistry::StoppedNativeSwapRequest> GuestImportRegistry::inspectNativeSwapRequestLocked(
        const GuestImportCall& call,bool postOutput,bool afterSwap) const {
    if(!nativeNoSystemOverlayBound_ || !nativeSystemContext_ || !completedOwnedColor_ || !addressSpace_ ||
        !graphicsSystemIdentifier_ || !graphicsIdentifierAllocation_ || !physicalMemoryArenaBound_ ||
        !graphicsRingConfiguration_ || !graphicsRingConfiguration_->writeBack)return std::nullopt;
    if(afterSwap && (!postOutput || !postSwapSystemContext_ || !validateNativePostOutputScanoutLocked()))return std::nullopt;
    if(postOutput && (!postOutputSystemContext_ || !completedPostOutputColor_ || !pendingPostOutputColorImage_ ||
        !completedWrappedSwap_ || pendingNativeSwap_ || nativeSwapStatus_.pending || !nativeSwapConsumer_ ||
        pendingPostOutputSwap_ || (!afterSwap && postOutputSwapStatus_.issued) || !validateNativeScalerStorageLocked()))return std::nullopt;
    const auto& context=afterSwap?*postSwapSystemContext_:(postOutput?*postOutputSystemContext_:*nativeSystemContext_);
    const auto& ring=*graphicsRingConfiguration_;
    const auto copyRead=afterSwap?completedPostOutputWrappedSwap_->readWord:(postOutput?completedPostOutputColor_->color.readWord:completedOwnedColor_->readWord);
    const auto sp=std::uint64_t{context.stackPointer};
    if(call.stackPointer!=sp || context.readWord!=ring.readWord || ring.readWord!=copyRead ||
        call.argument32(1)!=sp+128 || call.argument32(2)!=*graphicsSystemIdentifier_ || call.argument32(3)!=context.buffer ||
        call.argument32(4)!=0 || call.argument32(5)!=sp+108 || call.argument32(6)!=sp+112 || call.argument32(7)!=sp+104)
        return std::nullopt;
    const auto thread=threadObjects_.find(nativeSystemContextThread_);
    if(thread==threadObjects_.end() || (afterSwap && (thread->second.returned || thread->second.suspendCount)))return std::nullopt;
    if(!thread->second.initial){const auto owner=threadStorage_.lookup(thread->second.threadId);
        if(!owner || sp<owner->layout.stackBase || sp+480>std::uint64_t{owner->layout.stackBase}+owner->layout.stackSize)
            return std::nullopt;
    }
    // Frame already validated when issued; its owner must still exist. Read
    // only exact bounded fields, independently of the executor's stopped token.
    std::uint32_t widthPointer=0,heightPointer=0,systemIdentifier=1,currentRead=0;
    if(!readU32(context.stackPointer+84,widthPointer) || widthPointer!=sp+96 ||
        !readU32(context.stackPointer+92,heightPointer) || heightPointer!=sp+100 ||
        !readU32(context.identifierOutput,systemIdentifier) || systemIdentifier ||
        !readU32(ring.writeBack->virtualAddress,currentRead) || currentRead!=ring.readWord)return std::nullopt;
    const auto* empty=addressSpace_->translate(context.buffer);
    for(unsigned i=0;i<148;++i)if(empty[i])return std::nullopt;
    const auto command=call.argument32(0);
    const auto resolveVirtual=[&](std::uint32_t address,std::uint32_t bytes){
        if(address<physicalArenaBase_ || std::uint64_t{address}+bytes>std::uint64_t{physicalArenaBase_}+physicalArenaSize_)
            return decltype(resolvePhysicalRange(0,0,true)){};
        const auto physical=std::uint64_t{physicalAddressOffset_}+address-physicalArenaBase_;
        if((physical&3) || physical+bytes>0x20000000)return decltype(resolvePhysicalRange(0,0,true)){};
        return resolvePhysicalRange(static_cast<std::uint32_t>(physical),bytes,true);
    };
    const auto commandOwner=resolveVirtual(command,256),identifierOwner=resolveVirtual(*graphicsSystemIdentifier_,4);
    if(!commandOwner || !identifierOwner || identifierOwner->allocationBase!=*graphicsIdentifierAllocation_)return std::nullopt;
    const auto overlapsCommand=[&](std::uint32_t a,std::uint32_t n){
        return std::uint64_t{command}<std::uint64_t{a}+n && std::uint64_t{a}<std::uint64_t{command}+256;
    };
    if(overlapsCommand(ring.virtualAddress,ring.bytes) || overlapsCommand(ring.writeBack->virtualAddress,4) ||
        overlapsCommand(*graphicsSystemIdentifier_,4))return std::nullopt;
    if(postOutput){
        if(commandOwner->allocationBase!=nativeScalerStorageOwner_->virtualBase ||
            hasDispatcherWaitInRange(command,256) || hasDispatcherWaitInRange(context.stackPointer,480))return std::nullopt;
        for(const auto& cs:criticalSections_)
            if(overlapsCommand(cs.first,28) || (sp<std::uint64_t{cs.first}+28 && cs.first<sp+480))return std::nullopt;
    }
    const auto allocation=physicalAllocations_.find(commandOwner->allocationBase);
    if(allocation==physicalAllocations_.end())return std::nullopt;
    StoppedNativeSwapRequest result{};
    result.commandVirtual=command;result.commandPhysical=physicalAddressOffset_+(command-physicalArenaBase_);
    result.allocationBase=commandOwner->allocationBase;result.allocationBytes=allocation->second.size;
    result.protection=allocation->second.protection;
    (void)readU32(context.stackPointer+108,result.frontbufferVirtual);(void)readU32(context.stackPointer+112,result.format);
    (void)readU32(context.stackPointer+104,result.colorSpace);(void)readU32(widthPointer,result.width);(void)readU32(heightPointer,result.height);
    (void)readU32(*graphicsSystemIdentifier_,result.identifierWord);
    for(unsigned i=0;i<6;++i)(void)readU32(context.stackPointer+128+i*4,result.fetch[i]);
    return result; // Raw owned metadata, not a validated rendering/presentation request.
}
std::optional<GuestImportRegistry::StoppedNativeSwapRequest> GuestImportRegistry::inspectStoppedPostSwapRequest(const GuestImportCall& call) const {
    RECOMP_REGISTRY_LOCK(lock);return inspectNativeSwapRequestLocked(call,true,true);
}
bool GuestImportRegistry::visitNativeOwnedSwapImage(const GuestImportCall& call,
        const std::function<bool(const NativeOwnedSwapImage&)>& visitor) const {
    RECOMP_REGISTRY_LOCK(lock);
    return visitNativeOwnedSwapImageLocked(call,visitor);
}
bool GuestImportRegistry::visitStoppedPostOutputSwapImage(const GuestImportCall& call,
        const std::function<bool(const NativeOwnedSwapImage&)>& visitor) const {
    RECOMP_REGISTRY_LOCK(lock);
    return visitNativeOwnedSwapImageLocked(call,visitor,true);
}
bool GuestImportRegistry::visitNativeOwnedSwapImageLocked(const GuestImportCall& call,
        const std::function<bool(const NativeOwnedSwapImage&)>& visitor,bool postOutput) const {
    using Plan=recomp::gpu::NativeSwapImagePlan;
    const auto& selected=postOutput?pendingPostOutputColorImage_:completedOwnedColorImage_;
    if(!visitor || !selected || !graphicsInitialized_ || !graphicsBackend_ || !graphicsBackend_->ready())return false;
    const auto request=inspectNativeSwapRequestLocked(call,postOutput);
    if(!request)return false;
    const auto& receipt=*selected;
    if(request->frontbufferVirtual!=receipt.virtualBase || receipt.readWord!=graphicsRingConfiguration_->readWord)return false;
    const auto plan=Plan::make(request->fetch,receipt.virtualBase,receipt.physicalBase,request->format,
        request->colorSpace,request->width,request->height);
    if(!plan)return false;
    const auto mapped=resolvePhysicalRange(receipt.physicalBase,Plan::Capacity,true);
    const auto allocation=physicalAllocations_.find(receipt.allocationBase);
    if(!mapped || mapped->allocationBase!=receipt.allocationBase || mapped->virtualAddress!=receipt.virtualBase ||
        allocation==physicalAllocations_.end() || allocation->second.size!=receipt.allocationBytes ||
        allocation->second.protection!=receipt.protection)return false;
    const auto overlaps=[&](std::uint32_t address,std::uint32_t bytes){
        return std::uint64_t{receipt.virtualBase}<std::uint64_t{address}+bytes &&
            std::uint64_t{address}<std::uint64_t{receipt.virtualBase}+Plan::Capacity;
    };
    const auto& ring=*graphicsRingConfiguration_;
    if(overlaps(request->commandVirtual,256) || overlaps(ring.virtualAddress,ring.bytes) ||
        overlaps(ring.writeBack->virtualAddress,4) || overlaps(*graphicsSystemIdentifier_,4))return false;
    if(postOutput){
        if(hasDispatcherWaitInRange(receipt.virtualBase,Plan::Capacity))return false;
        for(const auto& cs:criticalSections_)if(overlaps(cs.first,28))return false;
    }
    const auto* pixels=addressSpace_->translate(mapped->virtualAddress);
    if(!v232Matches(receipt.copy,receipt.physicalBase,pixels,Plan::Capacity))return false;
    try {
        NativeOwnedSwapImage image{*request,
            {physicalAddressOffset_+receipt.allocationBase-physicalArenaBase_,receipt.allocationBase,
                receipt.allocationBytes,receipt.protection,true},receipt.physicalBase,{}};
        if(!plan->decode(pixels,Plan::Capacity,image.rgba))return false;
        return visitor(image);
    }catch(...){return false;}
}
bool GuestImportRegistry::bindExperimentalNativeSwapCommands(std::function<bool(const NativeOwnedSwapImage&)> consumer){
    RECOMP_REGISTRY_LOCK(lock);
    if(!consumer || !nativeNoSystemOverlayBound_ || graphicsInitialized_ || nativeSwapConsumer_)return false;
    nativeSwapConsumer_=std::move(consumer);return true;
}
GuestImportRegistry::NativeSwapCommandStatus GuestImportRegistry::nativeSwapCommandStatus() const {
    RECOMP_REGISTRY_LOCK(lock);return nativeSwapStatus_;
}
GuestImportRegistry::NativeSwapCommandStatus GuestImportRegistry::nativePostOutputSwapStatus() const {
    RECOMP_REGISTRY_LOCK(lock);return postOutputSwapStatus_;
}
GuestImportRegistry::NativeDisplayModeStatus GuestImportRegistry::nativeDisplayModeStatus() const {
    RECOMP_REGISTRY_LOCK(lock);return nativeDisplayModeStatus_;
}
bool GuestImportRegistry::issueExperimentalNativeSwapCommand(const GuestImportCall& call){
    RECOMP_REGISTRY_LOCK(lock);
    return issueExperimentalNativeSwapCommandLocked(call);
}
bool GuestImportRegistry::issueExperimentalNativeSwapCommandLocked(const GuestImportCall& call){
    if(completedPostOutputWrappedSwap_)return issueDeferredNativeSwapCommandLocked(call);
    if(completedPostOutputColor_)return issuePostOutputSwapCommandLocked(call);
    if(!nativeSwapConsumer_ || nativeSwapStatus_.issued || pendingNativeSwap_ ||
        RECOMP_REGISTRY_CALLER_TOKEN!=nativeSystemContextThread_ || !graphicsRingAllocation_ ||
        !graphicsWriteBackAllocation_ || !graphicsIdentifierAllocation_ || !graphicsSystemIdentifier_)return false;
    const auto commandAddress=std::uint64_t{call.argument32(0)};
    const auto overlaps=[&](std::uint32_t address,std::uint32_t bytes){
        return commandAddress<std::uint64_t{address}+bytes && std::uint64_t{address}<commandAddress+recomp::gpu::NativeSwapCommand::Bytes;
    };
    // Exact28byte critical-section /16byte event layouts used by this registry.
    for(const auto& entry:criticalSections_)if(overlaps(entry.first,28))return false;
    for(const auto& entry:dispatcherEventWaits_)if(overlaps(entry.first,16))return false;
    return visitNativeOwnedSwapImageLocked(call,[&](const auto& image){
        using Command=recomp::gpu::NativeSwapCommand;
        const auto command=Command::make({1,image.request.commandPhysical,image.physicalAddress,
            completedOwnedColorImage_->readWord});
        if(!command)return false;
        // All allocation/copy work BEFORE the first command store. The issued
        // descriptor owns its image and no future caller-stack view is needed.
        PendingNativeSwapCommand pending{*command,image,*graphicsRingConfiguration_,*graphicsSystemIdentifier_,
            {image.request.allocationBase,image.owner.virtualBase,*graphicsRingAllocation_,*graphicsWriteBackAllocation_,*graphicsIdentifierAllocation_}};
        static_assert(std::is_nothrow_move_assignable_v<decltype(pendingNativeSwap_)>);
        pendingNativeSwap_=std::move(pending);
        std::memcpy(addressSpace_->translate(image.request.commandVirtual),command->bytes().data(),Command::Bytes);
        nativeSwapStatus_={true,true,false,image.request.commandPhysical,command->identity().previousReadWord,
            command->identity().previousReadWord,command->identity().previousReadWord};
        return true; // Issued work ONLY; consumer not invoked, no RPTR/identifier writes.
    });
}
bool GuestImportRegistry::issuePostOutputSwapCommandLocked(const GuestImportCall& call){
    if(!postOutputSystemContext_ || pendingPostOutputSwap_ || postOutputSwapStatus_.issued ||
        !nativeSwapStatus_.issued || !nativeSwapStatus_.presentQueued || nativeSwapStatus_.pending ||
        RECOMP_REGISTRY_CALLER_TOKEN!=nativeSystemContextThread_ || !graphicsRingAllocation_ || !graphicsWriteBackAllocation_ ||
        !graphicsIdentifierAllocation_ || !graphicsSystemIdentifier_ || !nativeDisplayModeStatus_.selected ||
        nativeDisplayModeStatus_.queries!=1)return false;
    const auto* thread=findThreadObject(RECOMP_REGISTRY_CALLER_TOKEN);
    if(!thread || thread->returned || thread->suspendCount)return false;
    return visitNativeOwnedSwapImageLocked(call,[&](const auto& image){
        using Command=recomp::gpu::NativeSwapCommand;
        const auto& ring=*graphicsRingConfiguration_;
        const std::array<std::pair<unsigned,unsigned>,3> controls{{{ring.virtualAddress,ring.bytes},
            {ring.writeBack->virtualAddress,4},{*graphicsSystemIdentifier_,4}}};
        for(const auto& span:controls){
            if(hasDispatcherWaitInRange(span.first,span.second))return false;
            for(const auto& cs:criticalSections_)if(std::uint64_t{span.first}<std::uint64_t{cs.first}+28 &&
                std::uint64_t{cs.first}<std::uint64_t{span.first}+span.second)return false;
        }
        // The first ticket remains historical. Ticket2 can be issued once;
        // neither identical pixels nor a reused address can inherit ticket1.
        const auto command=Command::make({2,image.request.commandPhysical,image.physicalAddress,ring.readWord});
        if(!command)return false;
        PendingNativeSwapCommand pending{*command,image,ring,*graphicsSystemIdentifier_,
            {image.request.allocationBase,image.owner.virtualBase,*graphicsRingAllocation_,*graphicsWriteBackAllocation_,*graphicsIdentifierAllocation_}};
        static_assert(std::is_nothrow_move_assignable_v<decltype(pendingPostOutputSwap_)>);
        pendingPostOutputSwap_=std::move(pending);
        std::memcpy(addressSpace_->translate(image.request.commandVirtual),command->bytes().data(),Command::Bytes);
        postOutputSwapStatus_={true,true,false,image.request.commandPhysical,ring.readWord,ring.readWord,ring.readWord};
        return true; // One owned native ticket, no native execution or presentation.
    },true);
}
std::optional<GuestImportRegistry::NativeDeferredSwapRequest> GuestImportRegistry::nativeDeferredSwapRequest() const {
    RECOMP_REGISTRY_LOCK(lock);
    if(pendingDeferredSwap_)return pendingDeferredSwap_->metadata;
    return completedDeferredWrappedSwap_?std::optional<NativeDeferredSwapRequest>{completedDeferredWrappedSwap_->issuedRequest}:std::nullopt;
}
bool GuestImportRegistry::issueDeferredNativeSwapCommandLocked(const GuestImportCall& call){
    using Plan=recomp::gpu::NativeSwapImagePlan;
    using Command=recomp::gpu::NativeSwapCommand;
    if(pendingDeferredSwap_ || completedDeferredWrappedSwap_ || RECOMP_REGISTRY_CALLER_TOKEN!=nativeSystemContextThread_ ||
        !graphicsRingAllocation_ || !graphicsWriteBackAllocation_ || !graphicsIdentifierAllocation_ ||
        !completedOwnedColorImage_ || !pendingPostOutputColorImage_)return false;
    for(const auto arg:call.arguments)if(arg>UINT32_MAX)return false;
    const auto request=inspectNativeSwapRequestLocked(call,true,true);
    if(!request)return false;
    // Do not overwrite historical commands or redirect a previous image's
    // consumers. This bounded phase admits a distinct third image only.
    if(request->commandPhysical<std::uint64_t{completedPostOutputWrappedSwap_->childPhysical}+
            recomp::gpu::NativePostOutputSwapWrapper::Bytes ||
        request->frontbufferVirtual==completedOwnedColorImage_->virtualBase ||
        request->frontbufferVirtual==pendingPostOutputColorImage_->virtualBase ||
        request->frontbufferVirtual<physicalArenaBase_)return false;
    const auto physical=std::uint64_t{request->frontbufferVirtual}-physicalArenaBase_+physicalAddressOffset_;
    if(physical>UINT32_MAX || !Plan::make(request->fetch,request->frontbufferVirtual,static_cast<std::uint32_t>(physical),
            request->format,request->colorSpace,request->width,request->height))return false;
    const auto mapping=resolvePhysicalRange(static_cast<std::uint32_t>(physical),Plan::Capacity,true);
    if(!mapping || mapping->virtualAddress!=request->frontbufferVirtual || mapping->allocationBase!=request->frontbufferVirtual)return false;
    const auto allocation=physicalAllocations_.find(mapping->allocationBase);
    if(allocation==physicalAllocations_.end() || allocation->second.size<Plan::Capacity)return false;
    const auto imageEnd=std::uint64_t{mapping->allocationBase}+allocation->second.size;
    const auto overlapsImage=[&](std::uint32_t address,std::uint32_t bytes){
        return std::uint64_t{mapping->allocationBase}<std::uint64_t{address}+bytes && address<imageEnd;
    };
    const auto& ring=*graphicsRingConfiguration_;
    if(overlapsImage(request->allocationBase,request->allocationBytes) || overlapsImage(ring.virtualAddress,ring.bytes) ||
        overlapsImage(ring.writeBack->virtualAddress,4) || overlapsImage(*graphicsSystemIdentifier_,4) ||
        overlapsImage(completedOwnedColorImage_->allocationBase,completedOwnedColorImage_->allocationBytes) ||
        overlapsImage(pendingPostOutputColorImage_->allocationBase,pendingPostOutputColorImage_->allocationBytes) ||
        hasDispatcherWaitInRange(mapping->allocationBase,allocation->second.size))return false;
    for(const auto& cs:criticalSections_)if(overlapsImage(cs.first,28))return false;
    // All writes follow complete ownership/layout validation. Crucially there
    // is no read/decode of the requested image, no borrowed copy receipt and
    // no consumer call. The future ordered executor must supply fresh pixels.
    const auto command=Command::make({3,request->commandPhysical,static_cast<std::uint32_t>(physical),ring.readWord});
    if(!command)return false;
    const NativeDeferredSwapRequest metadata{*request,
        {static_cast<std::uint32_t>(physical),mapping->allocationBase,allocation->second.size,allocation->second.protection,true},
        static_cast<std::uint32_t>(physical),ring.readWord};
    PendingDeferredSwapCommand pending{*command,metadata,{request->allocationBase,mapping->allocationBase,
        *graphicsRingAllocation_,*graphicsWriteBackAllocation_,*graphicsIdentifierAllocation_},ring,*graphicsSystemIdentifier_};
    static_assert(std::is_nothrow_move_assignable_v<decltype(pendingDeferredSwap_)>);
    pendingDeferredSwap_=std::move(pending);
    std::memcpy(addressSpace_->translate(request->commandVirtual),command->bytes().data(),Command::Bytes);
    return true;
}
bool GuestImportRegistry::validatePendingNativeSwapLocked(bool postOutput) const noexcept {
    using Command=recomp::gpu::NativeSwapCommand;
    const auto& selected=postOutput?pendingPostOutputSwap_:pendingNativeSwap_;
    const auto& selectedCopy=postOutput?pendingPostOutputColorImage_:completedOwnedColorImage_;
    const auto& status=postOutput?postOutputSwapStatus_:nativeSwapStatus_;
    if(postOutput && (!completedPostOutputColor_ || !postOutputSystemContext_ || !completedWrappedSwap_ ||
        !nativeSwapStatus_.presentQueued || pendingNativeSwap_ || !validateNativeScalerStorageLocked()))return false;
    if(!nativeSwapConsumer_ || !selected || !status.pending || !selectedCopy ||
        !graphicsInitialized_ || !graphicsBackend_ || !graphicsBackend_->ready() || !graphicsRingConfiguration_ ||
        !graphicsRingConfiguration_->writeBack || !graphicsRingAllocation_ || !graphicsWriteBackAllocation_ ||
        !graphicsIdentifierAllocation_ || !graphicsSystemIdentifier_)return false;
    const auto& pending=*selected;const auto& command=pending.command;
    const auto& copy=*selectedCopy;auto& ring=*graphicsRingConfiguration_;
    if(postOutput && (command.identity().ticket!=2 || command.identity().previousReadWord!=completedPostOutputColor_->color.readWord ||
        copy.readWord!=command.identity().previousReadWord || postOutputSystemContext_->readWord!=copy.readWord))return false;
    const auto previous=pending.ring.readWord;
    const auto& original=pending.ring;
    if(ring.physicalAddress!=original.physicalAddress || ring.virtualAddress!=original.virtualAddress || ring.bytes!=original.bytes ||
        !original.writeBack || ring.writeBack->physicalAddress!=original.writeBack->physicalAddress ||
        ring.writeBack->virtualAddress!=original.writeBack->virtualAddress || ring.writeBack->blockSizeLog2!=original.writeBack->blockSizeLog2 ||
        *graphicsSystemIdentifier_!=pending.identifierAddress || *graphicsRingAllocation_!=pending.pinnedAllocations[2] ||
        *graphicsWriteBackAllocation_!=pending.pinnedAllocations[3] || *graphicsIdentifierAllocation_!=pending.pinnedAllocations[4])return false;
    if(ring.readWord!=previous || status.previousWord!=previous || status.readWord!=previous ||
        status.issuedReadWord!=command.identity().previousReadWord)return false;
    const auto ownedRing=resolvePhysicalRange(ring.physicalAddress,ring.bytes,false);
    const auto ownedWb=resolvePhysicalRange(ring.writeBack->physicalAddress,4,true);
    const auto ownedCommand=resolvePhysicalRange(command.identity().commandPhysical,Command::Bytes,false);
    const auto ownedCopy=resolvePhysicalRange(copy.physicalBase,recomp::gpu::NativeSwapImagePlan::Capacity,true);
    if(!ownedRing || ownedRing->allocationBase!=*graphicsRingAllocation_ || ownedRing->virtualAddress!=ring.virtualAddress ||
        !ownedWb || ownedWb->allocationBase!=*graphicsWriteBackAllocation_ || ownedWb->virtualAddress!=ring.writeBack->virtualAddress ||
        !ownedCommand || ownedCommand->allocationBase!=pending.image.request.allocationBase ||
        ownedCommand->virtualAddress!=pending.image.request.commandVirtual ||
        !ownedCopy || ownedCopy->allocationBase!=copy.allocationBase || ownedCopy->virtualAddress!=copy.virtualBase)return false;
    const auto commandOwner=physicalAllocations_.find(ownedCommand->allocationBase),copyOwner=physicalAllocations_.find(copy.allocationBase);
    if(commandOwner==physicalAllocations_.end() || copyOwner==physicalAllocations_.end() ||
        commandOwner->second.size!=pending.image.request.allocationBytes || commandOwner->second.protection!=pending.image.request.protection ||
        copyOwner->second.size!=copy.allocationBytes || copyOwner->second.protection!=copy.protection)return false;
    std::uint32_t published=0;
    if(!readU32(ring.writeBack->virtualAddress,published) || published!=previous)return false;
    if(postOutput){
        const std::array<std::pair<unsigned,unsigned>,5> spans{{{pending.image.request.commandVirtual,Command::Bytes},
            {copy.virtualBase,recomp::gpu::NativeSwapImagePlan::Capacity},{ring.virtualAddress,ring.bytes},
            {ring.writeBack->virtualAddress,4},{pending.identifierAddress,4}}};
        for(const auto& span:spans){
            if(hasDispatcherWaitInRange(span.first,span.second))return false;
            for(const auto& cs:criticalSections_)if(std::uint64_t{span.first}<std::uint64_t{cs.first}+28 &&
                std::uint64_t{cs.first}<std::uint64_t{span.first}+span.second)return false;
        }
    }
    return command.matches(command.identity().commandPhysical,addressSpace_->translate(ownedCommand->virtualAddress),Command::Bytes) &&
        v232Matches(copy.copy,copy.physicalBase,addressSpace_->translate(copy.virtualBase),
            recomp::gpu::NativeSwapImagePlan::Capacity);
}
bool GuestImportRegistry::submitExperimentalNativeSwapCommand(std::uint32_t writeWord){
    RECOMP_REGISTRY_LOCK(lock);
    using Command=recomp::gpu::NativeSwapCommand;
    if(!validatePendingNativeSwapLocked())return false;
    const auto& pending=*pendingNativeSwap_;const auto& command=pending.command;
    auto& ring=*graphicsRingConfiguration_;const auto previous=pending.ring.readWord;
    if(std::uint64_t{previous}+3!=writeWord || writeWord>=ring.bytes/4)return false;
    noteNativeSubmissionMayCommitAt(1); // v183 site 1: may publish completion: the renderer's pending reads first
    std::array<std::uint32_t,3> root{};
    for(unsigned i=0;i<3;++i)if(!readU32(ring.virtualAddress+(previous+i)*4,root[i]))return false;
    if(root!=std::array<std::uint32_t,3>{0xC0013F00,command.identity().commandPhysical,Command::Words})return false;
    std::array<std::uint8_t,4> next{};for(unsigned i=0;i<4;++i)next[i]=static_cast<std::uint8_t>(writeWord>>(24-i*8));
    bool queued=false;
    try {queued=nativeSwapConsumer_(pending.image);}catch(...){return false;}
    if(!queued)return false;
    // A broken native callback cannot mutate this submission and turn an
    // accepted presentation into retirement of different work. Fail-stop that
    // contract violation, never offer a resumable false/retry after acceptance.
    std::uint32_t current=0;
    if(!validatePendingNativeSwapLocked())std::_Exit(4);
    for(unsigned i=0;i<3;++i)
        if(!readU32(ring.virtualAddress+(previous+i)*4,current) || current!=root[i])std::_Exit(4);
    // After the fail-stop checks, no fallible operations precede/follow the
    // first store. Writers remain parked, owners pinned/locked. RPTR retires
    // ONLY the exact root IB.
    // Presentation queue acceptance is not vblank or a system GPU identifier.
    std::memcpy(addressSpace_->translate(ring.writeBack->virtualAddress),next.data(),next.size());
    ring.readWord=writeWord;nativeSwapStatus_.pending=false;nativeSwapStatus_.presentQueued=true;
    nativeSwapStatus_.readWord=writeWord;pendingNativeSwap_.reset();return true;
}
std::optional<GuestImportRegistry::NativeCommandStorageCompletion> GuestImportRegistry::completedNativeCommandStorageCoherency() const {
    RECOMP_REGISTRY_LOCK(lock);return completedCommandStorageCoherency_;
}
std::optional<GuestImportRegistry::NativeWrappedSwapCompletion> GuestImportRegistry::completedNativeWrappedSwap() const {
    RECOMP_REGISTRY_LOCK(lock);return completedWrappedSwap_;
}
bool GuestImportRegistry::submitNativeWrappedSwapCommand(std::uint32_t writeWord){
    using namespace recomp::gpu;
    RECOMP_REGISTRY_LOCK(lock);
    if(completedWrappedSwap_ || !completedCommandStorageCoherency_ || !experimentalIdleCachePairBound_ ||
        experimentalIdleCachePairs_==UINT64_MAX || !nativeDisplayModeStatus_.selected || nativeDisplayModeStatus_.queries!=1 ||
        !validatePendingNativeSwapLocked())return false;
    const auto& pending=*pendingNativeSwap_;auto& ring=*graphicsRingConfiguration_;const auto previous=ring.readWord;
    const auto command=pending.command.identity().commandPhysical;
    if(completedCommandStorageCoherency_->readWord!=previous || command<NativeSwapWrapper::CommandOffsetWords*4 ||
        std::uint64_t{previous}+3!=writeWord || writeWord>=ring.bytes/4)return false;
    noteNativeSubmissionMayCommitAt(2); // v183 site 2: may publish completion: the renderer's pending reads first
    std::array<std::uint32_t,3> root{};
    for(unsigned i=0;i<3;++i)if(!readU32(ring.virtualAddress+(previous+i)*4,root[i]))return false;
    if(root!=std::array<std::uint32_t,3>{0xC0013F00,command-NativeSwapWrapper::CommandOffsetWords*4,NativeSwapWrapper::Words})return false;
    const auto child=resolvePhysicalRange(root[1],NativeSwapWrapper::Bytes,false);
    if(!child || child->allocationBase!=pending.pinnedAllocations[0])return false;
    std::array<std::uint8_t,NativeSwapWrapper::Bytes> bytes{};
    std::memcpy(bytes.data(),addressSpace_->translate(child->virtualAddress),bytes.size());
    const auto plan=NativeSwapWrapper::parse(pending.command,root[1],bytes.data(),bytes.size());if(!plan)return false;
    const auto overlaps=[](std::uint32_t a,std::uint32_t n,std::uint32_t b,std::uint32_t m){
        return std::uint64_t{a}<std::uint64_t{b}+m && std::uint64_t{b}<std::uint64_t{a}+n;
    };
    // Only the existing pinned control allocation can receive these events.
    // It must be distinct from the entire ring, command and image allocations,
    // not merely from their active subranges. No new guest lease is invented.
    for(unsigned i=0;i<3;++i)if(pending.pinnedAllocations[3]==pending.pinnedAllocations[i])return false;
    std::array<std::uint32_t,4> destinations{};
    std::array<std::uint8_t,16> originalEvents{};
    std::optional<NativeWrappedSwapCompletion> prepared{NativeWrappedSwapCompletion{previous,writeWord,root[1],{}, {}}};
    auto& completion=*prepared;
    for(unsigned i=0;i<4;++i){const auto physical=plan->events()[i].physicalAddress;
        const auto owner=resolvePhysicalRange(physical,4,true);
        if(!owner || owner->allocationBase!=pending.pinnedAllocations[3] ||
            overlaps(owner->virtualAddress,4,ring.writeBack->virtualAddress,4) ||
            overlaps(owner->virtualAddress,4,pending.identifierAddress,4))return false;
        for(const auto& entry:criticalSections_)if(overlaps(owner->virtualAddress,4,entry.first,28))return false;
        for(const auto& entry:dispatcherEventWaits_)if(overlaps(owner->virtualAddress,4,entry.first,16))return false;
        destinations[i]=owner->virtualAddress;completion.eventPhysical[i]=physical;
        std::memcpy(originalEvents.data()+i*4,addressSpace_->translate(destinations[i]),4);
    }
    std::array<std::uint8_t,4> next{};for(unsigned i=0;i<4;++i)next[i]=writeWord>>(24-i*8);
    bool accepted=false,invoked=false,completed=false;
    // Construct the type-erased callback (potential allocation) before native
    // work. Queue acceptance is sticky; a broken backend cannot allow retry.
    const std::function<bool()> present=[&]{
        if(invoked){if(accepted)std::_Exit(4);return false;}invoked=true;
        accepted=nativeSwapConsumer_(pending.image);return accepted;
    };
    try {completed=RECOMP_V_BACKEND()->executeNativeSwapWrapper(*plan,present,completion.gpu);}
    catch(...){if(accepted)std::_Exit(4);return false;}
    if(!accepted)return false; // A bare backend true is not a presentation receipt.
    if(!completed || !validNativeSwapCompletion(*plan,completion.gpu) || !validatePendingNativeSwapLocked())std::_Exit(4);
    std::uint32_t current=0;
    for(unsigned i=0;i<3;++i)if(!readU32(ring.virtualAddress+(previous+i)*4,current) || current!=root[i])std::_Exit(4);
    if(std::memcmp(bytes.data(),addressSpace_->translate(child->virtualAddress),bytes.size()))std::_Exit(4);
    for(unsigned i=0;i<4;++i)
        if(std::memcmp(originalEvents.data()+i*4,addressSpace_->translate(destinations[i]),4))std::_Exit(4);
    // Other guest writers are parked and this exact wrapper contains no
    // intervening reads. Publish audited GPU events in packet order, then RPTR.
    // No fallible allocation/work from the first guest store onward.
    static_assert(std::is_nothrow_move_assignable_v<decltype(prepared)>);
    completedWrappedSwap_=std::move(prepared);++experimentalIdleCachePairs_;
    nativeScanoutRequest_=pending.image.request;
    if (nativeIdentityScalerBound_) {
        const auto& request=pending.image.request;
        nativeScalerStorageOwner_=StoppedPhysicalOwnership{
            physicalAddressOffset_+(request.allocationBase-physicalArenaBase_),request.allocationBase,
            request.allocationBytes,request.protection,true};
    }
    for(unsigned i=0;i<4;++i)std::memcpy(addressSpace_->translate(destinations[i]),completedWrappedSwap_->gpu.eventBytes.data()+i*4,4);
    std::memcpy(addressSpace_->translate(ring.writeBack->virtualAddress),next.data(),next.size());
    ring.readWord=writeWord;nativeSwapStatus_.pending=false;nativeSwapStatus_.presentQueued=true;
    nativeSwapStatus_.readWord=writeWord;pendingNativeSwap_.reset();return true;
}
std::optional<GuestImportRegistry::NativePendingSubmission> GuestImportRegistry::inspectStoppedNativePendingSubmission(std::uint32_t writeWord) const {
    RECOMP_REGISTRY_LOCK(lock);
    if(!completedCommandStorageCoherency_ || !validatePendingNativeSwapLocked())return std::nullopt;
    const auto& ring=*graphicsRingConfiguration_;const auto previous=ring.readWord;
    if(std::uint64_t{previous}+3!=writeWord || writeWord>=ring.bytes/4)return std::nullopt;
    std::array<std::uint32_t,3> root{};
    for(unsigned i=0;i<3;++i)if(!readU32(ring.virtualAddress+(previous+i)*4,root[i]))return std::nullopt;
    if(root[0]!=0xC0013F00 || (root[1]&3) || root[2]<64 || root[2]>1024 ||
        std::uint64_t{root[1]}+root[2]*4>0x20000000)return std::nullopt;
    const auto& pending=*pendingNativeSwap_;const auto command=pending.command.identity().commandPhysical;
    const auto owner=resolvePhysicalRange(root[1],root[2]*4,false);
    if(!owner || owner->allocationBase!=pending.pinnedAllocations[0] || root[1]>command ||
        std::uint64_t{command}+recomp::gpu::NativeSwapCommand::Bytes>std::uint64_t{root[1]}+root[2]*4)return std::nullopt;
    return NativePendingSubmission{ring.physicalAddress+previous*4,root[1],root[2],(command-root[1])/4,previous,writeWord};
}
std::optional<GuestImportRegistry::NativePostOutputWrappedSwapCompletion> GuestImportRegistry::completedNativePostOutputWrappedSwap() const {
    RECOMP_REGISTRY_LOCK(lock);return completedPostOutputWrappedSwap_;
}
bool GuestImportRegistry::submitNativePostOutputWrappedSwapCommand(std::uint32_t writeWord,
        const GuestGraphicsBackend::NativePostOutputSwapInterrupt& interrupt){
    using namespace recomp::gpu;
    RECOMP_REGISTRY_UNIQUE(lock);
    if(!interrupt || postOutputWrappedSwapAttempted_ || completedPostOutputWrappedSwap_ || !experimentalIdleCachePairBound_ ||
        experimentalIdleCachePairs_==UINT64_MAX || !postOutputSwapInterruptWorker_ || postOutputSwapInterruptScopeStatus_.scope.attempted ||
        RECOMP_REGISTRY_CALLER_TOKEN!=nativeSystemContextThread_ || !nativeDisplayModeStatus_.selected || nativeDisplayModeStatus_.queries!=1)return false;
    const auto* issuer=findThreadObject(RECOMP_REGISTRY_CALLER_TOKEN);const auto* worker=findThreadObject(*postOutputSwapInterruptWorker_);
    if(!issuer || issuer->returned || issuer->suspendCount || !worker || worker->initial || worker->returned || worker->suspendCount || worker->virtualCpuMask!=4)return false;
    const auto snapshot=inspectPostOutputSwapInterruptLocked(writeWord,nullptr);
    if(!snapshot || postOutputSwapQueuedInterruptRejectionLocked(*snapshot))return false;
    unsigned base=0,mask=0;
    if(!graphicsBackend_->queryNativeScratchWriteback(base,mask) || !snapshot->wrapper.scratchStateMatches(base,mask))return false;
    const auto& pending=*pendingPostOutputSwap_;const auto ring=*graphicsRingConfiguration_;
    const auto plan=snapshot->wrapper;const auto previous=ring.readWord;
    const auto child=resolvePhysicalRange(snapshot->submission.childPhysical,NativePostOutputSwapWrapper::Bytes,false);
    if(!child)return false;
    std::array<std::uint8_t,NativePostOutputSwapWrapper::Bytes> commandBytes{};
    std::memcpy(commandBytes.data(),addressSpace_->translate(child->virtualAddress),commandBytes.size());
    const auto overlaps=[](unsigned a,unsigned n,unsigned b,unsigned m){
        return std::uint64_t{a}<std::uint64_t{b}+m && std::uint64_t{b}<std::uint64_t{a}+n;};
    for(unsigned i=0;i<3;++i)if(pending.pinnedAllocations[3]==pending.pinnedAllocations[i])return false;
    std::array<unsigned,4> destinations{};std::array<std::uint8_t,16> oldEvents{};
    std::optional<NativePostOutputWrappedSwapCompletion> prepared{NativePostOutputWrappedSwapCompletion{
        previous,writeWord,snapshot->submission.childPhysical,{}, {},pending.image.request,pending.image.owner}};
    for(unsigned i=0;i<4;++i){
        const auto physical=plan.events()[i].physicalAddress;const auto owner=resolvePhysicalRange(physical,4,true);
        if(!owner || owner->allocationBase!=pending.pinnedAllocations[3] ||
            overlaps(owner->virtualAddress,4,ring.writeBack->virtualAddress,4) || overlaps(owner->virtualAddress,4,pending.identifierAddress,4))return false;
        // Raw inspector already excludes scratch, command, active wait/lock and
        // full root/identifier aliases. Restrict these events to the pinned owner.
        destinations[i]=owner->virtualAddress;prepared->eventPhysical[i]=physical;
        std::memcpy(oldEvents.data()+i*4,addressSpace_->translate(destinations[i]),4);
    }
    const auto irqWorker=*postOutputSwapInterruptWorker_,issuerToken=RECOMP_REGISTRY_CALLER_TOKEN;
    const auto image=pending.image; // Independent RGBA allocated BEFORE native work/unlocking.
    std::array<std::uint8_t,4> next{};for(unsigned i=0;i<4;++i)next[i]=writeWord>>(24-i*8);
    const auto intact=[&](bool beforeInterrupt=false){
        const auto current=inspectPostOutputSwapInterruptLocked(writeWord,nullptr);
        if(!current || !plan.sameRequest(current->wrapper) || current->submission.rootPhysical!=snapshot->submission.rootPhysical ||
            current->device!=snapshot->device || current->handler!=snapshot->handler || current->frequency!=snapshot->frequency ||
            std::memcmp(commandBytes.data(),addressSpace_->translate(child->virtualAddress),commandBytes.size()))return false;
        if(beforeInterrupt && (current->deviceValues!=snapshot->deviceValues || current->queuedSurfaces!=snapshot->queuedSurfaces ||
            current->scratch!=snapshot->scratch))return false;
        for(unsigned i=0;i<4;++i)if(std::memcmp(oldEvents.data()+i*4,addressSpace_->translate(destinations[i]),4))return false;
        return true;
    };
    bool accepted=false,presentCalled=false,interruptCalled=false;
    const std::function<bool()> present=[&]{
        RECOMP_REGISTRY_LOCK(guard);
        if(presentCalled || interruptCalled){if(accepted)std::_Exit(4);return false;}
        presentCalled=true;
        if(RECOMP_REGISTRY_CALLER_TOKEN!=issuerToken || !intact(true))return false;
        accepted=nativeSwapConsumer_(image);return accepted;
    };
    const GuestGraphicsBackend::NativePostOutputSwapInterrupt dispatch=[&](const auto& action){
        if(!accepted || interruptCalled){if(accepted)std::_Exit(4);interruptCalled=true;return false;}
        interruptCalled=true;
        {RECOMP_REGISTRY_LOCK(guard);if(RECOMP_REGISTRY_CALLER_TOKEN!=issuerToken || !intact(true))return false;}
        if(!interrupt(action))return false;
        RECOMP_REGISTRY_LOCK(guard);
        const auto* finished=findThreadObject(irqWorker);
        return RECOMP_REGISTRY_CALLER_TOKEN==issuerToken && finished && finished->returned &&
            postOutputSwapInterruptScopeStatus_.scope.worker==irqWorker && postOutputSwapInterruptScopeStatus_.scope.balanced &&
            postOutputSwapInterruptScopeStatus_.queuedCallbackValidated && postOutputSwapInterruptSequence_ &&
            postOutputSwapInterruptSequence_->phase()==NativePostOutputSwapInterruptSequence::Phase::Acknowledged &&
            validateNativePostOutputSwapGuestResultLocked(writeWord,true);
    };
    postOutputWrappedSwapAttempted_=true;
    const auto backend=RECOMP_V_BACKEND_SHARED();RECOMP_REGISTRY_UNLOCK(lock);bool completed=false;
    try{completed=backend->executeNativePostOutputSwapWrapper(plan,present,dispatch,prepared->gpu);}
    catch(...){if(accepted)std::_Exit(4);return false;}
    RECOMP_REGISTRY_RELOCK(lock);
    if(!accepted)return false;
    if(!completed || !interruptCalled || RECOMP_REGISTRY_CALLER_TOKEN!=issuerToken || !intact() ||
        !validNativePostOutputSwapCompletion(plan,prepared->gpu) || !validateNativePostOutputSwapGuestResultLocked(writeWord,true))std::_Exit(4);
    // All guest writers stayed parked. The reviewed source1 callback does not
    // read these events; publish audited words in packet order, then RPTR last.
    // Separate scanout metadata preserves the first-frame historical receipt.
    static_assert(std::is_nothrow_move_assignable_v<decltype(prepared)>);
    completedPostOutputWrappedSwap_=std::move(prepared);++experimentalIdleCachePairs_;
    for(unsigned i=0;i<4;++i)std::memcpy(addressSpace_->translate(destinations[i]),completedPostOutputWrappedSwap_->gpu.eventBytes.data()+i*4,4);
    std::memcpy(addressSpace_->translate(ring.writeBack->virtualAddress),next.data(),4);
    graphicsRingConfiguration_->readWord=writeWord;postOutputSwapStatus_.pending=false;postOutputSwapStatus_.presentQueued=true;
    postOutputSwapStatus_.readWord=writeWord;pendingPostOutputSwap_.reset();return true;
}
std::optional<GuestImportRegistry::NativeDeferredWrappedSwapCompletion> GuestImportRegistry::completedNativeDeferredWrappedSwap() const {
    RECOMP_REGISTRY_LOCK(lock);return completedDeferredWrappedSwap_;
}
bool GuestImportRegistry::submitNativeDeferredWrappedSwapCommand(std::uint32_t writeWord,
        const GuestGraphicsBackend::NativeDeferredSwapInterrupt& interrupt){
    using namespace recomp::gpu;RECOMP_REGISTRY_UNIQUE(lock);
    if(!interrupt || deferredWrappedSwapAttempted_ || completedDeferredWrappedSwap_ || !experimentalIdleCachePairBound_ ||
        experimentalIdleCachePairs_==UINT64_MAX || !deferredSwapInterruptWorker_ || deferredSwapInterruptScopeStatus_.scope.attempted ||
        RECOMP_REGISTRY_CALLER_TOKEN!=nativeSystemContextThread_ || !nativeDisplayModeStatus_.selected || nativeDisplayModeStatus_.queries!=1 ||
        !nativeSwapConsumer_)return false;
    const auto* issuer=findThreadObject(RECOMP_REGISTRY_CALLER_TOKEN);const auto* worker=findThreadObject(*deferredSwapInterruptWorker_);
    if(!issuer || issuer->returned || issuer->suspendCount || !worker || worker->initial || worker->returned || worker->suspendCount || worker->virtualCpuMask!=4)return false;
    const auto snapshot=inspectDeferredSwapInterruptLocked(writeWord,nullptr);
    if(!snapshot || deferredSwapQueuedInterruptRejectionLocked(*snapshot))return false;
    const auto submission=deferredColorSubmission_; // Shared owned identity; no borrowed command bytes across unlock.
    const auto& plan=submission->swap();const auto ring=*graphicsRingConfiguration_;
    const auto metadata=pendingDeferredSwap_->metadata;const auto previous=ring.readWord;
    const auto identifierAddress=pendingDeferredSwap_->identifierAddress;unsigned identifier=0;
    if(!readU32(identifierAddress,identifier))return false;
    const auto& request=metadata.request;
    const auto imagePlan=NativeSwapImagePlan::make(request.fetch,request.frontbufferVirtual,metadata.imagePhysical,
        request.format,request.colorSpace,request.width,request.height);
    if(!imagePlan)return false;
    NativeOwnedSwapImage image{request,metadata.imageOwner,metadata.imagePhysical,{}};
    if(!imagePlan->decode(addressSpace_->translate(request.frontbufferVirtual),NativeSwapImagePlan::Capacity,image.rgba))return false;
    std::optional<NativeDeferredWrappedSwapCompletion> prepared{NativeDeferredWrappedSwapCompletion{
        previous,writeWord,snapshot->submission.childPhysical,snapshot->submission.childWords,{}, {},metadata}};
    std::array<unsigned,4> destinations{};std::array<std::uint8_t,16> oldEvents{};
    for(unsigned i=0;i<4;++i){
        const auto physical=plan.events()[i].physicalAddress;const auto owner=resolvePhysicalRange(physical,4,true);
        if(!owner || owner->allocationBase!=pendingDeferredSwap_->pinnedAllocations[3])return false;
        // Inspector excludes root/cursor/identifier/old-image/scratch and lock
        // aliases. Prefix already pinned the exact future-event owner.
        destinations[i]=owner->virtualAddress;prepared->eventPhysical[i]=physical;
        std::memcpy(oldEvents.data()+i*4,addressSpace_->translate(destinations[i]),4);
    }
    const auto irqWorker=*deferredSwapInterruptWorker_,issuerToken=RECOMP_REGISTRY_CALLER_TOKEN;
    std::array<std::uint8_t,4> next{};for(unsigned i=0;i<4;++i)next[i]=writeWord>>(24-i*8);
    const auto intact=[&](bool beforeInterrupt=false){
        const auto now=inspectDeferredSwapInterruptLocked(writeWord,nullptr);unsigned currentIdentifier=0;
        if(!now || !now->wrapper.sameRequest(plan) || now->submission.rootPhysical!=snapshot->submission.rootPhysical ||
            now->device!=snapshot->device || now->handler!=snapshot->handler || now->frequency!=snapshot->frequency ||
            !readU32(identifierAddress,currentIdentifier) || currentIdentifier!=identifier)return false;
        if(beforeInterrupt && (now->deviceValues!=snapshot->deviceValues || now->queuedSurfaces!=snapshot->queuedSurfaces ||
            now->scratch!=snapshot->scratch || now->pacingRecord!=snapshot->pacingRecord))return false;
        for(unsigned i=0;i<4;++i)if(std::memcmp(oldEvents.data()+i*4,addressSpace_->translate(destinations[i]),4))return false;
        return true;
    };
    bool accepted=false,presentCalled=false,interruptCalled=false;
    const std::function<bool()> present=[&]{
        RECOMP_REGISTRY_LOCK(guard);
        if(presentCalled || interruptCalled){if(accepted)std::_Exit(4);return false;}
        presentCalled=true;
        if(RECOMP_REGISTRY_CALLER_TOKEN!=issuerToken || !intact(true))return false;
        accepted=nativeSwapConsumer_(image);return accepted;
    };
    const GuestGraphicsBackend::NativeDeferredSwapInterrupt dispatch=[&](const auto& action){
        if(!accepted || interruptCalled){if(accepted)std::_Exit(4);interruptCalled=true;return false;}
        interruptCalled=true;
        {RECOMP_REGISTRY_LOCK(guard);if(RECOMP_REGISTRY_CALLER_TOKEN!=issuerToken || !intact(true))return false;}
        if(!interrupt(action))return false;
        RECOMP_REGISTRY_LOCK(guard);const auto* finished=findThreadObject(irqWorker);
        return RECOMP_REGISTRY_CALLER_TOKEN==issuerToken && finished && finished->returned &&
            deferredSwapInterruptScopeStatus_.scope.worker==irqWorker && deferredSwapInterruptScopeStatus_.scope.balanced &&
            deferredSwapInterruptScopeStatus_.queuedCallbackValidated && deferredSwapInterruptSequence_ &&
            deferredSwapInterruptSequence_->phase()==NativeDeferredSwapInterruptSequence::Phase::Acknowledged &&
            validateNativeDeferredSwapGuestResultLocked(writeWord,true);
    };
    deferredWrappedSwapAttempted_=true;const auto backend=RECOMP_V_BACKEND_SHARED();RECOMP_REGISTRY_UNLOCK(lock);bool completed=false;
    try{completed=backend->executeNativeDeferredSwapWrapper(*submission,present,dispatch,prepared->gpu);}
    catch(...){if(accepted)std::_Exit(4);return false;}
    RECOMP_REGISTRY_RELOCK(lock);if(!accepted)return false;
    if(!completed || !interruptCalled || RECOMP_REGISTRY_CALLER_TOKEN!=issuerToken || !intact() ||
        !validNativeDeferredSwapCompletion(plan,prepared->gpu) || !validateNativeDeferredSwapGuestResultLocked(writeWord,true))std::_Exit(4);
    // All postaccept work and validation completed. No fallible work after the
    // first event store; retirement belongs to the FULL combined request only.
    static_assert(std::is_nothrow_move_assignable_v<decltype(prepared)>);
    completedDeferredWrappedSwap_=std::move(prepared);++experimentalIdleCachePairs_;
    for(unsigned i=0;i<4;++i)std::memcpy(addressSpace_->translate(destinations[i]),completedDeferredWrappedSwap_->gpu.eventBytes.data()+i*4,4);
    std::memcpy(addressSpace_->translate(ring.writeBack->virtualAddress),next.data(),4);
    graphicsRingConfiguration_->readWord=writeWord;pendingDeferredSwap_.reset();return true;
}
std::optional<GuestImportRegistry::NativePendingSubmission> GuestImportRegistry::inspectStoppedPostOutputSwapSubmission(std::uint32_t writeWord) const {
    RECOMP_REGISTRY_LOCK(lock);
    return inspectPostOutputSwapSubmissionLocked(writeWord);
}
std::optional<GuestImportRegistry::NativePendingSubmission> GuestImportRegistry::inspectPostOutputSwapSubmissionLocked(std::uint32_t writeWord) const {
    if(!validatePendingNativeSwapLocked(true))return std::nullopt;
    const auto& ring=*graphicsRingConfiguration_;const auto previous=ring.readWord;
    if(std::uint64_t{previous}+3!=writeWord || writeWord>=ring.bytes/4)return std::nullopt;
    std::array<std::uint32_t,3> root{};
    for(unsigned i=0;i<3;++i)if(!readU32(ring.virtualAddress+(previous+i)*4,root[i]))return std::nullopt;
    if(root[0]!=0xC0013F00 || (root[1]&3) || root[2]<64 || root[2]>1024 ||
        std::uint64_t{root[1]}+root[2]*4>0x20000000)return std::nullopt;
    const auto& pending=*pendingPostOutputSwap_;const auto command=pending.command.identity().commandPhysical;
    const auto owner=resolvePhysicalRange(root[1],root[2]*4,false);
    if(!owner || owner->allocationBase!=pending.pinnedAllocations[0] || root[1]>command ||
        std::uint64_t{command}+recomp::gpu::NativeSwapCommand::Bytes>std::uint64_t{root[1]}+root[2]*4)return std::nullopt;
    return NativePendingSubmission{ring.physicalAddress+previous*4,root[1],root[2],(command-root[1])/4,previous,writeWord};
}
std::optional<GuestImportRegistry::StoppedPostOutputSwapInterrupt> GuestImportRegistry::inspectStoppedPostOutputSwapInterrupt(
        std::uint32_t writeWord,std::uint32_t* rejection) const {
    RECOMP_REGISTRY_LOCK(lock);
    return inspectPostOutputSwapInterruptLocked(writeWord,rejection);
}
std::optional<GuestImportRegistry::StoppedPostOutputSwapInterrupt> GuestImportRegistry::inspectPostOutputSwapInterruptLocked(
        std::uint32_t writeWord,std::uint32_t* rejection,bool allowOwnedActiveScope) const {
    if(rejection)*rejection=0;
    const auto reject=[&](unsigned reason)->std::optional<StoppedPostOutputSwapInterrupt>{if(rejection)*rejection=reason;return std::nullopt;};
    using Wrapper=recomp::gpu::NativePostOutputSwapWrapper;
    const auto pending=inspectPostOutputSwapSubmissionLocked(writeWord);
    if(!pending || !completedPostOutputSwapStorage_ || completedPostOutputSwapStorage_->readWord!=pending->readWord ||
        pending->childWords!=Wrapper::Words || pending->commandOffsetWords!=Wrapper::CommandOffsetWords ||
        (nativeInterruptScope_ && (!allowOwnedActiveScope || !nativeInterruptScope_->postOutputSwap ||
            RECOMP_REGISTRY_CALLER_TOKEN!=nativeInterruptScope_->worker)) ||
        !graphicsInterruptRegistration_ || !exactFunctionValidator_ || !nativeInterruptDeviceRoot_)return reject(1);
    const auto child=resolvePhysicalRange(pending->childPhysical,Wrapper::Bytes,false);
    if(!child)return reject(2);
    const auto plan=Wrapper::parse(pendingPostOutputSwap_->command,pending->childPhysical,
        addressSpace_->translate(child->virtualAddress),Wrapper::Bytes);
    if(!plan)return reject(3);
    return inspectSurfaceSwapFieldsLocked(*plan,*pending,rejection);
}
template<class Wrapper> std::optional<GuestImportRegistry::StoppedSurfaceSwapInterrupt<Wrapper>>
GuestImportRegistry::inspectSurfaceSwapFieldsLocked(const Wrapper& wrapper,const NativePendingSubmission& submission,
        std::uint32_t* rejection) const {
    using Snapshot=StoppedSurfaceSwapInterrupt<Wrapper>;
    const auto reject=[&](unsigned reason)->std::optional<Snapshot>{if(rejection)*rejection=reason;return std::nullopt;};
    const auto* plan=&wrapper;const auto* pending=&submission;
    const auto child=resolvePhysicalRange(pending->childPhysical,pending->childWords*4,false);
    if(!child)return reject(2);
    const auto& registration=*graphicsInterruptRegistration_;const auto device=registration.userData;
    constexpr unsigned DeviceBytes=23792;
    if(!exactFunctionValidator_(registration.routine) || !exactFunctionValidator_(plan->callbackAddress()))return reject(4);
    if(device<heapBase_ || std::uint64_t{device}+DeviceBytes>std::uint64_t{heapBase_}+heapSize_ ||
        !validGuestDataBuffer(device,DeviceBytes,true))return reject(5);
    if(!nativeInterruptDeviceRoot_(device))return reject(6);
    const auto allocation=allocations_.upper_bound(device);
    if(allocation==allocations_.begin())return reject(7);
    const auto& owned=*std::prev(allocation);
    if(std::uint64_t{device}+DeviceBytes>std::uint64_t{owned.first}+owned.second)return reject(7);
    const auto scratch=resolvePhysicalRange(plan->scratchPhysical(),32,true);
    if(!scratch)return reject(8);
    const auto overlaps=[](unsigned a,unsigned n,unsigned b,unsigned m){
        return std::uint64_t{a}<std::uint64_t{b}+m && std::uint64_t{b}<std::uint64_t{a}+n;};
    const auto& ring=*graphicsRingConfiguration_;
    const std::array<std::pair<unsigned,unsigned>,4> controls{{{ring.virtualAddress,ring.bytes},
        {ring.writeBack->virtualAddress,4},{*graphicsSystemIdentifier_,4},
        {nativeScalerStorageOwner_->virtualBase,nativeScalerStorageOwner_->byteCount}}};
    for(const auto& span:controls)if(overlaps(scratch->virtualAddress,32,span.first,span.second))return reject(9);
    const auto clear=[&](unsigned address,unsigned bytes){
        if(hasDispatcherWaitInRange(address,bytes))return false;
        for(const auto& cs:criticalSections_)if(overlaps(address,bytes,cs.first,28))return false;
        return true;};
    const std::array<std::pair<unsigned,unsigned>,6> touched{{{child->virtualAddress,pending->childWords*4},
        {device+11028,8},{device+16944,180},{device+21964,4},{device+23788,4},{scratch->virtualAddress,32}}};
    for(unsigned i=0;i<touched.size();++i)if(!clear(touched[i].first,touched[i].second))return reject(10+i);
    // Each eventual event must have writable owned storage and be disjoint
    // from the root/cursor/identifier. Reading is not authorization to write.
    for(const auto& event:plan->events()){
        const auto owner=resolvePhysicalRange(event.physicalAddress,4,true);
        if(!owner || !clear(owner->virtualAddress,4))return reject(16);
        for(const auto& span:controls)if(overlaps(owner->virtualAddress,4,span.first,span.second))return reject(17);
    }
    Snapshot result{*plan,*pending,device,registration.routine,performanceFrequency_};
    for(unsigned i=0;i<result.deviceValues.size();++i)if(!readU32(device+result.DeviceOffsets[i],result.deviceValues[i]))return reject(18);
    if(result.deviceValues[0]!=scratch->virtualAddress)return reject(19);
    for(unsigned i=0;i<32;++i)if(!readU32(device+16980+i*4,result.queuedSurfaces[i]))return reject(20);
    for(unsigned i=0;i<8;++i)if(!readU32(scratch->virtualAddress+i*4,result.scratch[i]))return reject(21);
    return result;
}
std::optional<GuestImportRegistry::StoppedDeferredSwapInterrupt> GuestImportRegistry::inspectStoppedDeferredSwapInterrupt(
        std::uint32_t writeWord,std::uint32_t* rejection) const {
    RECOMP_REGISTRY_LOCK(lock);return inspectDeferredSwapInterruptLocked(writeWord,rejection);
}
std::optional<GuestImportRegistry::StoppedDeferredSwapInterrupt> GuestImportRegistry::inspectDeferredSwapInterruptLocked(
        std::uint32_t writeWord,std::uint32_t* rejection,bool allowOwnedActiveScope) const {
    if(rejection)*rejection=0;
    const auto reject=[&](unsigned reason)->std::optional<StoppedDeferredSwapInterrupt>{if(rejection)*rejection=reason;return std::nullopt;};
    const auto pending=inspectDeferredSwapSubmissionLocked(writeWord);
    if(!pending || !completedDeferredColorPrefix_ || !pendingDeferredColorImage_ || !deferredColorSubmission_ ||
        (nativeInterruptScope_ && (!allowOwnedActiveScope || !nativeInterruptScope_->deferredSwap ||
            nativeInterruptScope_->postOutputSwap || RECOMP_REGISTRY_CALLER_TOKEN!=nativeInterruptScope_->worker)) ||
        !graphicsInterruptRegistration_ || !exactFunctionValidator_ || !nativeInterruptDeviceRoot_ ||
        completedDeferredColorPrefix_->previousWord!=pending->readWord || completedDeferredColorPrefix_->pendingWriteWord!=writeWord ||
        pending->childWords>recomp::gpu::NativeDeferredSwapSubmission::MaximumWords)return reject(1);
    const auto child=resolvePhysicalRange(pending->childPhysical,pending->childWords*4,false);
    if(!child)return reject(2);
    try{
        const auto frame=recomp::gpu::NativeDeferredSwapSubmission::parse(pendingDeferredSwap_->command,pending->childPhysical,
            addressSpace_->translate(child->virtualAddress),pending->childWords*4);
        if(!frame || !frame->sameRequest(*deferredColorSubmission_))return reject(3);
        const auto& image=*pendingDeferredColorImage_;
        const auto owner=resolvePhysicalRange(image.physicalBase,recomp::gpu::NativeSwapImagePlan::Capacity,true);
        const auto allocation=physicalAllocations_.find(image.allocationBase);
        if(!owner || owner->allocationBase!=image.allocationBase || owner->virtualAddress!=image.virtualBase ||
            allocation==physicalAllocations_.end() || allocation->second.size!=image.allocationBytes || allocation->second.protection!=image.protection ||
            image.readWord!=pending->readWord || !v232Matches(image.copy,image.physicalBase,
                addressSpace_->translate(image.virtualBase),recomp::gpu::NativeSwapImagePlan::Capacity))return reject(22);
        const auto fields=inspectSurfaceSwapFieldsLocked(frame->swap(),*pending,rejection);if(!fields)return std::nullopt;
        constexpr unsigned PacingRecord=0x83BFBE40;
        if(!imageProtections_.writable(PacingRecord,24) || !validGuestDataBuffer(PacingRecord,24,true) ||
            hasDispatcherWaitInRange(PacingRecord,24))return reject(23);
        for(const auto& cs:criticalSections_)if(std::uint64_t{PacingRecord}<std::uint64_t{cs.first}+28 &&
            cs.first<std::uint64_t{PacingRecord}+24)return reject(23);
        StoppedDeferredSwapInterrupt result{*fields,{}};
        for(unsigned i=0;i<6;++i)if(!readU32(PacingRecord+i*4,result.pacingRecord[i]))return reject(23);
        return result; // Raw current queue; no empty/full/direct profile assumed.
    }catch(...){return reject(24);}
}
std::uint32_t GuestImportRegistry::postOutputSwapQueuedInterruptRejectionLocked(const StoppedPostOutputSwapInterrupt& snapshot) const {
    const auto& values=snapshot.deviceValues;
    // Source1 queues page/target2 in the observed second-call empty queue. This
    // is not generic admission for direct MMIO, a full/wrapped queue or source0.
    if(snapshot.frequency!=50'000'000 || values[1] || values[9] || values[10]!=60 ||
        values[3] || values[4] || values[5]!=1 || values[6]!=1 || values[7]!=1 || values[8] || values[12] ||
        snapshot.scratch[0] || snapshot.scratch[4]!=0x0BADF00D || snapshot.scratch[5]!=0x208 ||
        std::any_of(snapshot.queuedSurfaces.begin(),snapshot.queuedSurfaces.end(),[](auto value){return value!=0;}))return 22;
    constexpr std::uint32_t PacingCallback=0x83216908,PacingRecord=0x83BFBE40;
    if(values[2]!=PacingCallback || !exactFunctionValidator_ || !exactFunctionValidator_(PacingCallback) ||
        !imageProtections_.writable(PacingRecord,24) || !validGuestDataBuffer(PacingRecord,24,true) ||
        hasActiveCriticalSectionInRange(PacingRecord,24) || hasDispatcherWaitInRange(PacingRecord,24))return 23;
    return 0;
}
std::optional<GuestImportRegistry::StoppedPostOutputSwapInterrupt> GuestImportRegistry::inspectStoppedPostOutputSwapQueuedInterrupt(
        std::uint32_t writeWord,std::uint32_t* rejection) const {
    RECOMP_REGISTRY_LOCK(lock);
    const auto snapshot=inspectPostOutputSwapInterruptLocked(writeWord,rejection);
    if(!snapshot)return std::nullopt;
    const auto reason=postOutputSwapQueuedInterruptRejectionLocked(*snapshot);
    if(reason){if(rejection)*rejection=reason;return std::nullopt;}
    unsigned base=0,mask=0;
    if(!graphicsBackend_->queryNativeScratchWriteback(base,mask) || !snapshot->wrapper.scratchStateMatches(base,mask)){
        if(rejection)*rejection=24;return std::nullopt;
    }
    return snapshot; // Owned read-only evidence, not a future memory lease.
}
std::optional<std::uint32_t> GuestImportRegistry::nativePostOutputSwapInterruptWorker() const {
    RECOMP_REGISTRY_LOCK(lock);return postOutputSwapInterruptWorker_;
}
std::optional<GuestPreparedThread> GuestImportRegistry::createNativePostOutputSwapInterruptWorker(
        std::uint32_t writeWord,const NativeCommandInterruptEnqueue& enqueue){
    RECOMP_REGISTRY_LOCK(lock);
    const auto* issuer=findThreadObject(RECOMP_REGISTRY_CALLER_TOKEN);
    if(!enqueue || postOutputSwapInterruptWorker_ || !virtualCpuAffinityBound_ || !completedNativeScaler_ ||
        !nativeCommandInterruptWorker_ || !nativeInterruptScopeStatus_.balanced || nativeInterruptScope_ ||
        RECOMP_REGISTRY_CALLER_TOKEN!=nativeSystemContextThread_ || !issuer || issuer->returned || issuer->suspendCount)
        return std::nullopt;
    const auto* previousWorker=findThreadObject(*nativeCommandInterruptWorker_);
    if(!previousWorker || !previousWorker->returned)return std::nullopt;
    const auto snapshot=inspectPostOutputSwapInterruptLocked(writeWord,nullptr);
    if(!snapshot)return std::nullopt;
    const auto worker=createNativeWorkerLocked(snapshot->handler,snapshot->device,false,2,[&](const auto& thread){
        const auto entry=makeGuestCommandInterruptEntry(thread);
        return entry && enqueue(thread,*entry);
    });
    if(worker)postOutputSwapInterruptWorker_=worker->layout.threadAddress;
    return worker; // No fallible work after enqueue; no old history changed.
}
GuestImportRegistry::NativePostOutputSwapInterruptScopeStatus GuestImportRegistry::nativePostOutputSwapInterruptScopeStatus() const {
    RECOMP_REGISTRY_LOCK(lock);return postOutputSwapInterruptScopeStatus_;
}
bool GuestImportRegistry::runNativePostOutputSwapInterruptScope(std::uint32_t writeWord,const std::function<void()>& invoke){
    RECOMP_REGISTRY_UNIQUE(lock);auto& status=postOutputSwapInterruptScopeStatus_;
    const auto refuse=[&](unsigned reason){if(!status.scope.attempted)status.rejection=reason;return false;};
    if(!invoke || nativeInterruptScope_ || status.scope.attempted || !postOutputSwapInterruptWorker_ ||
        RECOMP_REGISTRY_CALLER_TOKEN!=*postOutputSwapInterruptWorker_ || !nativeInterruptScopeStatus_.balanced || !completedNativeScaler_)
        return refuse(1);
    const auto* thread=findThreadObject(RECOMP_REGISTRY_CALLER_TOKEN);
    if(!thread || thread->initial || thread->returned || thread->suspendCount || thread->virtualCpuMask!=4)return refuse(2);
    const auto owner=threadStorage_.lookup(thread->threadId);
    const auto snapshot=inspectPostOutputSwapInterruptLocked(writeWord,nullptr);
    if(!owner || !snapshot || owner->startRoutine!=snapshot->handler || owner->startContext!=snapshot->device ||
        !makeGuestCommandInterruptEntry(*owner))return refuse(3);
    const auto admission=postOutputSwapQueuedInterruptRejectionLocked(*snapshot);
    if(admission)return refuse(admission==22?4:5);
    constexpr unsigned PacingRecord=0x83BFBE40;
    const auto allocation=*std::prev(allocations_.upper_bound(snapshot->device));
    const auto device=snapshot->device;
    nativeInterruptScope_=NativeInterruptScope{RECOMP_REGISTRY_CALLER_TOKEN,owner->layout.pcrAddress,device,
        allocation.first,allocation.second,{device+11028+4,device+17120},0,{},true};
    if(!validateNativeInterruptScopeLocked()){nativeInterruptScope_.reset();return refuse(6);}
    postOutputSwapInterruptSnapshot_=*snapshot;
    postOutputSwapInterruptSequence_.emplace(snapshot->wrapper);
    status={};status.scope={true,false,false,RECOMP_REGISTRY_CALLER_TOKEN,0,{}};
    RECOMP_REGISTRY_UNLOCK(lock); // Original imports reenter the registry; exclusive executor gate stays held.
    const auto finish=[&](bool returned){
        RECOMP_REGISTRY_RELOCK(lock);auto& scopeStatus=status.scope;
        scopeStatus.returned=returned;scopeStatus.heldLocks=nativeInterruptScope_->held;scopeStatus.lockPairs=nativeInterruptScope_->pairs;
        std::uint32_t ack=1,pacing=1;
        scopeStatus.balanced=returned && validateNativeInterruptScopeLocked() && !scopeStatus.heldLocks &&
            scopeStatus.lockPairs==std::array<std::uint32_t,2>{1,1} &&
            readU32(device+11032,ack) && !ack && readU32(device+17120,pacing) && !pacing;
        nativeInterruptScope_.reset(); // Never clear guest lock or acknowledgement words on failure.
        if(!scopeStatus.balanced){postOutputSwapInterruptSequence_->fail();status.rejection=7;return false;}
        if(!validateNativePostOutputSwapInterruptPlanLocked(writeWord)){
            postOutputSwapInterruptSequence_->fail();status.rejection=8;return false;}
        if(!postOutputSwapInterruptSequence_->guestReturned() || !validateNativePostOutputSwapGuestResultLocked(writeWord,false)){
            postOutputSwapInterruptSequence_->fail();status.rejection=9;return false;}
        std::array<std::uint32_t,6> record{};
        for(unsigned i=0;i<record.size();++i)(void)readU32(PacingRecord+i*4,record[i]);
        postOutputSwapPacingRecord_=record; // Exact returned record must remain unchanged through ACK.
        status.queuedCallbackValidated=true;return true; // Still before command ACK/poison or literal tail.
    };
    try{invoke();}catch(...){(void)finish(false);throw;}
    return finish(true);
}
std::optional<recomp::gpu::NativePostOutputSwapInterruptSequence::Phase> GuestImportRegistry::nativePostOutputSwapInterruptPhase() const {
    RECOMP_REGISTRY_LOCK(lock);if(!postOutputSwapInterruptSequence_)return std::nullopt;
    return postOutputSwapInterruptSequence_->phase();
}
bool GuestImportRegistry::validateNativePostOutputSwapInterruptPlanLocked(std::uint32_t writeWord) const {
    if(!postOutputSwapInterruptSnapshot_)return false;
    const auto current=inspectPostOutputSwapInterruptLocked(writeWord,nullptr,true);
    const auto& before=*postOutputSwapInterruptSnapshot_;
    return current && current->wrapper.sameRequest(before.wrapper) && current->handler==before.handler &&
        current->device==before.device && current->frequency==before.frequency;
}
bool GuestImportRegistry::validateNativePostOutputSwapGuestResultLocked(std::uint32_t writeWord,bool poisoned) const {
    if(!validateNativePostOutputSwapInterruptPlanLocked(writeWord))return false;
    const auto current=inspectPostOutputSwapInterruptLocked(writeWord,nullptr);
    if(!current)return false;
    const auto& before=*postOutputSwapInterruptSnapshot_;
    auto values=before.deviceValues;values[5]=2;values[6]=2;values[8]=1;
    auto queue=before.queuedSurfaces;queue[0]=before.wrapper.command().identity().imagePhysical;queue[1]=2;
    auto scratch=before.scratch;scratch[4]=poisoned?0x0BADF00D:before.wrapper.callbackAddress();scratch[5]=before.wrapper.callbackContext();
    if(current->deviceValues!=values || current->queuedSurfaces!=queue || current->scratch!=scratch)return false;
    // Only comparisons: never manufacture the original handler's zero or queue.
    constexpr unsigned recordAddress=0x83BFBE40;
    if(!imageProtections_.writable(recordAddress,24) || !validGuestDataBuffer(recordAddress,24,true) ||
        hasActiveCriticalSectionInRange(recordAddress,24) || hasDispatcherWaitInRange(recordAddress,24))return false;
    std::array<unsigned,6> record{};
    for(unsigned i=0;i<6;++i)if(!readU32(recordAddress+i*4,record[i]))return false;
    if(postOutputSwapPacingRecord_ && record!=*postOutputSwapPacingRecord_)return false;
    return record[0]==1 && record[1]==2 && record[2]==0 && record[3]==1 && record[4]>=1 && record[4]<=100 && record[5]==2;
}
bool GuestImportRegistry::nativePostOutputSwapInterruptActionLocked(std::uint32_t writeWord,
        const recomp::gpu::NativePostOutputSwapWrapper::Operation& op,
        const recomp::gpu::NativePostOutputSwapInterruptSequence::Action& nativeCommand){
    using Kind=recomp::gpu::NativePostOutputSwapWrapper::Kind;
    if(!nativeCommand || !validateNativePostOutputSwapInterruptPlanLocked(writeWord))return false;
    if(op.kind==Kind::ScratchWrite || op.kind==Kind::WaitScratch){
        if(op.target>=8 || op.target==1)return false;
        const auto scratch=resolvePhysicalRange(postOutputSwapInterruptSnapshot_->wrapper.scratchPhysical(),32,true);
        if(!scratch)return false;
        const auto address=scratch->virtualAddress+op.target*4;
        if(op.kind==Kind::ScratchWrite){
            if(!op.value || !writeU32(address,op.value))return false; // No host zero acknowledgements.
        }else{unsigned value=0;if(!readU32(address,value) || value!=op.value)return false;}
    }
    return nativeCommand(op) && validateNativePostOutputSwapInterruptPlanLocked(writeWord);
}
bool GuestImportRegistry::beginNativePostOutputSwapInterrupt(std::uint32_t writeWord,
        const recomp::gpu::NativePostOutputSwapInterruptSequence::Action& nativeCommand){
    RECOMP_REGISTRY_LOCK(lock);
    if(!nativeInterruptScope_ || !nativeInterruptScope_->postOutputSwap || !validateNativeInterruptScopeLocked() ||
        !postOutputSwapInterruptSequence_ || !nativeCommand || !validateNativePostOutputSwapInterruptPlanLocked(writeWord))return false;
    const auto current=inspectPostOutputSwapInterruptLocked(writeWord,nullptr,true);
    const auto& before=*postOutputSwapInterruptSnapshot_;
    if(!current || current->deviceValues!=before.deviceValues || current->queuedSurfaces!=before.queuedSurfaces || current->scratch!=before.scratch)return false;
    unsigned base=0,mask=0;if(!graphicsBackend_->queryNativeScratchWriteback(base,mask))return false;
    const auto action=[&](const auto& op){return nativePostOutputSwapInterruptActionLocked(writeWord,op,nativeCommand);};
    if(!postOutputSwapInterruptSequence_->prefixWith(base,mask,action))return false;
    const auto after=inspectPostOutputSwapInterruptLocked(writeWord,nullptr,true);
    auto expected=before.scratch;expected[0]=4;expected[4]=before.wrapper.callbackAddress();expected[5]=before.wrapper.callbackContext();
    if(!after || after->scratch!=expected || after->deviceValues!=before.deviceValues || after->queuedSurfaces!=before.queuedSurfaces){
        postOutputSwapInterruptSequence_->fail();return false;}
    return postOutputSwapInterruptSequence_->enterGuest();
}
bool GuestImportRegistry::acknowledgeNativePostOutputSwapInterrupt(std::uint32_t writeWord,
        const recomp::gpu::NativePostOutputSwapInterruptSequence::Action& nativeCommand){
    RECOMP_REGISTRY_LOCK(lock);using Phase=recomp::gpu::NativePostOutputSwapInterruptSequence::Phase;
    if(nativeInterruptScope_ || !postOutputSwapInterruptWorker_ || RECOMP_REGISTRY_CALLER_TOKEN!=*postOutputSwapInterruptWorker_ ||
        !postOutputSwapInterruptSequence_ || postOutputSwapInterruptSequence_->phase()!=Phase::GuestReturned ||
        !postOutputSwapInterruptScopeStatus_.queuedCallbackValidated || !nativeCommand)return false;
    const auto* thread=findThreadObject(RECOMP_REGISTRY_CALLER_TOKEN);
    if(!thread || thread->initial || thread->returned || thread->suspendCount || thread->virtualCpuMask!=4)return false;
    const auto fail=[&]{postOutputSwapInterruptSequence_->fail();return false;};
    unsigned base=0,mask=0;
    if(!graphicsBackend_->queryNativeScratchWriteback(base,mask) || !postOutputSwapInterruptSequence_->plan().scratchStateMatches(base,mask) ||
        !validateNativePostOutputSwapGuestResultLocked(writeWord,false))return fail();
    const auto action=[&](const auto& op){return nativePostOutputSwapInterruptActionLocked(writeWord,op,nativeCommand);};
    if(!postOutputSwapInterruptSequence_->acknowledgeWith(action) || !validateNativePostOutputSwapGuestResultLocked(writeWord,true))return fail();
    return true;
}
std::uint32_t GuestImportRegistry::deferredSwapQueuedInterruptRejectionLocked(const StoppedDeferredSwapInterrupt& now) const {
    // Admit precisely the already verified second callback's nonempty history,
    // not an arbitrary ring state or a replay of its empty-queue admission.
    if(!postOutputSwapInterruptSnapshot_ || !postOutputSwapPacingRecord_ || !completedPostOutputWrappedSwap_ ||
        !postOutputSwapInterruptScopeStatus_.queuedCallbackValidated || !postOutputSwapInterruptSequence_ ||
        postOutputSwapInterruptSequence_->phase()!=recomp::gpu::NativePostOutputSwapInterruptSequence::Phase::Acknowledged)return 25;
    const auto& old=*postOutputSwapInterruptSnapshot_;
    auto values=old.deviceValues;values[5]=2;values[6]=2;values[8]=1;
    auto queue=old.queuedSurfaces;queue[0]=old.wrapper.command().identity().imagePhysical;queue[1]=2;
    auto scratch=old.scratch;scratch[4]=0x0BADF00D;scratch[5]=old.wrapper.callbackContext();
    if(now.device!=old.device || now.handler!=old.handler || now.frequency!=50'000'000 || now.frequency!=old.frequency ||
        now.wrapper.callbackAddress()!=old.wrapper.callbackAddress() || now.wrapper.scratchPhysical()!=old.wrapper.scratchPhysical() ||
        now.deviceValues!=values || now.queuedSurfaces!=queue || now.scratch!=scratch ||
        now.pacingRecord!=*postOutputSwapPacingRecord_)return 26;
    if(now.deviceValues[2]!=0x83216908 || !exactFunctionValidator_ || !exactFunctionValidator_(0x83216908))return 27;
    unsigned base=0,mask=0;
    if(!graphicsBackend_->queryNativeScratchWriteback(base,mask) || !now.wrapper.scratchStateMatches(base,mask))return 28;
    return 0;
}
std::optional<std::uint32_t> GuestImportRegistry::nativeDeferredSwapInterruptWorker() const {
    RECOMP_REGISTRY_LOCK(lock);return deferredSwapInterruptWorker_;
}
std::optional<GuestPreparedThread> GuestImportRegistry::createNativeDeferredSwapInterruptWorker(
        std::uint32_t writeWord,const NativeCommandInterruptEnqueue& enqueue){
    RECOMP_REGISTRY_LOCK(lock);
    const auto* issuer=findThreadObject(RECOMP_REGISTRY_CALLER_TOKEN);
    if(!enqueue || deferredSwapInterruptWorker_ || !virtualCpuAffinityBound_ || !postOutputSwapInterruptWorker_ ||
        nativeInterruptScope_ || RECOMP_REGISTRY_CALLER_TOKEN!=nativeSystemContextThread_ ||
        !issuer || issuer->returned || issuer->suspendCount)return std::nullopt;
    const auto* previous=findThreadObject(*postOutputSwapInterruptWorker_);
    const auto snapshot=inspectDeferredSwapInterruptLocked(writeWord,nullptr);
    if(!previous || !previous->returned || !snapshot || deferredSwapQueuedInterruptRejectionLocked(*snapshot))return std::nullopt;
    const auto worker=createNativeWorkerLocked(snapshot->handler,snapshot->device,false,2,[&](const auto& thread){
        const auto entry=makeGuestCommandInterruptEntry(thread);return entry && enqueue(thread,*entry);
    });
    if(worker)deferredSwapInterruptWorker_=worker->layout.threadAddress;
    return worker;
}
GuestImportRegistry::NativeDeferredSwapInterruptScopeStatus GuestImportRegistry::nativeDeferredSwapInterruptScopeStatus() const {
    RECOMP_REGISTRY_LOCK(lock);return deferredSwapInterruptScopeStatus_;
}
bool GuestImportRegistry::runNativeDeferredSwapInterruptScope(std::uint32_t writeWord,const std::function<void()>& invoke){
    RECOMP_REGISTRY_UNIQUE(lock);auto& status=deferredSwapInterruptScopeStatus_;
    const auto refuse=[&](unsigned reason){if(!status.scope.attempted)status.rejection=reason;return false;};
    if(!invoke || nativeInterruptScope_ || status.scope.attempted || !deferredSwapInterruptWorker_ ||
        RECOMP_REGISTRY_CALLER_TOKEN!=*deferredSwapInterruptWorker_)return refuse(1);
    const auto* thread=findThreadObject(RECOMP_REGISTRY_CALLER_TOKEN);
    if(!thread || thread->initial || thread->returned || thread->suspendCount || thread->virtualCpuMask!=4)return refuse(2);
    const auto owner=threadStorage_.lookup(thread->threadId);
    const auto snapshot=inspectDeferredSwapInterruptLocked(writeWord,nullptr);
    if(!owner || !snapshot || owner->startRoutine!=snapshot->handler || owner->startContext!=snapshot->device ||
        !makeGuestCommandInterruptEntry(*owner))return refuse(3);
    const auto admission=deferredSwapQueuedInterruptRejectionLocked(*snapshot);if(admission)return refuse(admission);
    const auto allocation=*std::prev(allocations_.upper_bound(snapshot->device));const auto device=snapshot->device;
    nativeInterruptScope_=NativeInterruptScope{RECOMP_REGISTRY_CALLER_TOKEN,owner->layout.pcrAddress,device,
        allocation.first,allocation.second,{device+11032,device+17120},0,{},false,true};
    if(!validateNativeInterruptScopeLocked()){nativeInterruptScope_.reset();return refuse(6);}
    deferredSwapInterruptSnapshot_=*snapshot;deferredSwapInterruptSequence_.emplace(snapshot->wrapper);
    status={};status.scope={true,false,false,RECOMP_REGISTRY_CALLER_TOKEN,0,{}};
    RECOMP_REGISTRY_UNLOCK(lock); // All peer writers remain parked by the caller's exclusive executor.
    const auto finish=[&](bool returned){
        RECOMP_REGISTRY_RELOCK(lock);auto& scope=status.scope;
        scope.returned=returned;scope.heldLocks=nativeInterruptScope_->held;scope.lockPairs=nativeInterruptScope_->pairs;
        unsigned ack=1,pacing=1;
        scope.balanced=returned && validateNativeInterruptScopeLocked() && !scope.heldLocks &&
            scope.lockPairs==std::array<std::uint32_t,2>{1,1} &&
            readU32(device+11032,ack) && !ack && readU32(device+17120,pacing) && !pacing;
        nativeInterruptScope_.reset(); // Never manufacture a lock release or scratch acknowledgement.
        const auto fail=[&](unsigned reason){deferredSwapInterruptSequence_->fail();status.rejection=reason;return false;};
        if(!scope.balanced)return fail(7);
        if(!validateNativeDeferredSwapInterruptPlanLocked(writeWord))return fail(8);
        if(!deferredSwapInterruptSequence_->guestReturned() || !validateNativeDeferredSwapGuestResultLocked(writeWord,false))return fail(9);
        std::array<unsigned,6> record{};
        for(unsigned i=0;i<record.size();++i)if(!readU32(0x83BFBE40+i*4,record[i]))return fail(9);
        deferredSwapPacingRecord_=record;status.queuedCallbackValidated=true;return true;
    };
    try{invoke();}catch(...){(void)finish(false);throw;}
    return finish(true);
}
std::optional<recomp::gpu::NativeDeferredSwapInterruptSequence::Phase> GuestImportRegistry::nativeDeferredSwapInterruptPhase() const {
    RECOMP_REGISTRY_LOCK(lock);if(!deferredSwapInterruptSequence_)return std::nullopt;
    return deferredSwapInterruptSequence_->phase();
}
bool GuestImportRegistry::validateNativeDeferredSwapInterruptPlanLocked(std::uint32_t writeWord) const {
    if(!deferredSwapInterruptSnapshot_)return false;
    const auto now=inspectDeferredSwapInterruptLocked(writeWord,nullptr,true);const auto& old=*deferredSwapInterruptSnapshot_;
    return now && now->wrapper.sameRequest(old.wrapper) && now->handler==old.handler &&
        now->device==old.device && now->frequency==old.frequency &&
        now->submission.rootPhysical==old.submission.rootPhysical;
}
bool GuestImportRegistry::validateNativeDeferredSwapGuestResultLocked(std::uint32_t writeWord,bool poisoned) const {
    if(!validateNativeDeferredSwapInterruptPlanLocked(writeWord))return false;
    const auto now=inspectDeferredSwapInterruptLocked(writeWord,nullptr);if(!now)return false;
    const auto& old=*deferredSwapInterruptSnapshot_;
    auto values=old.deviceValues;values[5]=3;values[6]=3;values[8]=2;
    auto queue=old.queuedSurfaces;queue[2]=old.wrapper.command().identity().imagePhysical;queue[3]=3;
    auto scratch=old.scratch;scratch[4]=poisoned?0x0BADF00D:old.wrapper.callbackAddress();scratch[5]=old.wrapper.callbackContext();
    if(now->deviceValues!=values || now->queuedSurfaces!=queue || now->scratch!=scratch)return false;
    const auto& record=now->pacingRecord;
    if(deferredSwapPacingRecord_ && record!=*deferredSwapPacingRecord_)return false;
    return record[0]==1 && record[1]==3 && record[2]==0 && record[3]==2 && record[4]>=1 && record[4]<=100 && record[5]==3;
}
bool GuestImportRegistry::nativeDeferredSwapInterruptActionLocked(std::uint32_t writeWord,
        const recomp::gpu::NativeDeferredSwapWrapper::Operation& op,
        const recomp::gpu::NativeDeferredSwapInterruptSequence::Action& nativeCommand){
    using Kind=recomp::gpu::NativeDeferredSwapWrapper::Kind;
    if(!nativeCommand || !validateNativeDeferredSwapInterruptPlanLocked(writeWord))return false;
    if(op.kind==Kind::ScratchWrite || op.kind==Kind::WaitScratch){
        if(op.target>=8 || op.target==1)return false;
        const auto scratch=resolvePhysicalRange(deferredSwapInterruptSnapshot_->wrapper.scratchPhysical(),32,true);if(!scratch)return false;
        const auto address=scratch->virtualAddress+op.target*4;
        if(op.kind==Kind::ScratchWrite){if(!op.value || !writeU32(address,op.value))return false;}
        else{unsigned value=0;if(!readU32(address,value) || value!=op.value)return false;}
    }
    return nativeCommand(op) && validateNativeDeferredSwapInterruptPlanLocked(writeWord);
}
bool GuestImportRegistry::beginNativeDeferredSwapInterrupt(std::uint32_t writeWord,
        const recomp::gpu::NativeDeferredSwapInterruptSequence::Action& nativeCommand){
    RECOMP_REGISTRY_LOCK(lock);
    if(!nativeInterruptScope_ || !nativeInterruptScope_->deferredSwap || nativeInterruptScope_->postOutputSwap ||
        !validateNativeInterruptScopeLocked() || !deferredSwapInterruptSequence_ || !nativeCommand ||
        !validateNativeDeferredSwapInterruptPlanLocked(writeWord))return false;
    const auto now=inspectDeferredSwapInterruptLocked(writeWord,nullptr,true);const auto& old=*deferredSwapInterruptSnapshot_;
    if(!now || now->deviceValues!=old.deviceValues || now->queuedSurfaces!=old.queuedSurfaces ||
        now->scratch!=old.scratch || now->pacingRecord!=old.pacingRecord)return false;
    unsigned base=0,mask=0;if(!graphicsBackend_->queryNativeScratchWriteback(base,mask))return false;
    const auto action=[&](const auto& op){return nativeDeferredSwapInterruptActionLocked(writeWord,op,nativeCommand);};
    if(!deferredSwapInterruptSequence_->prefixWith(base,mask,action))return false;
    const auto after=inspectDeferredSwapInterruptLocked(writeWord,nullptr,true);
    auto expected=old.scratch;expected[0]=4;expected[4]=old.wrapper.callbackAddress();expected[5]=old.wrapper.callbackContext();
    if(!after || after->scratch!=expected || after->deviceValues!=old.deviceValues ||
        after->queuedSurfaces!=old.queuedSurfaces || after->pacingRecord!=old.pacingRecord){
        deferredSwapInterruptSequence_->fail();return false;}
    return deferredSwapInterruptSequence_->enterGuest();
}
bool GuestImportRegistry::acknowledgeNativeDeferredSwapInterrupt(std::uint32_t writeWord,
        const recomp::gpu::NativeDeferredSwapInterruptSequence::Action& nativeCommand){
    RECOMP_REGISTRY_LOCK(lock);using Phase=recomp::gpu::NativeDeferredSwapInterruptSequence::Phase;
    if(nativeInterruptScope_ || !deferredSwapInterruptWorker_ || RECOMP_REGISTRY_CALLER_TOKEN!=*deferredSwapInterruptWorker_ ||
        !deferredSwapInterruptSequence_ || deferredSwapInterruptSequence_->phase()!=Phase::GuestReturned ||
        !deferredSwapInterruptScopeStatus_.queuedCallbackValidated || !nativeCommand)return false;
    const auto* thread=findThreadObject(RECOMP_REGISTRY_CALLER_TOKEN);
    if(!thread || thread->initial || thread->returned || thread->suspendCount || thread->virtualCpuMask!=4)return false;
    const auto fail=[&]{deferredSwapInterruptSequence_->fail();return false;};unsigned base=0,mask=0;
    if(!graphicsBackend_->queryNativeScratchWriteback(base,mask) || !deferredSwapInterruptSequence_->plan().scratchStateMatches(base,mask) ||
        !validateNativeDeferredSwapGuestResultLocked(writeWord,false))return fail();
    const auto action=[&](const auto& op){return nativeDeferredSwapInterruptActionLocked(writeWord,op,nativeCommand);};
    if(!deferredSwapInterruptSequence_->acknowledgeWith(action) || !validateNativeDeferredSwapGuestResultLocked(writeWord,true))return fail();
    return true;
}
bool GuestImportRegistry::validateNativeScalerStorageLocked() const noexcept {
    if (!nativeIdentityScalerBound_ || !nativeScalerStorageOwner_ || !graphicsInitialized_ ||
        !graphicsBackend_ || !graphicsBackend_->ready() || !graphicsRingConfiguration_ ||
        !graphicsRingConfiguration_->writeBack || !graphicsRingAllocation_ ||
        !graphicsWriteBackAllocation_ || !graphicsIdentifierAllocation_) return false;
    const auto& original=*nativeScalerStorageOwner_;
    if (!original.byteCount || original.byteCount>8*1024*1024 || !original.writable ||
        original.virtualBase==*graphicsRingAllocation_ || original.virtualBase==*graphicsWriteBackAllocation_ ||
        original.virtualBase==*graphicsIdentifierAllocation_) return false;
    const auto owner=resolvePhysicalRange(original.physicalBase,original.byteCount,true);
    const auto allocation=physicalAllocations_.find(original.virtualBase);
    return owner && owner->allocationBase==original.virtualBase && owner->virtualAddress==original.virtualBase &&
        allocation!=physicalAllocations_.end() && allocation->second.size==original.byteCount &&
        allocation->second.protection==original.protection;
}
std::optional<GuestImportRegistry::NativePendingSubmission> GuestImportRegistry::inspectStoppedNativeScalerSubmission(std::uint32_t writeWord) const {
    RECOMP_REGISTRY_LOCK(lock);
    return inspectStoppedNativeScalerSubmissionLocked(writeWord);
}
std::optional<recomp::gpu::NativeScalerWrapper> GuestImportRegistry::inspectStoppedNativeScalerWrapper(std::uint32_t writeWord) const {
    RECOMP_REGISTRY_LOCK(lock);
    return planNativeScalerWrapperLocked(writeWord);
}
std::optional<recomp::gpu::NativeScalerWrapper> GuestImportRegistry::planNativeScalerWrapperLocked(std::uint32_t writeWord) const {
    const auto pending=inspectStoppedNativeScalerSubmissionLocked(writeWord);
    using Wrapper=recomp::gpu::NativeScalerWrapper;
    if (!pending || pending->childWords!=Wrapper::Words || pending->commandOffsetWords!=Wrapper::CommandOffsetWords) return std::nullopt;
    const auto owner=resolvePhysicalRange(pending->childPhysical,Wrapper::Bytes,false);
    if (!owner) return std::nullopt;
    return Wrapper::parse(*pendingNativeScaler_,pending->childPhysical,completedScalerStorageCoherency_->commandPhysical,
        addressSpace_->translate(owner->virtualAddress),Wrapper::Bytes);
}
std::optional<GuestPreparedThread> GuestImportRegistry::createNativeCommandInterruptWorker(
        std::uint32_t writeWord,const NativeCommandInterruptEnqueue& enqueue) {
    RECOMP_REGISTRY_LOCK(lock);
    const auto* current=findThreadObject(RECOMP_REGISTRY_CALLER_TOKEN);
    if (!enqueue || nativeCommandInterruptWorker_ || !nativeIdentityScalerBound_ || !pendingNativeScaler_ ||
        !graphicsInterruptRegistration_ || !virtualCpuAffinityBound_ || !exactFunctionValidator_ ||
        RECOMP_REGISTRY_CALLER_TOKEN!=pendingNativeScaler_->identity().issuerThread || !current ||
        current->initial || current->returned || current->suspendCount) return std::nullopt;
    noteNativeSubmissionMayCommitAt(3); // v183 site 3: may publish completion: the renderer's pending reads first
    const auto wrapper=planNativeScalerWrapperLocked(writeWord);
    const auto registration=*graphicsInterruptRegistration_;
    if (!wrapper || !exactFunctionValidator_(registration.routine) || !exactFunctionValidator_(wrapper->callbackAddress())) return std::nullopt;
    const auto worker=createNativeWorkerLocked(registration.routine,registration.userData,false,2,[&](const auto& thread) {
        const auto entry=makeGuestCommandInterruptEntry(thread);
        return entry && enqueue(thread,*entry);
    });
    if (worker) nativeCommandInterruptWorker_=worker->layout.threadAddress; // No fallible work after enqueue.
    return worker;
}
std::optional<GuestImportRegistry::NativePendingSubmission> GuestImportRegistry::inspectStoppedNativeScalerSubmissionLocked(std::uint32_t writeWord) const {
    if (!pendingNativeScaler_ || !completedScalerStorageCoherency_ || !nativeScalerRing_ ||
        !validateNativeScalerStorageLocked()) return std::nullopt;
    const auto& ring=*graphicsRingConfiguration_;const auto& original=*nativeScalerRing_;
    const auto previous=completedScalerStorageCoherency_->readWord;
    if (!original.writeBack || ring.readWord!=previous || std::uint64_t{previous}+3!=writeWord || writeWord>=ring.bytes/4 ||
        ring.physicalAddress!=original.physicalAddress || ring.virtualAddress!=original.virtualAddress || ring.bytes!=original.bytes ||
        ring.writeBack->physicalAddress!=original.writeBack->physicalAddress || ring.writeBack->virtualAddress!=original.writeBack->virtualAddress ||
        ring.writeBack->blockSizeLog2!=original.writeBack->blockSizeLog2) return std::nullopt;
    std::uint32_t published=0;
    if (!readU32(ring.writeBack->virtualAddress,published) || published!=previous) return std::nullopt;
    std::array<std::uint32_t,3> root{};
    for (unsigned i=0;i<3;++i) if (!readU32(ring.virtualAddress+(previous+i)*4,root[i])) return std::nullopt;
    if (root[0]!=0xC0013F00 || (root[1]&3) || root[2]<64 || root[2]>1024 ||
        std::uint64_t{root[1]}+root[2]*4>0x20000000) return std::nullopt;
    const auto& storage=*nativeScalerStorageOwner_;
    const auto offset=pendingNativeScaler_->uniqueOffset(addressSpace_->translate(storage.virtualBase),storage.byteCount);
    const auto command=completedScalerStorageCoherency_->commandPhysical;
    const auto owner=resolvePhysicalRange(root[1],root[2]*4,false);
    if (!offset || storage.physicalBase+*offset!=command || !owner || owner->allocationBase!=storage.virtualBase ||
        root[1]>command || std::uint64_t{command}+recomp::gpu::NativeScalerCommand::Bytes>std::uint64_t{root[1]}+root[2]*4)
        return std::nullopt;
    return NativePendingSubmission{ring.physicalAddress+previous*4,root[1],root[2],(command-root[1])/4,previous,writeWord};
}
bool GuestImportRegistry::runNativeCommandInterruptScope(std::uint32_t writeWord,const std::function<void()>& invoke) {
    RECOMP_REGISTRY_UNIQUE(lock);
    const auto refuse=[&](unsigned reason) {
        if (!nativeInterruptScopeStatus_.attempted) nativeScalerInterruptStatus_.rejection=reason;
        return false;
    };
    if (!invoke || nativeInterruptScope_ || nativeInterruptScopeStatus_.attempted || !nativeCommandInterruptWorker_ ||
        RECOMP_REGISTRY_CALLER_TOKEN!=*nativeCommandInterruptWorker_ || !graphicsInterruptRegistration_) return refuse(8);
    const auto* thread=findThreadObject(RECOMP_REGISTRY_CALLER_TOKEN);
    if (!thread || thread->initial || thread->returned || thread->suspendCount || thread->virtualCpuMask!=4) return refuse(9);
    const auto owner=threadStorage_.lookup(thread->threadId);
    const auto wrapper=planNativeScalerWrapperLocked(writeWord);
    const auto& registration=*graphicsInterruptRegistration_;
    if (!owner || !wrapper || owner->startRoutine!=registration.routine || owner->startContext!=registration.userData ||
        !makeGuestCommandInterruptEntry(*owner) || !exactFunctionValidator_ ||
        !exactFunctionValidator_(registration.routine) || !exactFunctionValidator_(wrapper->callbackAddress())) return refuse(10);
    // Observed source1 handler uses device+11028 scratch pointer and +11032
    // acknowledgement lock; its posted callback uses device+17120 pacing lock.
    // This is a deliberately narrow native profile, not a generic IRQ service.
    // Posted callback also reads +21964 and writes its timebase at +23788.
    constexpr std::uint32_t DeviceBytes=23792;
    const auto device=registration.userData;
    if (device<heapBase_ || std::uint64_t{device}+DeviceBytes>std::uint64_t{heapBase_}+heapSize_ ||
        !validGuestDataBuffer(device,DeviceBytes,true)) return refuse(11);
    const auto after=allocations_.upper_bound(device);
    if (after==allocations_.begin()) return refuse(11);
    const auto allocation=*std::prev(after);
    const auto scratch=resolvePhysicalRange(wrapper->scratchPhysical(),32,true);
    if (!scratch) return refuse(14);
    // Ownership spans the full object, but synchronization exclusion concerns
    // the audited source1 handler's accesses, not unrelated embedded events.
    // Include the whole pacing/ring block conservatively, plus scratch words.
    // beginNativeScalerInterrupt separately excludes unreviewed optional calls.
    const std::array<std::pair<std::uint32_t,std::uint32_t>,5> touched{{
        {device+11028,8},{device+16944,180},{device+21964,4},{device+23788,4},
        {scratch->virtualAddress,32}}};
    for (const auto& [address,bytes]:touched)
        if (hasActiveCriticalSectionInRange(address,bytes)) return refuse(12);
    for (const auto& [address,bytes]:touched)
        if (hasDispatcherWaitInRange(address,bytes)) return refuse(13);
    std::uint32_t pointer=0,ackLock=1,pacingLock=1;
    if (!readU32(device+11028,pointer) || pointer!=scratch->virtualAddress) return refuse(14);
    if (!readU32(device+11032,ackLock) || ackLock) return refuse(15);
    if (!readU32(device+17120,pacingLock) || pacingLock) return refuse(16);
    nativeInterruptScope_=NativeInterruptScope{RECOMP_REGISTRY_CALLER_TOKEN,owner->layout.pcrAddress,device,
        allocation.first,allocation.second,{device+11032,device+17120}};
    if (!validateNativeInterruptScopeLocked()) {nativeInterruptScope_.reset();return refuse(17);}
    nativeScalerInterruptStatus_.rejection=0;
    nativeInterruptScopeStatus_={true,false,false,RECOMP_REGISTRY_CALLER_TOKEN,0,{}};
    // The caller retains the executor's non-yielding gate; never hold this mutex
    // across translated guest code, which reenters dispatch for real imports.
    RECOMP_REGISTRY_UNLOCK(lock);
    const auto finish=[&](bool returned) {
        RECOMP_REGISTRY_RELOCK(lock);
        auto& status=nativeInterruptScopeStatus_;
        status.returned=returned;
        status.heldLocks=nativeInterruptScope_->held;
        status.lockPairs=nativeInterruptScope_->pairs;
        std::uint32_t ack=1,pacing=1;
        status.balanced=returned && validateNativeInterruptScopeLocked() && !status.heldLocks &&
            status.lockPairs==std::array<std::uint32_t,2>{1,1} &&
            readU32(nativeInterruptScope_->locks[0],ack) && !ack && readU32(nativeInterruptScope_->locks[1],pacing) && !pacing;
        nativeInterruptScope_.reset(); // Never clear guest lock words on unwind.
        return status.balanced;
    };
    try {invoke();} catch (...) {(void)finish(false);throw;}
    return finish(true);
}
GuestImportRegistry::NativeCommandInterruptScopeStatus GuestImportRegistry::nativeCommandInterruptScopeStatus() const {
    RECOMP_REGISTRY_LOCK(lock);return nativeInterruptScopeStatus_;
}
GuestImportRegistry::NativeScalerInterruptStatus GuestImportRegistry::nativeScalerInterruptStatus() const {
    RECOMP_REGISTRY_LOCK(lock);return nativeScalerInterruptStatus_;
}
bool GuestImportRegistry::validateNativeScalerInterruptPlanLocked(std::uint32_t writeWord) const {
    if (!nativeScalerInterruptPlan_ || !nativeScalerInterruptScratchOwner_) return false;
    const auto plan=planNativeScalerWrapperLocked(writeWord);
    const auto& original=*nativeScalerInterruptScratchOwner_;
    const auto owner=resolvePhysicalRange(nativeScalerInterruptPlan_->scratchPhysical(),32,true);
    const auto allocation=physicalAllocations_.find(original.virtualBase);
    return plan && plan->sameRequest(*nativeScalerInterruptPlan_) && owner &&
        owner->allocationBase==original.virtualBase && allocation!=physicalAllocations_.end() &&
        allocation->second.size==original.byteCount && allocation->second.protection==original.protection;
}
bool GuestImportRegistry::nativeScalerInterruptMemoryActionLocked(const recomp::gpu::NativeScalerWrapper::Operation& op) {
    using Kind=recomp::gpu::NativeScalerWrapper::Kind;
    if (!nativeScalerInterruptPlan_ || op.target>=8) return false;
    const auto owner=resolvePhysicalRange(nativeScalerInterruptPlan_->scratchPhysical(),32,true);
    if (!owner) return false;
    const auto address=owner->virtualAddress+op.target*4;
    if (op.kind==Kind::ScratchWrite) {
        // The backend may request only literal writes present in the owned
        // wrapper. In particular zero acknowledgements are NEVER host writes.
        const auto& plan=*nativeScalerInterruptPlan_;
        const bool prefix=nativeScalerInterruptStatus_.prefixAttempted && !nativeScalerInterruptStatus_.prefixReady &&
            ((op.wordOffset==2 && op.target==1 && op.value==1) ||
             (op.wordOffset==12 && op.target==4 && op.value==plan.callbackAddress()) ||
             (op.wordOffset==12 && op.target==5 && op.value==plan.callbackContext()) ||
             (op.wordOffset==15 && op.target==0 && op.value==4));
        const bool poison=nativeScalerInterruptStatus_.ackAttempted && op.wordOffset==45 && op.target==4 && op.value==0x0BADF00D;
        return (prefix || poison) && writeU32(address,op.value);
    }
    std::uint32_t value=0;
    return op.kind==Kind::WaitScratch && readU32(address,value) && value==op.value;
}
bool GuestImportRegistry::beginNativeScalerInterrupt(std::uint32_t writeWord) {
    RECOMP_REGISTRY_LOCK(lock);auto& status=nativeScalerInterruptStatus_;
    if (status.prefixAttempted) return false;
    const auto reject=[&](unsigned reason) {status.rejection=reason;return false;};
    if (!validateNativeInterruptScopeLocked() || !nativeInterruptScopeStatus_.attempted || nativeInterruptScopeStatus_.returned)
        return reject(1);
    const auto plan=planNativeScalerWrapperLocked(writeWord);
    if (!plan || !graphicsBackend_ || !graphicsBackend_->ready()) return reject(2);
    const auto scratch=resolvePhysicalRange(plan->scratchPhysical(),32,true);
    if (!scratch) return reject(3);
    const auto overlaps=[&](unsigned address,unsigned bytes) {
        return std::uint64_t{scratch->virtualAddress}<std::uint64_t{address}+bytes &&
            std::uint64_t{address}<std::uint64_t{scratch->virtualAddress}+32;
    };
    if (overlaps(graphicsRingConfiguration_->virtualAddress,graphicsRingConfiguration_->bytes) ||
        overlaps(graphicsRingConfiguration_->writeBack->virtualAddress,4) ||
        overlaps(nativeScalerStorageOwner_->virtualBase,nativeScalerStorageOwner_->byteCount) ||
        (graphicsSystemIdentifier_ && overlaps(*graphicsSystemIdentifier_,4))) return reject(3);
    const auto allocation=physicalAllocations_.find(scratch->allocationBase);
    if (allocation==physicalAllocations_.end()) return reject(3);
    std::array<std::uint32_t,8> before{};
    for (unsigned i=0;i<8;++i) if (!readU32(scratch->virtualAddress+i*4,before[i])) return reject(3);
    if (before[0] || before[1]) return reject(3); // Never overwrite an outstanding guest acknowledgement.
    const auto device=nativeInterruptScope_->device;
    // Exported pointer storage belongs to GuestVideoDeviceGlobals, not the
    // title heap. Its explicit lifecycle binding verifies the live chain.
    std::uint32_t rate=0,count=1,callback=0;
    if (!nativeInterruptDeviceRoot_ || !nativeInterruptDeviceRoot_(device)) return reject(4);
    if (!readU32(device+21964,rate) || !readU32(device+16972,count) || !readU32(device+16944,callback)) return reject(5);
    status.pacing={1,rate,count,callback,plan->callbackContext(),performanceFrequency_};
    if (!performanceFrequency_ || static_cast<std::uint32_t>(performanceFrequency_)/(rate?rate:60)==0 || count ||
        (plan->callbackContext()&0xFFFFF000)) return reject(5);
    // This fixed-title profile's optional leaf adjusts the owned stack record
    // and copies exactly six words into writable title-image data. No calls,
    // imports or other writes. Execute the original mapped routine, never a
    // host replacement; all other optional callbacks still require review.
    constexpr std::uint32_t PacingCallback=0x83216908,PacingRecord=0x83BFBE40;
    if (callback && (callback!=PacingCallback || !exactFunctionValidator_ || !exactFunctionValidator_(callback) ||
        !imageProtections_.writable(PacingRecord,24) || !validGuestDataBuffer(PacingRecord,24,true) ||
        hasActiveCriticalSectionInRange(PacingRecord,24) || hasDispatcherWaitInRange(PacingRecord,24))) return reject(5);
    // First invocation constrains the reviewed device footprint. A nonzero
    // high context selects a different, still unsupported MMIO acknowledge.
    nativeScalerInterruptPlan_=*plan;
    nativeScalerInterruptScratchOwner_=StoppedPhysicalOwnership{plan->scratchPhysical()-(scratch->virtualAddress-scratch->allocationBase),
        scratch->allocationBase,allocation->second.size,allocation->second.protection,true};
    nativeScalerInterruptScratchBefore_=before;
    status.prefixAttempted=true;status.rejection=0;
    const auto action=[&](const auto& op) {return validateNativeScalerInterruptPlanLocked(writeWord) && nativeScalerInterruptMemoryActionLocked(op);};
    try {
        if (!RECOMP_V_BACKEND()->beginNativeScalerInterrupt(*plan,action)) return reject(6);
    } catch (...) {status.rejection=6;throw;}
    if (!validateNativeScalerInterruptPlanLocked(writeWord)) return reject(7);
    std::array<std::uint32_t,8> expected=before;
    expected[0]=4;expected[1]=1;expected[4]=plan->callbackAddress();expected[5]=plan->callbackContext();
    for (unsigned i=0;i<8;++i) {std::uint32_t value=0;
        if (!readU32(scratch->virtualAddress+i*4,value) || value!=expected[i]) return reject(7);
    }
    status.prefixReady=true;return true;
}
bool GuestImportRegistry::acknowledgeNativeScalerInterrupt(std::uint32_t writeWord) {
    RECOMP_REGISTRY_LOCK(lock);auto& status=nativeScalerInterruptStatus_;
    if (!status.prefixReady || status.ackAttempted || nativeInterruptScope_ ||
        !nativeInterruptScopeStatus_.balanced || !nativeCommandInterruptWorker_ ||
        RECOMP_REGISTRY_CALLER_TOKEN!=*nativeCommandInterruptWorker_) return false;
    status.ackAttempted=true; // Prefix/callback already ran; failed continuity is terminal.
    const auto reject=[&](unsigned reason) {status.rejection=reason;return false;};
    if (!validateNativeScalerInterruptPlanLocked(writeWord)) return reject(7);
    const auto& plan=*nativeScalerInterruptPlan_;
    const auto scratch=resolvePhysicalRange(plan.scratchPhysical(),32,true);
    auto expected=nativeScalerInterruptScratchBefore_;
    // Verify guest-produced zeros and untouched slots BEFORE sending ACK.
    expected[0]=0;expected[1]=0;expected[4]=plan.callbackAddress();expected[5]=plan.callbackContext();
    for (unsigned i=0;i<8;++i) {std::uint32_t value=0;
        if (!readU32(scratch->virtualAddress+i*4,value) || value!=expected[i]) return reject(7);
    }
    const auto action=[&](const auto& op) {return validateNativeScalerInterruptPlanLocked(writeWord) && nativeScalerInterruptMemoryActionLocked(op);};
    try {if (!RECOMP_V_BACKEND()->acknowledgeNativeScalerInterrupt(action)) return reject(6);}
    catch (...) {status.rejection=6;throw;}
    expected[4]=0x0BADF00D;
    if (!validateNativeScalerInterruptPlanLocked(writeWord)) return reject(7);
    for (unsigned i=0;i<8;++i) {std::uint32_t value=0;
        if (!readU32(scratch->virtualAddress+i*4,value) || value!=expected[i]) return reject(7);
    }
    status.acknowledged=true;return true; // Still no scaler/update/tail or RPTR retirement.
}
std::optional<GuestImportRegistry::NativeScalerCompletion> GuestImportRegistry::completedNativeScalerWrapper() const {
    RECOMP_REGISTRY_LOCK(lock);return completedNativeScaler_;
}
bool GuestImportRegistry::completeNativeScalerWrapper(std::uint32_t writeWord) {
    RECOMP_REGISTRY_LOCK(lock);
    if (nativeScalerTailAttempted_ || completedNativeScaler_ || !nativeScalerInterruptStatus_.acknowledged ||
        nativeInterruptScope_ || !nativeInterruptScopeStatus_.balanced || !nativeCommandInterruptWorker_ ||
        !pendingNativeScaler_ || RECOMP_REGISTRY_CALLER_TOKEN!=pendingNativeScaler_->identity().issuerThread ||
        !validateNativeScalerInterruptPlanLocked(writeWord)) return false;
    const auto* issuer=findThreadObject(RECOMP_REGISTRY_CALLER_TOKEN);
    const auto* worker=findThreadObject(*nativeCommandInterruptWorker_);
    if (!issuer || issuer->returned || issuer->suspendCount || !worker || !worker->returned) return false;
    const auto& plan=*nativeScalerInterruptPlan_;auto& ring=*graphicsRingConfiguration_;
    const auto scratch=resolvePhysicalRange(plan.scratchPhysical(),32,true);if (!scratch) return false;
    auto expectedScratch=nativeScalerInterruptScratchBefore_;
    expectedScratch[0]=0;expectedScratch[1]=0;expectedScratch[4]=0x0BADF00D;expectedScratch[5]=plan.callbackContext();
    const auto scratchIntact=[&] {
        for (unsigned i=0;i<8;++i) {std::uint32_t value=0;
            if (!readU32(scratch->virtualAddress+i*4,value) || value!=expectedScratch[i]) return false;
        }return true;
    };
    if (!scratchIntact() || !graphicsIdentifierAllocation_ || !graphicsSystemIdentifier_) return false;
    NativeScalerCompletion completion{ring.readWord,writeWord,{},{}};
    std::array<std::uint32_t,2> destinations{};
    std::array<std::uint8_t,8> previousEvents{};
    const auto overlaps=[](unsigned a,unsigned n,unsigned b,unsigned m) {
        return std::uint64_t{a}<std::uint64_t{b}+m && std::uint64_t{b}<std::uint64_t{a}+n;
    };
    for (unsigned i=0;i<2;++i) {
        const auto physical=plan.operations()[24+i].target;
        const auto owner=resolvePhysicalRange(physical,4,true);
        if (!owner || owner->allocationBase!=*graphicsIdentifierAllocation_ ||
            overlaps(owner->virtualAddress,4,ring.virtualAddress,ring.bytes) ||
            overlaps(owner->virtualAddress,4,ring.writeBack->virtualAddress,4) ||
            overlaps(owner->virtualAddress,4,*graphicsSystemIdentifier_,4) ||
            overlaps(owner->virtualAddress,4,nativeScalerStorageOwner_->virtualBase,nativeScalerStorageOwner_->byteCount) ||
            overlaps(owner->virtualAddress,4,scratch->virtualAddress,32)) return false;
        for (const auto& cs:criticalSections_) if (overlaps(owner->virtualAddress,4,cs.first,28)) return false;
        if (hasDispatcherWaitInRange(owner->virtualAddress,4)) return false;
        destinations[i]=owner->virtualAddress;completion.eventPhysical[i]=physical;
        std::memcpy(previousEvents.data()+i*4,addressSpace_->translate(owner->virtualAddress),4);
    }
    std::array<std::uint8_t,4> next{};for (unsigned b=0;b<4;++b) next[b]=writeWord>>(24-b*8);
    nativeScalerTailAttempted_=true;
    bool completed=false;
    try {completed=RECOMP_V_BACKEND()->completeNativeScalerTail(completion.gpu);} catch (...) {return false;}
    if (!completed) return false;
    // Native state/events are completed. Refuse contradictory late mutations
    // by terminating, never retiring different work or offering another try.
    if (!recomp::gpu::validNativeScalerTailResult(plan,completion.gpu) ||
        !validateNativeScalerInterruptPlanLocked(writeWord) || !scratchIntact()) std::_Exit(4);
    for (unsigned i=0;i<2;++i) {
        const auto owner=resolvePhysicalRange(completion.eventPhysical[i],4,true);
        if (!owner || owner->allocationBase!=*graphicsIdentifierAllocation_ || owner->virtualAddress!=destinations[i] ||
            std::memcmp(previousEvents.data()+i*4,addressSpace_->translate(destinations[i]),4)) std::_Exit(4);
    }
    // No allocating or fallible work after first guest publication. Preserve
    // issuance metadata, publish the two audited event words then RPTR last.
    completedNativeScaler_=completion;
    for (unsigned i=0;i<2;++i)
        std::memcpy(addressSpace_->translate(destinations[i]),completion.gpu.eventBytes.data()+i*4,4);
    std::memcpy(addressSpace_->translate(ring.writeBack->virtualAddress),next.data(),4);
    ring.readWord=writeWord;return true;
}
std::optional<GuestImportRegistry::NativeScalerStorageCompletion> GuestImportRegistry::completedNativeScalerStorageCoherency() const {
    RECOMP_REGISTRY_LOCK(lock);return completedScalerStorageCoherency_;
}
std::optional<GuestImportRegistry::NativePostScalerStorageCompletion> GuestImportRegistry::completedNativePostScalerStorageCoherency() const {
    RECOMP_REGISTRY_LOCK(lock);return completedPostScalerStorage_;
}
bool GuestImportRegistry::validateNativePostOutputScanoutLocked(bool afterDeferredStorage) const noexcept {
    using Plan=recomp::gpu::NativeSwapImagePlan;
    if(!completedPostOutputWrappedSwap_ || !completedPostOutputColor_ || !pendingPostOutputColorImage_ ||
        !completedPostOutputSwapStorage_ || pendingPostOutputSwap_ || postOutputSwapStatus_.pending ||
        !postOutputSwapStatus_.issued || !postOutputSwapStatus_.presentQueued ||
        !graphicsRingConfiguration_ || !graphicsRingConfiguration_->writeBack || !addressSpace_ ||
        !postOutputSwapInterruptScopeStatus_.queuedCallbackValidated || !postOutputSwapInterruptSequence_ ||
        postOutputSwapInterruptSequence_->phase()!=recomp::gpu::NativePostOutputSwapInterruptSequence::Phase::Acknowledged)return false;
    const auto& receipt=*completedPostOutputWrappedSwap_;const auto& copy=*pendingPostOutputColorImage_;
    const auto& request=receipt.imageRequest;const auto& saved=receipt.imageOwner;
    const auto& ring=*graphicsRingConfiguration_;std::uint32_t read=0;
    auto expectedRead=receipt.readWord;
    if(afterDeferredStorage){
        if(!pendingDeferredSwap_ || !completedDeferredSwapStorage_ ||
            completedDeferredSwapStorage_->previousWord!=receipt.readWord ||
            completedDeferredSwapStorage_->readWord!=receipt.readWord+3)return false;
        expectedRead=completedDeferredSwapStorage_->readWord;
    }
    if(receipt.gpu.nativeFences!=6 || receipt.gpu.interruptIdleFences!=1 || !receipt.gpu.interruptAcknowledged ||
        expectedRead!=ring.readWord || !readU32(ring.writeBack->virtualAddress,read) || read!=ring.readWord ||
        postOutputSwapStatus_.readWord!=receipt.readWord || receipt.previousWord!=completedPostOutputSwapStorage_->readWord ||
        copy.readWord!=completedPostOutputColor_->color.readWord || copy.readWord!=postOutputSwapStatus_.issuedReadWord ||
        request.frontbufferVirtual!=copy.virtualBase || saved.virtualBase!=copy.allocationBase ||
        saved.byteCount!=copy.allocationBytes || saved.protection!=copy.protection || !saved.writable ||
        saved.physicalBase!=physicalAddressOffset_+(copy.allocationBase-physicalArenaBase_) ||
        !Plan::make(request.fetch,request.frontbufferVirtual,copy.physicalBase,request.format,request.colorSpace,request.width,request.height))return false;
    const auto owner=resolvePhysicalRange(copy.physicalBase,Plan::Capacity,true);
    const auto allocation=physicalAllocations_.find(copy.allocationBase);
    return owner && owner->allocationBase==copy.allocationBase && owner->virtualAddress==copy.virtualBase &&
        allocation!=physicalAllocations_.end() && allocation->second.size==copy.allocationBytes && allocation->second.protection==copy.protection &&
        v232Matches(copy.copy,copy.physicalBase,addressSpace_->translate(copy.virtualBase),Plan::Capacity);
}
bool GuestImportRegistry::validateNativeScanoutLocked() const noexcept {
    // A completed copy may have changed the backing memory, even if its bytes
    // happen to match the old frame. The separate post-output validator owns
    // the new lease; old gamma/output consumers must never inherit its pixels.
    if(completedPostOutputColor_)return false;
    if (!nativeScanoutRequest_ || !completedOwnedColorImage_ || !completedWrappedSwap_ || !nativeSwapStatus_.presentQueued) return false;
    const auto& copy=*completedOwnedColorImage_;const auto& request=*nativeScanoutRequest_;
    if (request.frontbufferVirtual!=copy.virtualBase) return false;
    const auto owner=resolvePhysicalRange(copy.physicalBase,recomp::gpu::NativeSwapImagePlan::Capacity,true);
    const auto allocation=physicalAllocations_.find(copy.allocationBase);
    return owner && owner->allocationBase==copy.allocationBase && owner->virtualAddress==copy.virtualBase &&
        allocation!=physicalAllocations_.end() && allocation->second.size==copy.allocationBytes && allocation->second.protection==copy.protection &&
        v232Matches(copy.copy,copy.physicalBase,addressSpace_->translate(copy.virtualBase),recomp::gpu::NativeSwapImagePlan::Capacity);
}
std::optional<GuestImportRegistry::NativeDisplayGammaCompletion> GuestImportRegistry::completedNativeDisplayGamma() const {
    RECOMP_REGISTRY_LOCK(lock);return completedDisplayGamma_;
}
bool GuestImportRegistry::submitNativeDisplayGamma(std::uint32_t writeWord) {
    using namespace recomp::gpu;
    RECOMP_REGISTRY_LOCK(lock);
    if (nativeDisplayGammaAttempted_ || completedDisplayGamma_ || !completedNativeScaler_ || !completedPostScalerStorage_ ||
        !nativeScalerRing_ || !nativeSwapConsumer_ || !graphicsSystemIdentifier_ ||
        !validateNativeScalerStorageLocked() || !validateNativeScanoutLocked()) return false;
    const auto previous=completedPostScalerStorage_->readWord;
    auto& ring=*graphicsRingConfiguration_;const auto& original=*nativeScalerRing_;
    if (!original.writeBack || std::uint64_t{previous}+3!=writeWord || writeWord>=ring.bytes/4) return false;
    noteNativeSubmissionMayCommitAt(4); // v183 site 4: may publish completion: the renderer's pending reads first
    const auto continuity=[&] {
        if (!validateNativeScalerStorageLocked() || !validateNativeScanoutLocked() || ring.readWord!=previous ||
            ring.physicalAddress!=original.physicalAddress || ring.virtualAddress!=original.virtualAddress || ring.bytes!=original.bytes ||
            ring.writeBack->physicalAddress!=original.writeBack->physicalAddress ||
            ring.writeBack->virtualAddress!=original.writeBack->virtualAddress ||
            ring.writeBack->blockSizeLog2!=original.writeBack->blockSizeLog2) return false;
        const auto r=resolvePhysicalRange(ring.physicalAddress,ring.bytes,false),w=resolvePhysicalRange(ring.writeBack->physicalAddress,4,true);
        std::uint32_t value=0;
        return r && r->allocationBase==*graphicsRingAllocation_ && r->virtualAddress==ring.virtualAddress &&
            w && w->allocationBase==*graphicsWriteBackAllocation_ && w->virtualAddress==ring.writeBack->virtualAddress &&
            readU32(ring.writeBack->virtualAddress,value) && value==previous;
    };
    if (!continuity()) return false;
    std::array<std::uint32_t,3> root{};
    for(unsigned i=0;i<3;++i)if(!readU32(ring.virtualAddress+(previous+i)*4,root[i]))return false;
    if (root[0]!=0xC0013F00 || root[2]!=NativeDisplayGammaWrapper::Words || (root[1]&3)) return false;
    const auto child=resolvePhysicalRange(root[1],NativeDisplayGammaWrapper::Bytes,false);
    if (!child || child->allocationBase!=nativeScalerStorageOwner_->virtualBase) return false;
    const auto& copy=*completedOwnedColorImage_;const auto& request=*nativeScanoutRequest_;
    const auto plan=NativeDisplayGammaWrapper::parse(root[1],addressSpace_->translate(child->virtualAddress),NativeDisplayGammaWrapper::Bytes,copy.physicalBase);
    const auto imagePlan=NativeSwapImagePlan::make(request.fetch,copy.virtualBase,copy.physicalBase,request.format,request.colorSpace,request.width,request.height);
    if (!plan || !imagePlan) return false;
    const auto overlaps=[](unsigned a,unsigned n,unsigned b,unsigned m) {return std::uint64_t{a}<std::uint64_t{b}+m && std::uint64_t{b}<std::uint64_t{a}+n;};
    for (const auto& cs:criticalSections_) if(overlaps(child->virtualAddress,NativeDisplayGammaWrapper::Bytes,cs.first,28)) return false;
    if(hasDispatcherWaitInRange(child->virtualAddress,NativeDisplayGammaWrapper::Bytes)) return false;
    NativeDisplayGammaCompletion completion{previous,writeWord,root[1],copy.physicalBase,{},{}};
    std::array<unsigned,2> destinations{};std::array<std::uint8_t,8> beforeEvents{};
    for(unsigned i=0;i<2;++i) {
        const auto owner=resolvePhysicalRange(plan->eventPhysical(i),4,true);
        if (!owner || owner->allocationBase!=*graphicsIdentifierAllocation_ ||
            overlaps(owner->virtualAddress,4,ring.virtualAddress,ring.bytes) || overlaps(owner->virtualAddress,4,ring.writeBack->virtualAddress,4) ||
            overlaps(owner->virtualAddress,4,*graphicsSystemIdentifier_,4) ||
            overlaps(owner->virtualAddress,4,nativeScalerStorageOwner_->virtualBase,nativeScalerStorageOwner_->byteCount) ||
            overlaps(owner->virtualAddress,4,copy.virtualBase,NativeSwapImagePlan::Capacity) ||
            (nativeScalerInterruptPlan_ && overlaps(plan->eventPhysical(i),4,nativeScalerInterruptPlan_->scratchPhysical(),32))) return false;
        for (const auto& cs:criticalSections_) if(overlaps(owner->virtualAddress,4,cs.first,28)) return false;
        if (hasDispatcherWaitInRange(owner->virtualAddress,4)) return false;
        destinations[i]=owner->virtualAddress;completion.eventPhysical[i]=plan->eventPhysical(i);
        std::memcpy(beforeEvents.data()+i*4,addressSpace_->translate(owner->virtualAddress),4);
    }
    NativeOwnedSwapImage image{request,{physicalAddressOffset_+copy.allocationBase-physicalArenaBase_,copy.allocationBase,
        copy.allocationBytes,copy.protection,true},copy.physicalBase,{}};
    try {
        if (!imagePlan->decode(addressSpace_->translate(copy.virtualBase),NativeSwapImagePlan::Capacity,image.rgba) ||
            !plan->ramp().mapRgba(image.rgba.data(),image.rgba.size(),image.rgba)) return false;
    } catch (...) {return false;}
    std::array<std::uint8_t,4> next{};for(unsigned b=0;b<4;++b)next[b]=writeWord>>(24-b*8);
    bool accepted=false,invoked=false,completed=false;
    const std::function<bool()> present=[&] {
        if (invoked) {if(accepted)std::_Exit(4);return false;}invoked=true;
        accepted=nativeSwapConsumer_(image);return accepted;
    };
    nativeDisplayGammaAttempted_=true;
    try {completed=RECOMP_V_BACKEND()->executeNativeDisplayGamma(*plan,present,completion.gpu);}
    catch (...) {if (accepted) std::_Exit(4);return false;}
    if (!accepted) return false;
    if (!completed || !validNativeDisplayGammaResult(*plan,completion.gpu) || !continuity() ||
        !plan->matches(root[1],addressSpace_->translate(child->virtualAddress),NativeDisplayGammaWrapper::Bytes)) std::_Exit(4);
    std::uint32_t current=0;
    for(unsigned i=0;i<3;++i)if(!readU32(ring.virtualAddress+(previous+i)*4,current) || current!=root[i])std::_Exit(4);
    for(unsigned i=0;i<2;++i) {
        const auto owner=resolvePhysicalRange(completion.eventPhysical[i],4,true);
        if (!owner || owner->allocationBase!=*graphicsIdentifierAllocation_ || owner->virtualAddress!=destinations[i] ||
            std::memcmp(beforeEvents.data()+i*4,addressSpace_->translate(destinations[i]),4)) std::_Exit(4);
    }
    completedDisplayGamma_=completion;
    for(unsigned i=0;i<2;++i)std::memcpy(addressSpace_->translate(destinations[i]),completion.gpu.eventBytes.data()+i*4,4);
    std::memcpy(addressSpace_->translate(ring.writeBack->virtualAddress),next.data(),4);ring.readWord=writeWord;
    return true;
}
std::optional<GuestImportRegistry::NativeDisplayOutputCompletion> GuestImportRegistry::completedNativeDisplayOutput() const {
    RECOMP_REGISTRY_LOCK(lock);return completedDisplayOutput_;
}
bool GuestImportRegistry::submitExperimentalNativeDisplayOutput(std::uint32_t writeWord) {
    using namespace recomp::gpu;
    RECOMP_REGISTRY_LOCK(lock);
    if (nativeDisplayOutputAttempted_ || completedDisplayOutput_ || !completedDisplayGamma_ || !completedPostDisplayStorage_ ||
        !nativeScalerRing_ || !nativeSwapConsumer_ || !graphicsSystemIdentifier_ ||
        !validateNativeScalerStorageLocked() || !validateNativeScanoutLocked()) return false;
    const auto previous=completedPostDisplayStorage_->readWord;
    auto& ring=*graphicsRingConfiguration_;const auto& original=*nativeScalerRing_;
    if (!original.writeBack || std::uint64_t{previous}+3!=writeWord || writeWord>=ring.bytes/4) return false;
    noteNativeSubmissionMayCommitAt(5); // v183 site 5: may publish completion: the renderer's pending reads first
    const auto continuity=[&] {
        if (!validateNativeScalerStorageLocked() || !validateNativeScanoutLocked() || ring.readWord!=previous ||
            ring.physicalAddress!=original.physicalAddress || ring.virtualAddress!=original.virtualAddress || ring.bytes!=original.bytes ||
            ring.writeBack->physicalAddress!=original.writeBack->physicalAddress ||
            ring.writeBack->virtualAddress!=original.writeBack->virtualAddress ||
            ring.writeBack->blockSizeLog2!=original.writeBack->blockSizeLog2) return false;
        const auto r=resolvePhysicalRange(ring.physicalAddress,ring.bytes,false),w=resolvePhysicalRange(ring.writeBack->physicalAddress,4,true);
        std::uint32_t value=0;
        return r && r->allocationBase==*graphicsRingAllocation_ && r->virtualAddress==ring.virtualAddress &&
            w && w->allocationBase==*graphicsWriteBackAllocation_ && w->virtualAddress==ring.writeBack->virtualAddress &&
            readU32(ring.writeBack->virtualAddress,value) && value==previous;
    };
    if (!continuity()) return false;
    std::array<std::uint32_t,3> root{};
    for(unsigned i=0;i<3;++i)if(!readU32(ring.virtualAddress+(previous+i)*4,root[i]))return false;
    if (root[0]!=0xC0013F00 || root[2]!=NativeDisplayOutputWrapper::Words || (root[1]&3)) return false;
    const auto child=resolvePhysicalRange(root[1],NativeDisplayOutputWrapper::Bytes,false);
    if (!child || child->allocationBase!=nativeScalerStorageOwner_->virtualBase) return false;
    const auto& copy=*completedOwnedColorImage_;const auto& request=*nativeScanoutRequest_;
    const auto plan=NativeDisplayOutputWrapper::parse(root[1],addressSpace_->translate(child->virtualAddress),NativeDisplayOutputWrapper::Bytes);
    const auto imagePlan=NativeSwapImagePlan::make(request.fetch,copy.virtualBase,copy.physicalBase,request.format,request.colorSpace,request.width,request.height);
    if (!plan || !imagePlan) return false;
    const auto overlaps=[](unsigned a,unsigned n,unsigned b,unsigned m) {return std::uint64_t{a}<std::uint64_t{b}+m && std::uint64_t{b}<std::uint64_t{a}+n;};
    for (const auto& cs:criticalSections_) if(overlaps(child->virtualAddress,NativeDisplayOutputWrapper::Bytes,cs.first,28)) return false;
    if(hasDispatcherWaitInRange(child->virtualAddress,NativeDisplayOutputWrapper::Bytes)) return false;
    NativeDisplayOutputCompletion completion{previous,writeWord,root[1],{},{}};
    std::array<unsigned,2> destinations{};std::array<std::uint8_t,8> beforeEvents{};
    for(unsigned i=0;i<2;++i) {
        const auto owner=resolvePhysicalRange(plan->eventPhysical(i),4,true);
        if (!owner || owner->allocationBase!=*graphicsIdentifierAllocation_ ||
            overlaps(owner->virtualAddress,4,ring.virtualAddress,ring.bytes) || overlaps(owner->virtualAddress,4,ring.writeBack->virtualAddress,4) ||
            overlaps(owner->virtualAddress,4,*graphicsSystemIdentifier_,4) ||
            overlaps(owner->virtualAddress,4,nativeScalerStorageOwner_->virtualBase,nativeScalerStorageOwner_->byteCount) ||
            overlaps(owner->virtualAddress,4,copy.virtualBase,NativeSwapImagePlan::Capacity) ||
            (nativeScalerInterruptPlan_ && overlaps(plan->eventPhysical(i),4,nativeScalerInterruptPlan_->scratchPhysical(),32))) return false;
        for (const auto& cs:criticalSections_) if(overlaps(owner->virtualAddress,4,cs.first,28)) return false;
        if (hasDispatcherWaitInRange(owner->virtualAddress,4)) return false;
        destinations[i]=owner->virtualAddress;completion.eventPhysical[i]=plan->eventPhysical(i);
        std::memcpy(beforeEvents.data()+i*4,addressSpace_->translate(owner->virtualAddress),4);
    }
    NativeOwnedSwapImage image{request,{physicalAddressOffset_+copy.allocationBase-physicalArenaBase_,copy.allocationBase,
        copy.allocationBytes,copy.protection,true},copy.physicalBase,{}};
    // Do not apply gamma here: the installed native scanout consumer does it
    // once, unlike the initial gamma transaction before its ramp was installed.
    try {if (!imagePlan->decode(addressSpace_->translate(copy.virtualBase),NativeSwapImagePlan::Capacity,image.rgba))return false;}
    catch (...) {return false;}
    std::array<std::uint8_t,4> next{};for(unsigned b=0;b<4;++b)next[b]=writeWord>>(24-b*8);
    bool accepted=false,invoked=false,completed=false;
    const std::function<bool()> present=[&] {
        if (invoked) {if(accepted)std::_Exit(4);return false;}invoked=true;
        accepted=nativeSwapConsumer_(image);return accepted;
    };
    nativeDisplayOutputAttempted_=true;
    try {completed=RECOMP_V_BACKEND()->executeExperimentalNativeDisplayOutput(*plan,present,completion.gpu);}
    catch (...) {if (accepted) std::_Exit(4);return false;}
    if (!accepted) return false;
    if (!completed || !validNativeDisplayOutputResult(*plan,completion.gpu) || !continuity() ||
        !plan->matches(root[1],addressSpace_->translate(child->virtualAddress),NativeDisplayOutputWrapper::Bytes)) std::_Exit(4);
    std::uint32_t current=0;
    for(unsigned i=0;i<3;++i)if(!readU32(ring.virtualAddress+(previous+i)*4,current) || current!=root[i])std::_Exit(4);
    for(unsigned i=0;i<2;++i) {
        const auto owner=resolvePhysicalRange(completion.eventPhysical[i],4,true);
        if (!owner || owner->allocationBase!=*graphicsIdentifierAllocation_ || owner->virtualAddress!=destinations[i] ||
            std::memcmp(beforeEvents.data()+i*4,addressSpace_->translate(destinations[i]),4)) std::_Exit(4);
    }
    completedDisplayOutput_=completion;
    for(unsigned i=0;i<2;++i)std::memcpy(addressSpace_->translate(destinations[i]),completion.gpu.eventBytes.data()+i*4,4);
    std::memcpy(addressSpace_->translate(ring.writeBack->virtualAddress),next.data(),4);ring.readWord=writeWord;
    return true;
}
bool GuestImportRegistry::submitNativePostScalerStorageCoherency(std::uint32_t writeWord) {
    RECOMP_REGISTRY_LOCK(lock);
    return submitNativeStorageCoherencyLocked(writeWord,NativeStoragePhase::Scaler);
}
bool GuestImportRegistry::submitNativePostDisplayStorageCoherency(std::uint32_t writeWord) {
    RECOMP_REGISTRY_LOCK(lock);
    return submitNativeStorageCoherencyLocked(writeWord,NativeStoragePhase::Display);
}
std::optional<GuestImportRegistry::NativePostScalerStorageCompletion> GuestImportRegistry::completedNativePostDisplayStorageCoherency() const {
    RECOMP_REGISTRY_LOCK(lock);return completedPostDisplayStorage_;
}
bool GuestImportRegistry::submitNativePostOutputStorageCoherency(std::uint32_t writeWord) {
    RECOMP_REGISTRY_LOCK(lock);
    return submitNativeStorageCoherencyLocked(writeWord,NativeStoragePhase::Output);
}
std::optional<GuestImportRegistry::NativePostScalerStorageCompletion> GuestImportRegistry::completedNativePostOutputStorageCoherency() const {
    RECOMP_REGISTRY_LOCK(lock);return completedPostOutputStorage_;
}
bool GuestImportRegistry::submitNativeRuntimeCommandStorageCoherency(std::uint32_t writeWord) {
    RECOMP_REGISTRY_LOCK(lock);
    return submitNativeStorageCoherencyLocked(writeWord,NativeStoragePhase::Runtime);
}
std::optional<GuestImportRegistry::NativePostScalerStorageCompletion> GuestImportRegistry::completedNativeRuntimeCommandStorageCoherency() const {
    RECOMP_REGISTRY_LOCK(lock);return completedRuntimeCommandStorage_;
}
bool GuestImportRegistry::submitNativeStorageCoherencyLocked(std::uint32_t writeWord,NativeStoragePhase phase) {
    const bool runtime=phase==NativeStoragePhase::Runtime;
    const bool afterOutput=runtime || phase==NativeStoragePhase::Output,afterDisplay=phase!=NativeStoragePhase::Scaler;
    auto& failed=runtime?nativeRuntimeCommandStorageFailed_:
        (afterOutput?nativePostOutputStorageFailed_:(afterDisplay?nativePostDisplayStorageFailed_:nativePostScalerStorageFailed_));
    auto& receipt=runtime?completedRuntimeCommandStorage_:
        (afterOutput?completedPostOutputStorage_:(afterDisplay?completedPostDisplayStorage_:completedPostScalerStorage_));
    if(runtime && (!completedPostOutputStorage_ || pendingNativeSwap_ || pendingPostOutputSwap_ || pendingDeferredSwap_ ||
        nativeInterruptScope_ || nativeTileSceneInterruptScope_))return false;
    if (failed || !completedNativeScaler_ || !nativeScalerRing_ ||
        afterDisplay!=completedDisplayGamma_.has_value() || afterOutput!=completedDisplayOutput_.has_value() ||
        !validateNativeScalerStorageLocked()) return false;
    auto& ring=*graphicsRingConfiguration_;const auto& original=*nativeScalerRing_;
    const auto& storage=*nativeScalerStorageOwner_;
    // Startup phase cursors keep their original meaning. Runtime consumes only
    // the next exact 3-word IB after the registry's currently completed cursor,
    // revalidated against the owned writeback below. It does not claim any
    // intervening words or repurpose an earlier startup fence as completion.
    const auto previous=runtime?ring.readWord:(receipt?receipt->readWord:(afterOutput?completedDisplayOutput_->readWord:
        (afterDisplay?completedDisplayGamma_->readWord:completedNativeScaler_->readWord)));
    auto completion=receipt.value_or(NativePostScalerStorageCompletion{});
    if (completion.batches>=(runtime?UINT32_MAX:256u) || std::uint64_t{previous}+3!=writeWord || writeWord>=ring.bytes/4) return false;
    if(!runtime) noteNativeSubmissionMayCommitAt(6); // v183 site 6: may publish completion: the renderer's pending reads first
    const auto continuity=[&] {
        if (!validateNativeScalerStorageLocked() || !original.writeBack || ring.readWord!=previous ||
            ring.physicalAddress!=original.physicalAddress || ring.virtualAddress!=original.virtualAddress ||
            ring.bytes!=original.bytes || ring.writeBack->physicalAddress!=original.writeBack->physicalAddress ||
            ring.writeBack->virtualAddress!=original.writeBack->virtualAddress ||
            ring.writeBack->blockSizeLog2!=original.writeBack->blockSizeLog2) return false;
        const auto ownedRing=resolvePhysicalRange(ring.physicalAddress,ring.bytes,false);
        const auto ownedWb=resolvePhysicalRange(ring.writeBack->physicalAddress,4,true);
        std::uint32_t current=0;
        return ownedRing && ownedRing->allocationBase==*graphicsRingAllocation_ && ownedRing->virtualAddress==ring.virtualAddress &&
            ownedWb && ownedWb->allocationBase==*graphicsWriteBackAllocation_ && ownedWb->virtualAddress==ring.writeBack->virtualAddress &&
            readU32(ring.writeBack->virtualAddress,current) && current==previous;
    };
    if (!continuity()) return false;
    const auto overlaps=[](std::uint32_t a,std::uint32_t n,std::uint32_t b,std::uint32_t m) {
        return std::uint64_t{a}<std::uint64_t{b}+m && std::uint64_t{b}<std::uint64_t{a}+n;
    };
    // v219: range-bounded (cs_range_query.h), same predicates as the scans of every section / event wait (~50 calls/s under R).
    if (anyActiveOverlap(criticalSections_,storage.virtualBase,storage.byteCount,28,[](std::uint32_t,const auto&){return true;})) return false;
    if (anyActiveOverlap(dispatcherEventWaits_,storage.virtualBase,storage.byteCount,16,[](std::uint32_t,const auto&){return true;})) return false;
    std::vector<std::uint32_t> root(3),child(11);
    for (unsigned i=0;i<3;++i) if (!readU32(ring.virtualAddress+(previous+i)*4,root[i])) return false;
    if (root[0]!=0xC0013F00 || root[2]!=11 || (root[1]&3) || std::uint64_t{root[1]}+44>0x20000000) return false;
    const auto owner=resolvePhysicalRange(root[1],44,false);
    if (!owner || owner->allocationBase!=storage.virtualBase) return false;
    for (unsigned i=0;i<11;++i) if (!readU32(owner->virtualAddress+i*4,child[i])) return false;
    NativeInitialCoherency request;
    if (!parseNativeEmptyCacheCoherency(root,child,request) || request.cacheFlags!=0x01000000 ||
        request.physicalBase!=storage.physicalBase || request.byteCount!=storage.byteCount) return false;
    if (!completion.batches) completion.firstWord=previous;
    ++completion.batches;completion.previousWord=previous;completion.readWord=writeWord;completion.request=request;
    std::array<std::uint8_t,4> next{};
    for (unsigned i=0;i<4;++i) next[i]=writeWord>>(24-i*8);
    failed=true; // No retries after a partial native failure, separately per phase.
    bool completed=false;
    try {completed=runtime?RECOMP_V_BACKEND()->completeNativeRuntimeCommandStorageCoherency(request):
        (afterOutput?RECOMP_V_BACKEND()->completeNativePostOutputStorageCoherency(request):
        (afterDisplay?RECOMP_V_BACKEND()->completeNativePostDisplayStorageCoherency(request):
        RECOMP_V_BACKEND()->completeNativePostScalerStorageCoherency(request)));} catch (...) {return false;}
    if (!completed) return false;
    if (!continuity()) std::_Exit(4);
    std::uint32_t current=0;
    for (unsigned i=0;i<3;++i)
        if (!readU32(ring.virtualAddress+(previous+i)*4,current) || current!=root[i]) std::_Exit(4);
    for (unsigned i=0;i<11;++i)
        if (!readU32(owner->virtualAddress+i*4,current) || current!=child[i]) std::_Exit(4);
    receipt=completion;
    std::memcpy(addressSpace_->translate(ring.writeBack->virtualAddress),next.data(),next.size());
    ring.readWord=writeWord;failed=false;
    return true;
}
bool GuestImportRegistry::submitNativeScalerStorageCoherency(std::uint32_t writeWord) {
    RECOMP_REGISTRY_LOCK(lock);
    if (!pendingNativeScaler_ || !nativeScalerRing_ || completedScalerStorageCoherency_ ||
        !nativeScalerTimingBinding_ || !completedWrappedSwap_ || !validateNativeScalerStorageLocked()) return false;
    using Command=recomp::gpu::NativeScalerCommand;
    const auto& command=*pendingNativeScaler_;const auto& storage=*nativeScalerStorageOwner_;
    auto& ring=*graphicsRingConfiguration_;const auto& original=*nativeScalerRing_;
    const auto previous=command.identity().previousReadWord;
    if (previous!=completedWrappedSwap_->readWord || std::uint64_t{previous}+3!=writeWord || writeWord>=ring.bytes/4) return false;
    noteNativeSubmissionMayCommitAt(7); // v183 site 7: may publish completion: the renderer's pending reads first
    const auto continuity=[&] {
        if (!validateNativeScalerStorageLocked() || !original.writeBack || ring.readWord!=previous ||
            original.readWord!=previous || ring.physicalAddress!=original.physicalAddress ||
            ring.virtualAddress!=original.virtualAddress || ring.bytes!=original.bytes ||
            ring.writeBack->physicalAddress!=original.writeBack->physicalAddress ||
            ring.writeBack->virtualAddress!=original.writeBack->virtualAddress ||
            ring.writeBack->blockSizeLog2!=original.writeBack->blockSizeLog2) return false;
        std::uint32_t read=0,pointer=0,clock=0;
        const auto& timing=*nativeScalerTimingBinding_;
        return readU32(ring.writeBack->virtualAddress,read) && read==previous &&
            readU32(timing[0],pointer) && pointer==timing[1] && readU32(timing[1],clock) && clock==500;
    };
    if (!continuity()) return false;
    std::vector<std::uint32_t> root(3),child(11);
    for (unsigned i=0;i<3;++i) if (!readU32(ring.virtualAddress+(previous+i)*4,root[i])) return false;
    if (root[0]!=0xC0013F00 || root[2]!=11 || (root[1]&3) || std::uint64_t{root[1]}+44>0x20000000) return false;
    const auto childOwner=resolvePhysicalRange(root[1],44,false);
    if (!childOwner || childOwner->allocationBase!=storage.virtualBase) return false;
    for (unsigned i=0;i<11;++i) if (!readU32(childOwner->virtualAddress+i*4,child[i])) return false;
    NativeInitialCoherency request;
    if (!parseNativeEmptyCacheCoherency(root,child,request) || request.cacheFlags!=0x01000000 ||
        request.physicalBase!=storage.physicalBase || request.byteCount!=storage.byteCount) return false;
    const auto offset=command.uniqueOffset(addressSpace_->translate(storage.virtualBase),storage.byteCount);
    if (!offset) return false;
    const auto commandPhysical=storage.physicalBase+*offset;
    const auto overlaps=[](std::uint32_t a,std::uint32_t n,std::uint32_t b,std::uint32_t m) {
        return std::uint64_t{a}<std::uint64_t{b}+m && std::uint64_t{b}<std::uint64_t{a}+n;
    };
    if (overlaps(root[1],44,commandPhysical,Command::Bytes)) return false;
    if (anyActiveOverlap(criticalSections_,storage.virtualBase,storage.byteCount,28,[](std::uint32_t,const auto&){return true;})) return false; // v219
    if (anyActiveOverlap(dispatcherEventWaits_,storage.virtualBase,storage.byteCount,16,[](std::uint32_t,const auto&){return true;})) return false; // v219
    std::array<std::uint8_t,4> next{};
    for (unsigned i=0;i<4;++i) next[i]=writeWord>>(24-i*8);
    const NativeScalerStorageCompletion completion{previous,writeWord,commandPhysical,request};
    bool ordered=false;
    try {ordered=RECOMP_V_BACKEND()->completeNativeScalerStorageCoherency(request);} catch (...) {return false;}
    if (!ordered) return false;
    // Queue ordering happened; any contradictory native callback mutation now
    // fail-stops instead of retiring different commands or offering a retry.
    if (!continuity() || command.uniqueOffset(addressSpace_->translate(storage.virtualBase),storage.byteCount)!=offset) std::_Exit(4);
    std::uint32_t current=0;
    for (unsigned i=0;i<3;++i)
        if (!readU32(ring.virtualAddress+(previous+i)*4,current) || current!=root[i]) std::_Exit(4);
    for (unsigned i=0;i<11;++i)
        if (!readU32(childOwner->virtualAddress+i*4,current) || current!=child[i]) std::_Exit(4);
    completedScalerStorageCoherency_=completion;
    std::memcpy(addressSpace_->translate(ring.writeBack->virtualAddress),next.data(),next.size());
    ring.readWord=writeWord;
    return true; // Scaler command remains pending; no event/image/presentation.
}
bool GuestImportRegistry::submitNativeCommandStorageCoherency(std::uint32_t writeWord){
    RECOMP_REGISTRY_LOCK(lock);
    return submitNativeSwapStorageLocked(writeWord,false);
}
bool GuestImportRegistry::submitNativePostOutputSwapStorageCoherency(std::uint32_t writeWord){
    RECOMP_REGISTRY_LOCK(lock);
    return submitNativeSwapStorageLocked(writeWord,true);
}
std::optional<GuestImportRegistry::NativeCommandStorageCompletion> GuestImportRegistry::completedNativeDeferredSwapStorageCoherency() const {
    RECOMP_REGISTRY_LOCK(lock);return completedDeferredSwapStorage_;
}
bool GuestImportRegistry::validatePendingDeferredSwapLocked() const noexcept {
    using Plan=recomp::gpu::NativeSwapImagePlan;
    if(!pendingDeferredSwap_ || !postSwapSystemContext_ || !graphicsSystemIdentifier_ || !nativeSwapConsumer_ ||
        !validateNativeScalerStorageLocked() || !validateNativePostOutputScanoutLocked(completedDeferredSwapStorage_.has_value()))return false;
    const auto& pending=*pendingDeferredSwap_;const auto& metadata=pending.metadata;
    const auto& request=metadata.request;const auto& image=metadata.imageOwner;
    const auto& ring=*graphicsRingConfiguration_;const auto& original=pending.ring;
    if(!original.writeBack || original.readWord!=metadata.issuedReadWord || metadata.issuedReadWord!=postSwapSystemContext_->readWord ||
        pending.identifierAddress!=*graphicsSystemIdentifier_ || ring.physicalAddress!=original.physicalAddress ||
        ring.virtualAddress!=original.virtualAddress || ring.bytes!=original.bytes ||
        ring.writeBack->physicalAddress!=original.writeBack->physicalAddress || ring.writeBack->virtualAddress!=original.writeBack->virtualAddress ||
        ring.writeBack->blockSizeLog2!=original.writeBack->blockSizeLog2 ||
        pending.pinnedAllocations!=std::array<std::uint32_t,5>{request.allocationBase,image.virtualBase,*graphicsRingAllocation_,
            *graphicsWriteBackAllocation_,*graphicsIdentifierAllocation_} ||
        pending.command.identity().ticket!=3 || pending.command.identity().previousReadWord!=metadata.issuedReadWord ||
        pending.command.identity().commandPhysical!=request.commandPhysical || pending.command.identity().imagePhysical!=metadata.imagePhysical ||
        request.allocationBase!=nativeScalerStorageOwner_->virtualBase || request.allocationBytes!=nativeScalerStorageOwner_->byteCount ||
        request.protection!=nativeScalerStorageOwner_->protection || image.physicalBase!=metadata.imagePhysical ||
        image.virtualBase!=request.frontbufferVirtual || !image.writable ||
        !Plan::make(request.fetch,image.virtualBase,image.physicalBase,request.format,request.colorSpace,request.width,request.height))return false;
    const auto command=resolvePhysicalRange(request.commandPhysical,256,true);
    const auto pixels=resolvePhysicalRange(image.physicalBase,Plan::Capacity,true);
    const auto pixelOwner=physicalAllocations_.find(image.virtualBase);
    const auto ringOwner=resolvePhysicalRange(ring.physicalAddress,ring.bytes,true);
    const auto wb=resolvePhysicalRange(ring.writeBack->physicalAddress,4,true);
    const auto idPhysical=std::uint64_t{pending.identifierAddress}-physicalArenaBase_+physicalAddressOffset_;
    if(idPhysical>UINT32_MAX)return false;
    const auto id=resolvePhysicalRange(static_cast<std::uint32_t>(idPhysical),4,true);
    if(!command || command->allocationBase!=request.allocationBase || command->virtualAddress!=request.commandVirtual ||
        !pending.command.matches(request.commandPhysical,addressSpace_->translate(command->virtualAddress),256) ||
        !pixels || pixels->allocationBase!=image.virtualBase || pixels->virtualAddress!=image.virtualBase ||
        pixelOwner==physicalAllocations_.end() || pixelOwner->second.size!=image.byteCount || pixelOwner->second.protection!=image.protection ||
        !ringOwner || ringOwner->allocationBase!=pending.pinnedAllocations[2] || ringOwner->virtualAddress!=ring.virtualAddress ||
        !wb || wb->allocationBase!=pending.pinnedAllocations[3] || wb->virtualAddress!=ring.writeBack->virtualAddress ||
        !id || id->allocationBase!=pending.pinnedAllocations[4] || id->virtualAddress!=pending.identifierAddress)return false;
    // All future consumers must revalidate these live spans. Metadata alone
    // cannot establish current image pixels or rendering completion.
    for(const auto& span:std::array<std::pair<std::uint32_t,std::uint32_t>,5>{{{request.commandVirtual,256},
            {image.virtualBase,image.byteCount},{ring.virtualAddress,ring.bytes},{ring.writeBack->virtualAddress,4},{pending.identifierAddress,4}}}){
        if(hasDispatcherWaitInRange(span.first,span.second))return false;
        for(const auto& cs:criticalSections_)if(std::uint64_t{span.first}<std::uint64_t{cs.first}+28 &&
            cs.first<std::uint64_t{span.first}+span.second)return false;
    }
    return true;
}
bool GuestImportRegistry::submitNativeDeferredSwapStorageCoherency(std::uint32_t writeWord){
    RECOMP_REGISTRY_LOCK(lock);
    if(deferredSwapStorageAttempted_ || completedDeferredSwapStorage_ || RECOMP_REGISTRY_CALLER_TOKEN!=nativeSystemContextThread_ ||
        !validatePendingDeferredSwapLocked())return false;
    const auto* thread=findThreadObject(RECOMP_REGISTRY_CALLER_TOKEN);
    if(!thread || thread->returned || thread->suspendCount)return false;
    const auto& pending=*pendingDeferredSwap_;auto& ring=*graphicsRingConfiguration_;
    const auto previous=ring.readWord;
    if(previous!=pending.metadata.issuedReadWord || std::uint64_t{previous}+3!=writeWord || writeWord>=ring.bytes/4)return false;
    noteNativeSubmissionMayCommitAt(8); // v183 site 8: may publish completion: the renderer's pending reads first
    std::vector<std::uint32_t> root(3),child(11);
    for(unsigned i=0;i<3;++i)if(!readU32(ring.virtualAddress+(previous+i)*4,root[i]))return false;
    if(root[0]!=0xC0013F00 || root[2]!=11 || (root[1]&3) || std::uint64_t{root[1]}+44>0x20000000)return false;
    const auto owner=resolvePhysicalRange(root[1],44,false);
    if(!owner || owner->allocationBase!=pending.metadata.request.allocationBase ||
        (root[1]<std::uint64_t{pending.metadata.request.commandPhysical}+256 &&
            pending.metadata.request.commandPhysical<std::uint64_t{root[1]}+44))return false;
    for(unsigned i=0;i<11;++i)if(!readU32(owner->virtualAddress+i*4,child[i]))return false;
    NativeInitialCoherency request;
    if(!parseNativeEmptyCacheCoherency(root,child,request) || request.cacheFlags!=0x01000000 ||
        request.physicalBase!=nativeScalerStorageOwner_->physicalBase || request.byteCount!=nativeScalerStorageOwner_->byteCount)return false;
    const auto childSafe=[&]{
        if(hasDispatcherWaitInRange(owner->virtualAddress,44))return false;
        for(const auto& cs:criticalSections_)if(std::uint64_t{owner->virtualAddress}<std::uint64_t{cs.first}+28 &&
            cs.first<std::uint64_t{owner->virtualAddress}+44)return false;
        return !nativeScalerInterruptPlan_ || !(root[1]<std::uint64_t{nativeScalerInterruptPlan_->scratchPhysical()}+32 &&
            nativeScalerInterruptPlan_->scratchPhysical()<std::uint64_t{root[1]}+44);
    };
    std::uint32_t identifier=0;if(!childSafe() || !readU32(pending.identifierAddress,identifier))return false;
    std::array<std::uint8_t,4> next{};for(unsigned i=0;i<4;++i)next[i]=static_cast<std::uint8_t>(writeWord>>(24-i*8));
    const NativeCommandStorageCompletion completion{previous,writeWord,request};
    deferredSwapStorageAttempted_=true;
    bool ordered=false;
    try{ordered=RECOMP_V_BACKEND()->completeNativeDeferredSwapStorageCoherency(request);}catch(...){return false;}
    if(!ordered)return false;
    std::uint32_t current=0;
    if(!validatePendingDeferredSwapLocked() || !childSafe() || !readU32(pending.identifierAddress,current) || current!=identifier)std::_Exit(4);
    for(unsigned i=0;i<3;++i)if(!readU32(ring.virtualAddress+(previous+i)*4,current) || current!=root[i])std::_Exit(4);
    for(unsigned i=0;i<11;++i)if(!readU32(owner->virtualAddress+i*4,current) || current!=child[i])std::_Exit(4);
    completedDeferredSwapStorage_=completion;
    std::memcpy(addressSpace_->translate(ring.writeBack->virtualAddress),next.data(),4);
    ring.readWord=writeWord;return true; // Ticket remains pending; original receipts are historical.
}
std::optional<GuestImportRegistry::NativePendingSubmission> GuestImportRegistry::inspectStoppedDeferredSwapSubmission(std::uint32_t writeWord) const {
    RECOMP_REGISTRY_LOCK(lock);
    return inspectDeferredSwapSubmissionLocked(writeWord);
}
std::optional<GuestImportRegistry::NativePendingSubmission> GuestImportRegistry::inspectDeferredSwapSubmissionLocked(std::uint32_t writeWord) const {
    if(!completedDeferredSwapStorage_ || !validatePendingDeferredSwapLocked())return std::nullopt;
    const auto& ring=*graphicsRingConfiguration_;const auto& pending=*pendingDeferredSwap_;
    const auto previous=completedDeferredSwapStorage_->readWord;
    if(ring.readWord!=previous || std::uint64_t{previous}+3!=writeWord || writeWord>=ring.bytes/4)return std::nullopt;
    std::array<std::uint32_t,3> root{};
    for(unsigned i=0;i<3;++i)if(!readU32(ring.virtualAddress+(previous+i)*4,root[i]))return std::nullopt;
    // Diagnostic bounds only: no packet framing, rendering receipt, consumer,
    // GPU submission, or permission to skip the native envelope is implied.
    if(root[0]!=0xC0013F00 || (root[1]&3) || root[2]<64 || root[2]>8*1024*1024/4 ||
        std::uint64_t{root[1]}+std::uint64_t{root[2]}*4>0x20000000)return std::nullopt;
    const auto bytes=root[2]*4,command=pending.metadata.request.commandPhysical;
    const auto owner=resolvePhysicalRange(root[1],bytes,false);
    if(!owner || owner->allocationBase!=pending.metadata.request.allocationBase || root[1]>command ||
        std::uint64_t{command}+256>std::uint64_t{root[1]}+bytes || hasDispatcherWaitInRange(owner->virtualAddress,bytes))return std::nullopt;
    for(const auto& cs:criticalSections_)if(std::uint64_t{owner->virtualAddress}<std::uint64_t{cs.first}+28 &&
        cs.first<std::uint64_t{owner->virtualAddress}+bytes)return std::nullopt;
    return NativePendingSubmission{ring.physicalAddress+previous*4,root[1],root[2],(command-root[1])/4,previous,writeWord};
}
std::optional<GuestImportRegistry::NativeDeferredColorPrefixCompletion> GuestImportRegistry::completedNativeDeferredColorPrefix() const {
    RECOMP_REGISTRY_LOCK(lock);return completedDeferredColorPrefix_;
}
std::array<std::uint32_t,3> GuestImportRegistry::nativeDeferredColorPrefixStatus() const {
    RECOMP_REGISTRY_LOCK(lock);return {deferredColorPrefixAttempted_,deferredColorPrefixRejection_,deferredColorPrefixDetail_};
}
bool GuestImportRegistry::executeExperimentalDeferredColorPrefix(const recomp::gpu::NativeColorChainProgram& program,
        const recomp::gpu::DrawBinState& initial,std::uint32_t writeWord){
    using namespace recomp::gpu;
    RECOMP_REGISTRY_LOCK(lock);
    const auto reject=[&](unsigned reason,unsigned detail=0) noexcept {
        deferredColorPrefixRejection_=reason;deferredColorPrefixDetail_=detail;return false;
    };
    if(deferredColorPrefixAttempted_ || completedDeferredColorPrefix_ || pendingDeferredColorImage_)return false;
    if(!experimentalOwnedColorBound_ || RECOMP_REGISTRY_CALLER_TOKEN!=nativeSystemContextThread_)return reject(1);
    const auto* thread=findThreadObject(RECOMP_REGISTRY_CALLER_TOKEN);
    if(!thread || thread->returned || thread->suspendCount)return reject(1);
    const auto submission=inspectDeferredSwapSubmissionLocked(writeWord);
    if(!submission || submission->childWords>NativeDeferredSwapSubmission::MaximumWords)return reject(2);
    noteNativeSubmissionMayCommitAt(9); // v183 site 9: may publish completion: the renderer's pending reads first
    try{
        const auto& ring=*graphicsRingConfiguration_;
        const auto& pending=*pendingDeferredSwap_;
        const auto child=resolvePhysicalRange(submission->childPhysical,submission->childWords*4,false);
        if(!child)return reject(3);
        const auto frame=NativeDeferredSwapSubmission::parse(pending.command,submission->childPhysical,
            addressSpace_->translate(child->virtualAddress),submission->childWords*4);
        if(!frame)return reject(4);
        const Pm4PhysicalReader reader=[&](auto address,auto bytes)->std::optional<std::vector<std::uint8_t>>{
            if(!validNativePhysicalRange({address,bytes}) || bytes>2*1024*1024)return std::nullopt;
            const auto owner=resolvePhysicalRange(address,bytes,false);if(!owner)return std::nullopt;
            const auto* data=addressSpace_->translate(owner->virtualAddress);return std::vector<std::uint8_t>(data,data+bytes);
        };
        unsigned detail=0;
        auto capture=NativeDeferredColorCapture::create(*frame,initial,program,reader,&detail);
        if(!capture)return reject(5,detail);
        const auto idPhysical=physicalAddressOffset_+(pending.identifierAddress-physicalArenaBase_);
        NativeColorMemoryControls controls{{ring.physicalAddress,ring.bytes},{submission->childPhysical,submission->childWords*4},
            {ring.writeBack->physicalAddress,4},{idPhysical,4},submission->readWord,writeWord,
            {{0xC0013F00,submission->childPhysical,submission->childWords}}};
        const auto plan=NativeDeferredColorMemoryPlan::create(*capture,controls,
            {pendingPostOutputColorImage_->physicalBase,NativeSwapImagePlan::Capacity});
        if(!plan)return reject(6);
        const auto resolve=[&](NativePhysicalRange range,bool write)->NativeColorMappedRange{
            const auto owner=resolvePhysicalRange(range.address,range.bytes,write);if(!owner)return {};
            const auto allocation=physicalAllocations_.find(owner->allocationBase);if(allocation==physicalAllocations_.end())return {};
            return {range,{physicalAddressOffset_+owner->allocationBase-physicalArenaBase_,allocation->second.size},
                addressSpace_->translate(owner->allocationBase),(allocation->second.protection&4)!=0};
        };
        NativeDeferredColorResolvedMemory memory;
        for(unsigned i=0;i<plan->inputCount();++i)memory.inputs[i]=resolve(plan->inputs()[i],false);
        for(unsigned i=0;i<5;++i)memory.protectedRanges[i]=resolve(plan->protectedRanges()[i],i==2);
        for(unsigned i=0;i<7;++i)memory.writes[i]=resolve(plan->writes()[i],true);
        if(!matchesNativeDeferredColorMemory(*capture,*plan,memory))return reject(7);
        const auto continuity=[&]() noexcept {
            if(!validatePendingDeferredSwapLocked() || ring.readWord!=controls.previousWord)return reject(8);
            unsigned base=0,mask=0;
            if(!nativeScalerInterruptPlan_ || !graphicsBackend_->queryNativeScratchWriteback(base,mask) ||
                !frame->swap().scratchStateMatches(base,mask) ||
                frame->swap().scratchPhysical()!=nativeScalerInterruptPlan_->scratchPhysical())return reject(9);
            const auto current=[&](const NativeColorMappedRange& original,bool write,bool scratch=false){
                const auto now=resolve(original.range,write);
                if(!color_memory_detail::same(now.allocation,original.allocation) || now.allocationBytes!=original.allocationBytes ||
                    now.writable!=original.writable)return reject(10,original.range.address);
                const auto virtualAddress=physicalArenaBase_+original.range.address-physicalAddressOffset_;
                if(hasDispatcherWaitInRange(virtualAddress,original.range.bytes))return reject(11,original.range.address);
                for(const auto& cs:criticalSections_)if(std::uint64_t{virtualAddress}<std::uint64_t{cs.first}+28 &&
                    cs.first<std::uint64_t{virtualAddress}+original.range.bytes)return reject(12,original.range.address);
                if(!scratch && nativePhysicalOverlap(original.range,{base,32}))return reject(13,original.range.address);
                return true;
            };
            for(unsigned i=0;i<plan->inputCount();++i)if(!current(memory.inputs[i],false))return false;
            for(unsigned i=0;i<5;++i)if(!current(memory.protectedRanges[i],i==2))return false;
            for(unsigned i=0;i<7;++i)if(!current(memory.writes[i],true,i==5))return false;
            for(unsigned i=1;i<5;++i)if(memory.writes[i].allocation.address!=
                physicalAddressOffset_+*graphicsIdentifierAllocation_-physicalArenaBase_)return reject(14,memory.writes[i].range.address);
            return true;
        };
        if(!continuity())return false;
        auto guard=NativeDeferredColorWriteGuard::create(*capture,*plan,memory);
        if(!guard)return reject(15);
        // Allocate the whole-request receipt BEFORE native work. The registry
        // lock pins all owners throughout; explicit pins survive this call for
        // the still-unretired tail. A failed native attempt is terminal too.
        auto savedSubmission=std::make_shared<const NativeDeferredSwapSubmission>(*frame);
        std::array<std::uint32_t,60> pins{};unsigned pinCount=0;
        const auto pin=[&](const NativeColorMappedRange& range){
            const auto owner=physicalArenaBase_+range.allocation.address-physicalAddressOffset_;
            if(std::find(pins.begin(),pins.begin()+pinCount,owner)==pins.begin()+pinCount)pins[pinCount++]=owner;
        };
        for(unsigned i=0;i<plan->inputCount();++i)pin(memory.inputs[i]);
        for(const auto& range:memory.protectedRanges)pin(range);
        for(const auto& range:memory.writes)pin(range);
        const auto outputOwner=resolvePhysicalRange(memory.writes[0].range.address,memory.writes[0].range.bytes,true);
        if(!outputOwner)return reject(16);
        const auto allocation=physicalAllocations_.find(outputOwner->allocationBase);
        if(allocation==physicalAllocations_.end())return reject(16);
        struct Scope {const NativeDeferredColorCapture& capture;const NativeDeferredColorResolvedMemory& memory;
            NativeDeferredColorWriteGuard& guard;const decltype(continuity)& intact;
            unsigned virtualBase,allocationBase,protection;bool published=false;
            std::optional<NativeOwnedColorImage> image;};
        Scope scope{*capture,memory,*guard,continuity,outputOwner->virtualAddress,outputOwner->allocationBase,allocation->second.protection,false,{}};
        const NativeDeferredColorPublisher publisher{&scope,[](void* context,const NativeDeferredColorExecutionResult& result) noexcept {
            auto& s=*static_cast<Scope*>(context);
            if(s.published)std::_Exit(4);
            if(!s.intact() || result.committed || !result.submission || !result.submission->sameRequest(s.capture.submission()) ||
                !result.copy || !result.edram || !result.pendingRegisters || !result.pendingState ||
                result.nativeFences!=4 || result.leadingSetupFences!=1 || !result.auditedPrefixTimeline ||
                !result.auditedPrefixTimeline->complete() || result.edram->knownBytes().size()!=NativeColorEdramSnapshot::Bytes ||
                !result.copy->matchesPlan(s.capture.copy().copy) || !s.guard.matches(s.capture,s.memory))return false;
            try{s.image.emplace(NativeOwnedColorImage{s.memory.writes[0].range.address,s.virtualBase,s.allocationBase,
                s.memory.writes[0].allocation.bytes,s.protection,s.capture.submission().swap().command().identity().previousReadWord+3,*result.copy});}
            catch(...){return false;}
            s.published=s.guard.publishCopy(s.capture,s.memory,*result.copy);return s.published;
        }};
        const NativeDeferredColorPrefixCompletion completion{controls.previousWord,writeWord,frame->swapOffsetWords(),plan->inputCount(),
            capture->copy().copy.pixelBytes,4,1};
        deferredColorPinnedAllocations_=pins;deferredColorPinCount_=pinCount;
        deferredColorSubmission_=std::move(savedSubmission);deferredColorPrefixAttempted_=true;
        (void)reject(17);
        bool completed=false;
        try{completed=RECOMP_V_BACKEND()->executeExperimentalDeferredColorPrefix(program,*capture,publisher);}
        catch(...){if(scope.published)std::_Exit(4);return false;}
        if(scope.published && !completed)std::_Exit(4);
        if(!completed || !scope.published)return false;
        static_assert(std::is_nothrow_move_assignable_v<decltype(pendingDeferredColorImage_)>);
        pendingDeferredColorImage_=std::move(scope.image);completedDeferredColorPrefix_=completion;
        deferredColorPrefixRejection_=0;deferredColorPrefixDetail_=0;
        return true; // PREFIX ONLY. No event/IRQ/RPTR/presentation or old-history mutation.
    }catch(...){return reject(18);}
}
std::optional<GuestImportRegistry::NativeCommandStorageCompletion> GuestImportRegistry::completedNativePostOutputSwapStorageCoherency() const {
    RECOMP_REGISTRY_LOCK(lock);return completedPostOutputSwapStorage_;
}
bool GuestImportRegistry::submitNativeSwapStorageLocked(std::uint32_t writeWord,bool postOutput){
    auto& receipt=postOutput?completedPostOutputSwapStorage_:completedCommandStorageCoherency_;
    if(receipt || (postOutput && postOutputSwapStorageAttempted_) || !nativeDisplayModeStatus_.selected || nativeDisplayModeStatus_.queries!=1 ||
        !validatePendingNativeSwapLocked(postOutput))return false;
    auto& pending=postOutput?*pendingPostOutputSwap_:*pendingNativeSwap_;
    auto& status=postOutput?postOutputSwapStatus_:nativeSwapStatus_;
    auto& ring=*graphicsRingConfiguration_;const auto previous=ring.readWord;
    if(previous!=pending.command.identity().previousReadWord || std::uint64_t{previous}+3!=writeWord || writeWord>=ring.bytes/4)return false;
    noteNativeSubmissionMayCommitAt(10); // v183 site 10: may publish completion: the renderer's pending reads first
    std::vector<std::uint32_t> root(3),child(11);
    for(unsigned i=0;i<3;++i)if(!readU32(ring.virtualAddress+(previous+i)*4,root[i]))return false;
    if(root[0]!=0xC0013F00 || root[2]!=11 || (root[1]&3) || std::uint64_t{root[1]}+44>0x20000000)return false;
    const auto owner=resolvePhysicalRange(root[1],44,false);
    if(!owner || owner->allocationBase!=pending.pinnedAllocations[0])return false;
    const auto overlaps=[](std::uint32_t a,std::uint32_t n,std::uint32_t b,std::uint32_t m){
        return std::uint64_t{a}<std::uint64_t{b}+m && std::uint64_t{b}<std::uint64_t{a}+n;
    };
    if(overlaps(root[1],44,pending.command.identity().commandPhysical,recomp::gpu::NativeSwapCommand::Bytes))return false;
    for(unsigned i=0;i<11;++i)if(!readU32(owner->virtualAddress+i*4,child[i]))return false;
    NativeInitialCoherency request;
    const auto base=physicalAddressOffset_+(pending.pinnedAllocations[0]-physicalArenaBase_);
    if(!parseNativeEmptyCacheCoherency(root,child,request) || request.cacheFlags!=0x01000000 ||
        request.physicalBase!=base || request.byteCount!=pending.image.request.allocationBytes)return false;
    // This entire range is the pinned command allocation, not just an arbitrary
    // owned region. It cannot include the image or control allocations.
    for(unsigned i=1;i<pending.pinnedAllocations.size();++i)
        if(pending.pinnedAllocations[i]==pending.pinnedAllocations[0])return false;
    const auto childSafe=[&]{
        if(hasDispatcherWaitInRange(owner->virtualAddress,44))return false;
        for(const auto& cs:criticalSections_)if(overlaps(owner->virtualAddress,44,cs.first,28))return false;
        return !nativeScalerInterruptPlan_ || !overlaps(root[1],44,nativeScalerInterruptPlan_->scratchPhysical(),32);
    };
    std::uint32_t identifierBefore=0;
    if(postOutput && (!childSafe() || !readU32(pending.identifierAddress,identifierBefore)))return false;
    std::array<std::uint8_t,4> next{};for(unsigned i=0;i<4;++i)next[i]=static_cast<std::uint8_t>(writeWord>>(24-i*8));
    std::optional<NativeCommandStorageCompletion> completion{NativeCommandStorageCompletion{previous,writeWord,request}};
    static_assert(std::is_nothrow_move_assignable_v<decltype(completion)>);
    bool ordered=false;
    if(postOutput)postOutputSwapStorageAttempted_=true;
    try {ordered=postOutput?RECOMP_V_BACKEND()->completeNativePostOutputSwapStorageCoherency(request):
        RECOMP_V_BACKEND()->completeNativeCommandStorageCoherency(request);}catch(...){return false;}
    if(!ordered)return false;
    if(!validatePendingNativeSwapLocked(postOutput))std::_Exit(4);
    std::uint32_t current=0;
    if(postOutput && (!childSafe() || !readU32(pending.identifierAddress,current) || current!=identifierBefore))std::_Exit(4);
    for(unsigned i=0;i<3;++i)if(!readU32(ring.virtualAddress+(previous+i)*4,current) || current!=root[i])std::_Exit(4);
    for(unsigned i=0;i<11;++i)if(!readU32(owner->virtualAddress+i*4,current) || current!=child[i])std::_Exit(4);
    // All fallible work before this RPTR-last commit. Preserve the immutable
    // issuance cursor in the256byte native ticket; only ordered progress moves.
    receipt=std::move(completion);
    std::memcpy(addressSpace_->translate(ring.writeBack->virtualAddress),next.data(),next.size());
    ring.readWord=writeWord;pending.ring.readWord=writeWord;
    status.previousWord=writeWord;status.readWord=writeWord;return true;
}
bool GuestImportRegistry::submitExperimentalOwnedColorBatch(const recomp::gpu::NativeColorChainProgram& program,
        const recomp::gpu::DrawBinState& initial,std::uint32_t writeWord) {
    using namespace recomp::gpu;
    RECOMP_REGISTRY_LOCK(lock);
    if(!experimentalOwnedColorBound_ || completedOwnedColor_ || !retiredGraphicsRingReadWord_)return false;
    noteNativeSubmissionMayCommitAt(11); // v183 site 11: may publish completion: the renderer's pending reads first
    return visitNativeColorBatchLocked(program,initial,writeWord,[&](auto&& capture,const auto& memory){
        const auto ownerBase=physicalArenaBase_+memory.outputs[0].allocation.address-physicalAddressOffset_;
        const auto owner=physicalAllocations_.find(ownerBase);
        if(owner==physicalAllocations_.end())return false;
        struct Scope {const NativeColorBatchCapture& capture;const NativeColorResolvedMemory& memory;
            std::uint32_t ownerBase,protection,virtualBase;
            NativeOwnedColorCompletion receipt{};bool published=false;
            std::optional<NativeOwnedColorImage> image;};
        Scope scope{capture,memory,ownerBase,owner->second.protection,
            physicalArenaBase_+memory.outputs[0].range.address-physicalAddressOffset_,{},false,{}};
        NativeColorPublisher publisher{&scope,[](void* context,const NativeColorChainExecutionResult& result) noexcept{
            auto& s=*static_cast<Scope*>(context);
            if(s.published || !result.copy || !result.edram || !result.auditedTimeline ||
                !result.auditedTimeline->complete() || result.nativeFences!=4 ||
                result.edram->knownBytes().size()!=NativeColorEdramSnapshot::Bytes)return false;
            const auto& copy=s.capture.copy().copy;
            s.receipt={s.capture.controls().previousWord,s.capture.controls().nextWord,4,2,
                copy.pixelBytes,NativeColorEdramSnapshot::Bytes,4,1}; // pixelBytes is the TOTAL active copy size.
            // Allocate the continuity snapshot BEFORE any guest publication.
            // The later commit is a nonthrowing move; it never re-reads GPU RAM.
            try {s.image.emplace(NativeOwnedColorImage{s.memory.outputs[0].range.address,s.virtualBase,
                s.ownerBase,s.memory.outputs[0].allocation.bytes,s.protection,s.receipt.readWord,*result.copy});}
            catch(...){return false;}
            s.published=publishCapturedNativeColorOutputs(s.capture,s.memory,*result.copy,result.eventBytes.data(),result.eventBytes.size());
            return s.published;
        }};
        bool completed=false;
        try {completed=RECOMP_V_BACKEND()->executeExperimentalOwnedColorBatch(program,capture,publisher);}
        catch(...){if(scope.published)std::_Exit(4);return false;}
        // Fail-stop a broken backend contract; never resume guests after a
        // partially acknowledged publication. The real backend has no fallible
        // work after publisher true. A true result without publication rejects.
        if(scope.published && !completed)std::_Exit(4);
        if(!completed || !scope.published)return false;
        static_assert(std::is_nothrow_move_assignable_v<decltype(completedOwnedColorImage_)>);
        completedOwnedColorImage_=std::move(scope.image);
        completedOwnedColor_=scope.receipt;
        graphicsRingConfiguration_->readWord=writeWord;
        return true;
    });
}
std::optional<GuestImportRegistry::NativePostOutputColorCompletion> GuestImportRegistry::completedNativePostOutputColorBatch() const {
    RECOMP_REGISTRY_LOCK(lock);return completedPostOutputColor_;
}
std::array<std::uint32_t,3> GuestImportRegistry::nativePostOutputColorStatus() const {
    RECOMP_REGISTRY_LOCK(lock);return {postOutputColorAttempted_,postOutputColorRejection_,postOutputColorDetail_};
}
bool GuestImportRegistry::submitExperimentalPostOutputColorBatch(const recomp::gpu::NativeColorChainProgram& program,
        const recomp::gpu::DrawBinState& initial,std::uint32_t writeWord) {
    using namespace recomp::gpu;
    RECOMP_REGISTRY_LOCK(lock);
    const auto reject=[&](unsigned reason,unsigned detail=0) noexcept {postOutputColorRejection_=reason;postOutputColorDetail_=detail;return false;};
    if(!experimentalOwnedColorBound_ || postOutputColorAttempted_ || completedPostOutputColor_ ||
        pendingPostOutputColorImage_)return false; // Preserve completed/terminal reason on retries.
    // v247 (A/B: files/private-trace/diag-v247-baseline): visitNativeColorBatchLocked(..., postOutput) is const and refuses
    // before its visitor unless the batch is the 3-word submission right after the post-output storage's read word
    // (v245 run tj70amk5: its site 13 was reached twice in ~300 k calls); validateNativeScanoutLocked (const) compares the
    // whole 3.7 MB owned scanout copy with guest memory under R (~125 us per single-IB submission, the top R blocker). The
    // pure ring-word precondition now runs first: the visitor runs exactly when it did before; a submission refused here
    // records reason 3 (common capture refused) even when the scanout would also have failed (it recorded 2).
    if(!(&recomp_v247_baseline && recomp_v247_baseline.load(std::memory_order_relaxed))) {
        v247Prechecks.fetch_add(1,std::memory_order_relaxed);
        const auto* ring=graphicsRingConfiguration_?&*graphicsRingConfiguration_:nullptr;
        const auto previous=completedPostOutputStorage_?completedPostOutputStorage_->readWord:0u;
        if(!ring || !completedPostOutputStorage_ || !ring->writeBack || ring->readWord!=previous ||
           std::uint64_t{previous}+3!=writeWord || writeWord>=ring->bytes/4){v247Refused.fetch_add(1,std::memory_order_relaxed);return reject(3);}
    }
    if(!validateNativeScanoutLocked())return reject(2);
    (void)reject(3); // Common capture refused, unless the visitor is reached.
    return visitNativeColorBatchLocked(program,initial,writeWord,[&](auto&& capture,const auto& memory){
        noteNativeSubmissionMayCommitAt(12); // v183 site 12 (v183a had it after the first guard: every submission passed it)
        const auto& old=*completedOwnedColorImage_;
        // Same image OR a wholly separate owned backbuffer. A new destination
        // cannot alias the old image allocation, any command/control allocation
        // or any captured source allocation. Neither case implies a flip.
        const auto& output=memory.outputs[0];
        const NativePhysicalRange oldAllocation{physicalAddressOffset_+old.allocationBase-physicalArenaBase_,old.allocationBytes};
        const bool sameImage=output.range.address==old.physicalBase && color_memory_detail::same(output.allocation,oldAllocation);
        if(output.range.bytes!=NativeSwapImagePlan::Capacity || (!sameImage && nativePhysicalOverlap(output.allocation,oldAllocation)))
            return reject(4,output.range.address);
        for(unsigned i=0;i<4;++i)if(nativePhysicalOverlap(output.allocation,memory.controls[i].allocation))return reject(4,output.range.address);
        for(unsigned i=0;i<capture.memory().inputCount;++i)
            if(nativePhysicalOverlap(output.allocation,memory.inputs[i].allocation))return reject(4,output.range.address);
        const auto copyOwner=resolvePhysicalRange(output.range.address,output.range.bytes,true);
        if(!copyOwner)return reject(4,output.range.address);
        const auto allocation=physicalAllocations_.find(copyOwner->allocationBase);
        if(allocation==physicalAllocations_.end())return reject(4,output.range.address);
        if(memory.controls[1].allocation.address!=nativeScalerStorageOwner_->physicalBase)return reject(5,memory.controls[1].range.address);
        for(unsigned i=1;i<3;++i)
            if(physicalArenaBase_+memory.outputs[i].allocation.address-physicalAddressOffset_!=*graphicsIdentifierAllocation_)return reject(6,memory.outputs[i].range.address);
        const auto continuity=[&]() noexcept {
            if(!validateNativeScalerStorageLocked())return reject(7);
            if(!validateNativeScanoutLocked())return reject(8);
            if(graphicsRingConfiguration_->readWord!=capture.controls().previousWord)return reject(9);
            const auto current=[&](const NativeColorMappedRange& m,bool write){
                const auto owner=resolvePhysicalRange(m.range.address,m.range.bytes,write);
                if(!owner)return reject(10,m.range.address);
                const auto allocation=physicalAllocations_.find(owner->allocationBase);
                if(allocation==physicalAllocations_.end() || allocation->second.size!=m.allocation.bytes ||
                    physicalAddressOffset_+owner->allocationBase-physicalArenaBase_!=m.allocation.address ||
                    addressSpace_->translate(owner->allocationBase)!=m.allocationBytes ||
                    addressSpace_->translate(owner->virtualAddress)!=m.bytes() ||
                    ((allocation->second.protection&4)!=0)!=m.writable)return reject(10,m.range.address);
                const auto overlaps=[&](unsigned a,unsigned n){return std::uint64_t{a}<std::uint64_t{owner->virtualAddress}+m.range.bytes &&
                    std::uint64_t{owner->virtualAddress}<std::uint64_t{a}+n;};
                for(const auto& cs:criticalSections_)if(overlaps(cs.first,28))return reject(11,m.range.address);
                if(hasDispatcherWaitInRange(owner->virtualAddress,m.range.bytes))return reject(12,m.range.address);
                if(nativeScalerInterruptPlan_ && nativePhysicalOverlap(m.range,{nativeScalerInterruptPlan_->scratchPhysical(),32}))return reject(13,m.range.address);
                return true;
            };
            for(unsigned i=0;i<capture.memory().inputCount;++i)if(!current(memory.inputs[i],false))return false;
            for(unsigned i=0;i<4;++i)if(!current(memory.controls[i],i==2) || !current(memory.outputs[i],true))return false;
            return matchesNativeColorBatchMemory(capture,memory) || reject(14);
        };
        if(!continuity())return false;
        // Includes untouched tile padding. Do not overwrite an independently
        // modified backbuffer, even when all rendering sources still match.
        const std::vector<std::uint8_t> copyBefore(output.bytes(),output.bytes()+output.range.bytes);
        // Preserve event/identifier bytes across native execution too. The old
        // copy snapshot validates every active pixel; padding is not modified.
        std::array<std::uint8_t,12> before{};
        std::memcpy(before.data(),memory.outputs[1].bytes(),4);
        std::memcpy(before.data()+4,memory.outputs[2].bytes(),4);
        std::memcpy(before.data()+8,memory.controls[3].bytes(),4);
        struct Scope {const NativeColorBatchCapture& capture;const NativeColorResolvedMemory& memory;
            const decltype(continuity)& intact;const std::array<std::uint8_t,12>& before;
            const std::vector<std::uint8_t>& copyBefore;
            std::uint32_t virtualBase,allocationBase,protection;bool published=false;
            std::optional<NativeOwnedColorImage> image;NativePostOutputColorCompletion receipt{};};
        Scope scope{capture,memory,continuity,before,copyBefore,copyOwner->virtualAddress,copyOwner->allocationBase,
            allocation->second.protection,false,{},{}};
        NativeColorPublisher publisher{&scope,[](void* context,const NativeColorChainExecutionResult& result) noexcept {
            auto& s=*static_cast<Scope*>(context);
            if(s.published)std::_Exit(4);
            if(!s.intact() || !result.copy || !result.edram || !result.auditedTimeline || !result.auditedTimeline->complete() ||
                result.nativeFences!=4 || result.leadingSetupFences!=1 || result.edram->knownBytes().size()!=NativeColorEdramSnapshot::Bytes ||
                std::memcmp(s.copyBefore.data(),s.memory.outputs[0].bytes(),s.copyBefore.size()) ||
                std::memcmp(s.before.data(),s.memory.outputs[1].bytes(),4) || std::memcmp(s.before.data()+4,s.memory.outputs[2].bytes(),4) ||
                std::memcmp(s.before.data()+8,s.memory.controls[3].bytes(),4))return false;
            s.receipt={{s.capture.controls().previousWord,s.capture.controls().nextWord,4,2,
                s.capture.copy().copy.pixelBytes,NativeColorEdramSnapshot::Bytes,4,1},1};
            try{s.image.emplace(NativeOwnedColorImage{s.memory.outputs[0].range.address,s.virtualBase,s.allocationBase,
                s.memory.outputs[0].allocation.bytes,s.protection,s.receipt.color.readWord,*result.copy});}catch(...){return false;}
            s.published=publishCapturedNativeColorOutputs(s.capture,s.memory,*result.copy,result.eventBytes.data(),result.eventBytes.size());
            return s.published;
        }};
        postOutputColorAttempted_=true; // Partial native execution is terminal.
        (void)reject(15); // Backend/result failure unless a more exact late check records why.
        bool completed=false;
        try{completed=RECOMP_V_BACKEND()->executeExperimentalPostOutputColorBatch(program,capture,publisher);}
        catch(...){if(scope.published)std::_Exit(4);return false;}
        if(scope.published && !completed)std::_Exit(4);
        if(!completed || !scope.published)return false;
        static_assert(std::is_nothrow_move_assignable_v<decltype(pendingPostOutputColorImage_)>);
        pendingPostOutputColorImage_=std::move(scope.image);
        completedPostOutputColor_=scope.receipt;
        graphicsRingConfiguration_->readWord=writeWord;
        postOutputColorRejection_=0;postOutputColorDetail_=0;
        return true; // Old displayed image/receipts remain; no image callback.
    },true);
}
bool GuestImportRegistry::runStoppedNativeColorDiagnostic(const recomp::gpu::NativeColorChainProgram& program,
        const recomp::gpu::DrawBinState& initial,std::uint32_t writeWord,
        const std::function<bool(const recomp::gpu::NativeColorBatchCapture&)>& backend) const {
    if(!backend)return false;
    return visitStoppedNativeColorBatch(program,initial,writeWord,[&](auto&& capture){return backend(capture);});
}
bool GuestImportRegistry::inspectStoppedNativeColorBatch(const recomp::gpu::NativeColorChainProgram& program,
        const recomp::gpu::DrawBinState& initial,std::uint32_t writeWord,
        const std::function<bool(const recomp::gpu::NativeColorBatchCapture&)>& inspector) const {
    if(!inspector)return false;
    return visitStoppedNativeColorBatch(program,initial,writeWord,[&](auto&& capture){return inspector(capture);});
}
bool GuestImportRegistry::inspectStoppedPostOutputColorBatch(const recomp::gpu::NativeColorChainProgram& program,
        const recomp::gpu::DrawBinState& initial,std::uint32_t writeWord,
        const std::function<bool(const recomp::gpu::NativeColorBatchCapture&)>& inspector) const {
    RECOMP_REGISTRY_LOCK(lock);
    if(!inspector)return false;
    return visitNativeColorBatchLocked(program,initial,writeWord,[&](auto&& capture,const auto&){return inspector(capture);},true);
}
bool GuestImportRegistry::runStoppedPostOutputColorDiagnostic(const recomp::gpu::NativeColorChainProgram& program,
        const recomp::gpu::DrawBinState& initial,std::uint32_t writeWord,
        const std::function<bool(const recomp::gpu::NativeColorBatchCapture&)>& backend) const {
    RECOMP_REGISTRY_LOCK(lock);
    if(!backend)return false;
    return visitNativeColorBatchLocked(program,initial,writeWord,[&](auto&& capture,const auto&){return backend(capture);},true);
}
std::unique_ptr<recomp::gpu::NativeColorBatchCapture> GuestImportRegistry::snapshotStoppedNativeColorBatch(
        const recomp::gpu::NativeColorChainProgram& program,const recomp::gpu::DrawBinState& initial,std::uint32_t writeWord) const {
    std::unique_ptr<recomp::gpu::NativeColorBatchCapture> result;
    if(!visitStoppedNativeColorBatch(program,initial,writeWord,[&](auto&& capture){
        result=std::make_unique<recomp::gpu::NativeColorBatchCapture>(std::move(capture));return true;
    }))return nullptr;
    return result;
}
bool GuestImportRegistry::visitStoppedNativeColorBatch(const recomp::gpu::NativeColorChainProgram& program,
        const recomp::gpu::DrawBinState& initial,std::uint32_t writeWord,
        const std::function<bool(recomp::gpu::NativeColorBatchCapture&&)>& inspector) const {
    RECOMP_REGISTRY_LOCK(lock);
    if(!inspector)return false;
    return visitNativeColorBatchLocked(program,initial,writeWord,[&](auto&& capture,const auto&){return inspector(std::move(capture));});
}
bool GuestImportRegistry::visitNativeColorBatchLocked(const recomp::gpu::NativeColorChainProgram& program,
        const recomp::gpu::DrawBinState& initial,std::uint32_t writeWord,
        const std::function<bool(recomp::gpu::NativeColorBatchCapture&&,const recomp::gpu::NativeColorResolvedMemory&)>& visitor,bool postOutput) const {
    using namespace recomp::gpu;
    const bool warm=retiredGraphicsRingReadWord_.has_value();
    if(postOutput && (!warm || !completedOwnedColor_ || !completedOwnedColorImage_ || !completedDisplayOutput_ ||
        !completedPostOutputStorage_ || !completedPostOutputStorage_->batches || nativePostOutputStorageFailed_ ||
        !validateNativeScalerStorageLocked()))return false;
    if(!visitor || !nativeStateBatchesBound_ || (warm && !completedWarmRingCoherency_) || !completedExperimentalInitialization_ ||
        !addressSpace_ || !graphicsInitialized_ || !graphicsBackend_ || !graphicsBackend_->ready() || !graphicsRingConfiguration_ ||
        !graphicsRingAllocation_ || !graphicsWriteBackAllocation_ || !graphicsSystemIdentifier_)return false;
    const auto& ring=*graphicsRingConfiguration_;
    const auto previous=postOutput?completedPostOutputStorage_->readWord:(warm?std::max({22u,completedWarmStateBatches_?completedWarmStateBatches_->readWord:0,
        completedWarmEmptyCacheCoherency_?completedWarmEmptyCacheCoherency_->readWord:0}):
        std::max({completedExperimentalInitialization_->readWord,completedEmptyCacheCoherency_?completedEmptyCacheCoherency_->readWord:0,
            completedNativeStateBatches_?completedNativeStateBatches_->readWord:0}));
    if(!ring.writeBack || ring.readWord!=previous || std::uint64_t{previous}+3!=writeWord || writeWord>=ring.bytes/4)return false;
    noteNativeSubmissionMayCommitAt(13); // v183 site 13: may publish completion: the renderer's pending reads first
    const auto& wb=*ring.writeBack;
    const auto ownedRing=resolvePhysicalRange(ring.physicalAddress,ring.bytes,false),ownedWb=resolvePhysicalRange(wb.physicalAddress,4,true);
    if(!ownedRing || ownedRing->allocationBase!=*graphicsRingAllocation_ || ownedRing->virtualAddress!=ring.virtualAddress ||
        !ownedWb || ownedWb->allocationBase!=*graphicsWriteBackAllocation_ || ownedWb->virtualAddress!=wb.virtualAddress ||
        *graphicsSystemIdentifier_<physicalArenaBase_ ||
        std::uint64_t{*graphicsSystemIdentifier_}-physicalArenaBase_+4>physicalArenaSize_)return false;
    const auto identifier=std::uint64_t{physicalAddressOffset_}+*graphicsSystemIdentifier_-physicalArenaBase_;
    if(identifier+4>0x20000000)return false;
    try {
        std::uint32_t published=0;if(!readU32(wb.virtualAddress,published) || published!=previous)return false;
        NativeColorMemoryControls controls{{ring.physicalAddress,ring.bytes},{},{wb.physicalAddress,4},
            {static_cast<std::uint32_t>(identifier),4},previous,writeWord,{}};
        for(unsigned i=0;i<3;++i)if(!readU32(ring.virtualAddress+(previous+i)*4,controls.rootWords[i]))return false;
        if(!controls.rootWords[2] || controls.rootWords[2]>4096)return false;
        controls.child={controls.rootWords[1],controls.rootWords[2]*4};
        if(!validNativeColorMemoryControls(controls,controls.rootWords[2]))return false;
        const auto child=resolvePhysicalRange(controls.child.address,controls.child.bytes,false);
        if(!child)return false;
        Pm4Snapshot commands;Pm4Error error;Pm4Limits limits;limits.maxWords=4096;limits.maxPackets=512;
        if(!decodePm4(addressSpace_->translate(child->virtualAddress),controls.child.bytes,limits,commands,error))return false;
        // No nested public snapshot method: its mutex would deadlock and a
        // sequence of separately locked snapshots would not retain ownership.
        const Pm4PhysicalReader reader=[&](auto address,auto bytes)->std::optional<std::vector<std::uint8_t>>{
            if(!validNativePhysicalRange({address,bytes}) || bytes>2*1024*1024)return std::nullopt;
            const auto owner=resolvePhysicalRange(address,bytes,false);if(!owner)return std::nullopt;
            const auto* p=addressSpace_->translate(owner->virtualAddress);return std::vector<std::uint8_t>(p,p+bytes);
        };
        auto capture=NativeColorBatchCapture::create(commands,initial,program,controls,reader,
            postOutput?NativeColorBatchPhase::PostOutput:NativeColorBatchPhase::Startup);if(!capture)return false;
        const auto resolve=[&](NativePhysicalRange range,bool write)->NativeColorMappedRange{
            const auto owner=resolvePhysicalRange(range.address,range.bytes,write);if(!owner)return {};
            const auto allocation=physicalAllocations_.find(owner->allocationBase);if(allocation==physicalAllocations_.end())return {};
            const auto base=std::uint64_t{physicalAddressOffset_}+owner->allocationBase-physicalArenaBase_;
            if(base+allocation->second.size>0x20000000)return {};
            return {range,{static_cast<std::uint32_t>(base),allocation->second.size},addressSpace_->translate(owner->allocationBase),
                (allocation->second.protection&0x4u)!=0}; // Guest PAGE_READWRITE.
        };
        NativeColorResolvedMemory memory;const auto& request=capture->memory();
        for(unsigned i=0;i<request.inputCount;++i)memory.inputs[i]=resolve(request.inputs[i],false);
        for(unsigned i=0;i<4;++i){memory.controls[i]=resolve(request.controls[i],i==2);memory.outputs[i]=resolve(request.outputs[i],true);}
        if(!matchesNativeColorBatchMemory(*capture,memory))return false;
        // The calling wrapper defines whether this is a read-only inspection,
        // stopped GPU diagnostic or separately bound live transaction. This
        // same real lock covers the entire visitor; no borrowed views escape.
        return visitor(std::move(*capture),memory);
    }catch(...){return false;}
}

std::optional<std::uint32_t> GuestImportRegistry::retiredGraphicsRingReadWord() const {
    RECOMP_REGISTRY_LOCK(lock); return retiredGraphicsRingReadWord_;
}
std::optional<NativeInitialCoherency> GuestImportRegistry::completedWarmRingCoherency() const {
    RECOMP_REGISTRY_LOCK(lock); return completedWarmRingCoherency_;
}
bool GuestImportRegistry::bindNativeStateBatches() {
    RECOMP_REGISTRY_LOCK(lock);
    if (!experimentalInitializationBound_ || graphicsInitialized_ || nativeStateBatchesBound_) return false;
    nativeStateBatchesBound_=true; return true;
}
bool GuestImportRegistry::bindExperimentalIdleCachePair() {
    RECOMP_REGISTRY_LOCK(lock);
    if (!nativeStateBatchesBound_ || !emptyCacheCoherencyBound_ || graphicsInitialized_ || experimentalIdleCachePairBound_) return false;
    experimentalIdleCachePairBound_=true; return true;
}
std::uint64_t GuestImportRegistry::experimentalIdleCachePairs() const {
    RECOMP_REGISTRY_LOCK(lock); return experimentalIdleCachePairs_;
}
std::optional<GuestImportRegistry::NativeStateBatchCompletion> GuestImportRegistry::completedNativeStateBatches() const {
    RECOMP_REGISTRY_LOCK(lock); return completedNativeStateBatches_;
}
std::optional<GuestImportRegistry::NativeStateBatchCompletion> GuestImportRegistry::completedWarmStateBatches() const {
    RECOMP_REGISTRY_LOCK(lock); return completedWarmStateBatches_;
}
bool GuestImportRegistry::submitNativeStateBatch(std::uint32_t writeWord) {
    using namespace recomp::gpu;
    RECOMP_REGISTRY_LOCK(lock);
    const bool warm=retiredGraphicsRingReadWord_.has_value();
    if (!nativeStateBatchesBound_ || (warm && !completedWarmRingCoherency_) || !completedExperimentalInitialization_ || !addressSpace_ ||
        !graphicsInitialized_ || !graphicsBackend_ || !graphicsBackend_->ready() || !graphicsRingConfiguration_ ||
        !graphicsRingAllocation_ || !graphicsWriteBackAllocation_) return false;
    auto& ring=*graphicsRingConfiguration_;
    // A state batch may legitimately follow the one-shot post-cold-init render
    // chain (owned color batch, then post-output color batch), not only
    // another state batch or empty-cache-coherency call: both already require
    // a real fenced, audited native execution to record their readWord, so
    // recognizing them here reflects verified prior work, not a fabricated one.
    const auto previous=warm ? std::max({22u,
        completedWarmStateBatches_?completedWarmStateBatches_->readWord:0,
        completedWarmEmptyCacheCoherency_?completedWarmEmptyCacheCoherency_->readWord:0,
        completedOwnedColor_?completedOwnedColor_->readWord:0,
        completedPostOutputColor_?completedPostOutputColor_->color.readWord:0,
        completedWarmSideEffectFreeBatches_?completedWarmSideEffectFreeBatches_->readWord:0}) : std::max({completedExperimentalInitialization_->readWord,
        completedEmptyCacheCoherency_?completedEmptyCacheCoherency_->readWord:0,
        completedNativeStateBatches_?completedNativeStateBatches_->readWord:0,
        completedSideEffectFreeBatches_?completedSideEffectFreeBatches_->readWord:0});
    if (!ring.writeBack || ring.readWord!=previous || std::uint64_t{previous}+3!=writeWord || writeWord>=ring.bytes/4) return false;
    noteNativeSubmissionMayCommitAt(14); // v183 site 14: may publish completion: the renderer's pending reads first
    const auto& wb=*ring.writeBack;
    const auto ownedRing=resolvePhysicalRange(ring.physicalAddress,ring.bytes,false);
    const auto ownedWb=resolvePhysicalRange(wb.physicalAddress,4,true);
    if (!ownedRing || ownedRing->allocationBase!=*graphicsRingAllocation_ || ownedRing->virtualAddress!=ring.virtualAddress ||
        !ownedWb || ownedWb->allocationBase!=*graphicsWriteBackAllocation_ || ownedWb->virtualAddress!=wb.virtualAddress) return false;
    std::uint32_t publishedRead=0;
    if (!readU32(wb.virtualAddress,publishedRead) || publishedRead!=previous) return false;
    try {
        const auto overlaps=[](auto a,auto n,auto b,auto m) {return a<std::uint64_t{b}+m && b<std::uint64_t{a}+n;};
        std::array<std::uint32_t,3> root{};
        for (unsigned i=0;i<3;++i) if (!readU32(ring.virtualAddress+(previous+i)*4,root[i])) return false;
        if (root[0]!=0xC0013F00 || !root[2] || root[2]>256 || (root[1]&3) ||
            std::uint64_t{root[1]}+root[2]*4>0x20000000) return false;
        const auto child=resolvePhysicalRange(root[1],root[2]*4,false);
        if (!child || overlaps(root[1],root[2]*4,ring.physicalAddress,ring.bytes) ||
            overlaps(root[1],root[2]*4,wb.physicalAddress,4) ||
            (graphicsSystemIdentifier_ && overlaps(child->virtualAddress,root[2]*4,*graphicsSystemIdentifier_,4))) return false;
        std::vector<std::uint32_t> words(root[2]);
        for (unsigned i=0;i<root[2];++i) if (!readU32(child->virtualAddress+i*4,words[i])) return false;
        NativeStateBatch plan; Pm4Error error;
        if (!planNativeStateBatch(words,plan,error,experimentalIdleCachePairBound_)) return false;
        if (plan.experimentalIdleCachePair && experimentalIdleCachePairs_==UINT64_MAX) return false;
        auto& completion=warm?completedWarmStateBatches_:completedNativeStateBatches_;
        auto state=completion.value_or(NativeStateBatchCompletion{});
        if (state.batches==UINT32_MAX || state.words>UINT32_MAX-plan.updates.count ||
            state.stateWrites>UINT32_MAX-plan.stateWrites || state.invalidations>UINT32_MAX-plan.invalidations) return false;
        ++state.batches; state.readWord=writeWord; state.words+=static_cast<std::uint32_t>(plan.updates.count);
        state.stateWrites+=plan.stateWrites; state.invalidations+=plan.invalidations;
        std::array<NativeOutputWord,64> destinations{};
        for (std::size_t i=0;i<plan.updates.count;++i) {
            const auto physical=plan.destinations[i];
            const auto owned=resolvePhysicalRange(physical,4,true);
            if (!owned || overlaps(physical,4,ring.physicalAddress,ring.bytes) || overlaps(physical,4,wb.physicalAddress,4) ||
                overlaps(physical,4,root[1],root[2]*4) ||
                (graphicsSystemIdentifier_ && owned->virtualAddress==*graphicsSystemIdentifier_)) return false;
            destinations[i]={static_cast<std::uint32_t>(i*4),{physical,addressSpace_->translate(owned->virtualAddress),4}};
        }
        NativeInitializationReadback readback;
        const bool completed=plan.experimentalIdleCachePair
            ? RECOMP_V_BACKEND()->completeExperimentalIdleCachePair(words,readback)
            : RECOMP_V_BACKEND()->completeNativeStateBatch(words,readback);
        if (!completed || readback.draws ||
            readback.byteCount!=plan.updates.bufferBytes) return false;
        if (!publishNativeGpuOutputs(plan.updates,readback.bytes.data(),readback.byteCount,
                destinations.data(),plan.updates.count,{wb.physicalAddress,addressSpace_->translate(wb.virtualAddress),4},
                previous,writeWord)) return false;
        // Only non-throwing, private bookkeeping remains after guest publication.
        completion=state;
        if (plan.experimentalIdleCachePair) ++experimentalIdleCachePairs_;
        ring.readWord=writeWord;
        return true;
    } catch (...) { return false; }
}

std::optional<GuestImportRegistry::NativeRuntimeMaintenanceCompletion>
GuestImportRegistry::completedNativeRuntimeMaintenancePoints() const {
    RECOMP_REGISTRY_LOCK(lock);return completedRuntimeMaintenance_;
}
GuestImportRegistry::NativeRuntimeMaintenanceDiagnostic
GuestImportRegistry::nativeRuntimeMaintenanceDiagnostic() const {
    RECOMP_REGISTRY_LOCK(lock);return runtimeMaintenanceDiagnostic_;
}
bool GuestImportRegistry::submitNativeRuntimeMaintenancePoints(std::uint32_t device,std::uint32_t writeWord,
        const NativeSerializedGate& gate,const NativeRuntimeMaintenanceExecutor& execute) {
    using namespace recomp::gpu;
    if(!gate || !execute)return false;
    bool entered=false,reentered=false,published=false,completed=false;
    // Reporting only. `reject` records the FIRST admission condition that stopped
    // this attempt; no condition below is added, removed, reordered or relaxed.
    NativeRuntimeMaintenanceDiagnostic diagnostic;
    unsigned rejection=0;
    const auto reject=[&](unsigned reason){if(!rejection)rejection=reason;};
    try {
        const bool gated=gate([&] {
            if(entered){reentered=true;return;}entered=true;
            RECOMP_REGISTRY_LOCK(lock);
            diagnostic=runtimeMaintenanceDiagnostic_;++diagnostic.attempts;
            diagnostic.rejection=0;diagnostic.reachedExecute=0;
            // Destroyed before the lock guard, so every early return publishes.
            struct Recorder {
                NativeRuntimeMaintenanceDiagnostic& out;const NativeRuntimeMaintenanceDiagnostic& current;const unsigned& reason;
                ~Recorder(){out=current;out.rejection=reason;}
            } recorder{runtimeMaintenanceDiagnostic_,diagnostic,rejection};
            diagnostic.failedLatch=nativeRuntimeMaintenanceFailed_?1u:0u;
            if(nativeRuntimeMaintenanceFailed_){reject(2);return;}
            if(!addressSpace_ || !graphicsInitialized_ ||
               !graphicsBackend_ || !graphicsBackend_->ready() || !graphicsRingConfiguration_ ||
               !graphicsRingAllocation_ || !graphicsWriteBackAllocation_ || !nativeInterruptDeviceRoot_ ||
               !nativeTileSceneWorker_ || nativeTileSceneInterruptScope_ || nativeInterruptScope_ || nativeTileLockScope_ ||
               pendingNativeSwap_ || pendingPostOutputSwap_ || pendingDeferredSwap_){reject(3);return;}
            const auto worker=*nativeTileSceneWorker_;const auto issuerToken=RECOMP_REGISTRY_CALLER_TOKEN;
            const auto issuerReady=[&] {
                const auto* issuer=findThreadObject(issuerToken);const auto* returned=findThreadObject(worker.token);
                const auto& irq=nativeTileSceneInterruptStatus_;
                return RECOMP_REGISTRY_CALLER_TOKEN==issuerToken && issuer && !issuer->returned && !issuer->suspendCount &&
                    issuerToken!=worker.token && returned && returned->returned && worker.device==device &&
                    irq.attempted && irq.returned && irq.balanced && !irq.failed && !irq.held &&
                    irq.lockPairs==1 && irq.eventSignals==1 && irq.worker==worker.token && irq.device==device &&
                    retiredNativeTileInitialWorker_==worker.token && retiredNativeTileFirstWorker_==worker.token &&
                    retiredNativeTileFinalWorker_==worker.token;
            };
            if(!issuerReady()){reject(4);return;}
            if(!nativeInterruptDeviceRoot_(device) || device<heapBase_ ||
               std::uint64_t{device}+21944>std::uint64_t{heapBase_}+heapSize_ || !validGuestDataBuffer(device,21944,false)){reject(5);return;}
            const auto original=*graphicsRingConfiguration_;
            auto& ring=*graphicsRingConfiguration_;const auto previous=original.readWord;
            if(!original.writeBack || std::uint64_t{previous}+3!=writeWord || writeWord>=original.bytes/4){reject(6);return;}
            const auto wb=*original.writeBack;
            const auto overlap=[](auto a,auto n,auto b,auto m) {
                return std::uint64_t{a}<std::uint64_t{b}+m && std::uint64_t{b}<std::uint64_t{a}+n;
            };
            const auto ownedRing=resolvePhysicalRange(original.physicalAddress,original.bytes,false);
            const auto ownedRead=resolvePhysicalRange(wb.physicalAddress,4,true);
            diagnostic.ringAllocation=ownedRing?ownedRing->allocationBase:0u;
            diagnostic.writeBackAllocation=ownedRead?ownedRead->allocationBase:0u;
            if(!ownedRing || ownedRing->allocationBase!=*graphicsRingAllocation_ || ownedRing->virtualAddress!=original.virtualAddress ||
               !ownedRead || ownedRead->allocationBase!=*graphicsWriteBackAllocation_ || ownedRead->virtualAddress!=wb.virtualAddress ||
               overlap(original.physicalAddress,original.bytes,wb.physicalAddress,4)){reject(7);return;}
            const auto* stateBytes=addressSpace_->translate(device);
            const std::vector<std::uint8_t> state(stateBytes,stateBytes+21944);
            const auto stateWord=[&](unsigned at) {
                return (unsigned(state[at])<<24)|(unsigned(state[at+1])<<16)|(unsigned(state[at+2])<<8)|state[at+3];
            };
            if(stateWord(15300)!=original.virtualAddress || std::uint64_t{stateWord(15304)}+1!=original.bytes/4 ||
               stateWord(11088)!=writeWord || std::uint64_t{stateWord(11024)}+60!=wb.virtualAddress){reject(8);return;}
            unsigned read=0;if(!readU32(wb.virtualAddress,read) || read!=previous){reject(9);return;}
            struct Lease {unsigned physical;PhysicalRange owner;std::vector<std::uint8_t> bytes;};
            std::vector<Lease> leases;leases.reserve(4);
            Pm4Snapshot root;Pm4Error decode;
            if(!decodePm4Ring(addressSpace_->translate(original.virtualAddress),original.bytes,previous,writeWord,{3,1},root,decode)){reject(10);return;}
            const auto* rootBytes=addressSpace_->translate(original.virtualAddress+previous*4);
            const std::array<std::uint8_t,12> rootBefore=[&] {std::array<std::uint8_t,12> b{};std::copy_n(rootBytes,12,b.begin());return b;}();
            Pm4IndirectGraph graph;Pm4GraphError graphError;
            if(!inspectPm4IndirectGraph(root,[&](unsigned physical,unsigned bytes)->std::optional<std::vector<std::uint8_t>> {
                const auto owner=resolvePhysicalRange(physical,bytes,false);
                if(diagnostic.leaseCount<diagnostic.leasePhysical.size()) {
                    diagnostic.leasePhysical[diagnostic.leaseCount]=physical;
                    diagnostic.leaseAllocation[diagnostic.leaseCount]=owner?owner->allocationBase:0u;
                    ++diagnostic.leaseCount;
                }
                if(!owner){reject(12);return std::nullopt;}
                // The title describes one command-memory region: the ring extent
                // lives at device+15300/+15304 and the command-buffer pool bounds
                // at device+15312/+15320/+15324, and it suballocates indirect
                // buffers from that region alongside the ring. Sharing a backing
                // allocation is therefore normal and is not itself an ownership
                // defect. What must never happen is a lease aliasing the command
                // storage this transaction is mutating, so reject actual overlap
                // of the ring's live extent or the read-pointer writeback word
                // rather than mere co-residency in one allocation. Lease/lease,
                // device-state, system-identifier and fence overlaps are still
                // rejected by the checks that follow.
                if(overlap(owner->virtualAddress,bytes,original.virtualAddress,original.bytes) ||
                   overlap(owner->virtualAddress,bytes,wb.virtualAddress,4)){reject(13);return std::nullopt;}
                if(overlap(owner->virtualAddress,bytes,device,state.size()) ||
                   (graphicsSystemIdentifier_ && overlap(owner->virtualAddress,bytes,*graphicsSystemIdentifier_,4))){reject(14);return std::nullopt;}
                for(const auto& l:leases)if(overlap(physical,bytes,l.physical,l.bytes.size())){reject(15);return std::nullopt;}
                const auto* data=addressSpace_->translate(owner->virtualAddress);
                leases.push_back({physical,*owner,{data,data+bytes}});return leases.back().bytes;
            },graph,graphError,{4,2,32768,4096})){reject(11);return;}
            MaintenancePointPlan plan;MaintenancePointError error;
            if(!planRuntimeMaintenancePoints(root,graph,plan,error) || !plan.runtimeTail || plan.events.size()!=2){reject(16);return;}
            const auto& tail=*plan.runtimeTail;
            const auto fence=resolvePhysicalRange(tail.fencePhysical(),8,true);
            const auto cursorPhysical=(tail.commandCursor()&~3u)-0xA0000000;
            const auto cursor=resolvePhysicalRange(cursorPhysical,4,false);
            diagnostic.fencePhysical=tail.fencePhysical();
            diagnostic.fenceAllocation=fence?fence->allocationBase:0u;
            diagnostic.cursorPhysical=cursorPhysical;
            diagnostic.cursorAllocation=cursor?cursor->allocationBase:0u;
            if(!fence || fence->allocationBase!=*graphicsWriteBackAllocation_ ||
               std::uint64_t{fence->virtualAddress}+60!=wb.virtualAddress ||
               !tail.matchesProducedState(state,fence->virtualAddress,tail.fencePhysical())){reject(17);return;}
            if(!cursor){reject(18);return;}
            if(cursor->allocationBase==*graphicsRingAllocation_ ||
               cursor->allocationBase==*graphicsWriteBackAllocation_){reject(19);return;}
            if(overlap(fence->virtualAddress,8,device,state.size()) ||
               (graphicsSystemIdentifier_ && overlap(fence->virtualAddress,8,*graphicsSystemIdentifier_,4))){reject(20);return;}
            for(const auto& l:leases)if(overlap(l.physical,l.bytes.size(),tail.fencePhysical(),8)){reject(21);return;}
            for(const auto& entry:criticalSections_)
                if(overlap(fence->virtualAddress,8,entry.first,28) || overlap(wb.virtualAddress,4,entry.first,28)){reject(22);return;}
            for(const auto& entry:dispatcherEventWaits_)
                if(overlap(fence->virtualAddress,8,entry.first,16) || overlap(wb.virtualAddress,4,entry.first,16)){reject(23);return;}
            std::array<std::uint8_t,8> fenceBefore{};
            std::copy_n(addressSpace_->translate(fence->virtualAddress),8,fenceBefore.begin());
            auto receipt=completedRuntimeMaintenance_.value_or(NativeRuntimeMaintenanceCompletion{});
            if(receipt.batches==UINT32_MAX){reject(24);return;}
            ++receipt.batches;receipt.previousWord=previous;receipt.readWord=writeWord;
            receipt.draws=plan.points.size();receipt.shaderLoads=plan.shaderLoads;receipt.invalidations=plan.invalidations;
            receipt.fencePhysical=tail.fencePhysical();receipt.fenceValue=tail.completionValue();receipt.commandCursor=tail.commandCursor();
            const auto sameOwner=[&](unsigned physical,unsigned bytes,bool writable,const PhysicalRange& expected) {
                const auto now=resolvePhysicalRange(physical,bytes,writable);
                return now && now->allocationBase==expected.allocationBase && now->virtualAddress==expected.virtualAddress;
            };
            const auto unchanged=[&] {
                if(reentered || !issuerReady() || !nativeInterruptDeviceRoot_(device) ||
                   !ring.writeBack || ring.readWord!=previous || ring.physicalAddress!=original.physicalAddress ||
                   ring.virtualAddress!=original.virtualAddress || ring.bytes!=original.bytes ||
                   ring.writeBack->virtualAddress!=wb.virtualAddress || ring.writeBack->physicalAddress!=wb.physicalAddress ||
                   ring.writeBack->blockSizeLog2!=wb.blockSizeLog2 ||
                   !sameOwner(original.physicalAddress,original.bytes,false,*ownedRing) || !sameOwner(wb.physicalAddress,4,true,*ownedRead) ||
                   !sameOwner(tail.fencePhysical(),8,true,*fence) || !sameOwner(cursorPhysical,4,false,*cursor) ||
                   !readU32(wb.virtualAddress,read) || read!=previous ||
                   std::memcmp(addressSpace_->translate(device),state.data(),state.size()) ||
                   std::memcmp(rootBytes,rootBefore.data(),rootBefore.size()) ||
                   std::memcmp(addressSpace_->translate(fence->virtualAddress),fenceBefore.data(),fenceBefore.size()))return false;
                for(const auto& l:leases)if(!sameOwner(l.physical,l.bytes.size(),false,l.owner) ||
                    std::memcmp(addressSpace_->translate(l.owner.virtualAddress),l.bytes.data(),l.bytes.size()))return false;
                return true;
            };
            std::array<NativeOutputWord,2> outputs{};
            for(unsigned i=0;i<2;++i) {
                const auto& event=plan.events[i];
                const auto offset=event.address-tail.fencePhysical();if(offset>4 || offset%4){reject(25);return;}
                outputs[i]={i*4,{event.address,addressSpace_->translate(fence->virtualAddress+offset),4}};
            }
            if(!unchanged()){reject(26);return;}
            bool publisherCalled=false,publisherRepeated=false;
            diagnostic.reachedExecute=1;
            nativeRuntimeMaintenanceFailed_=true; // Partial native failure cannot be retried.
            const bool executed=execute(root,graph,[&](const NativeInitializationReadback& result) {
                if(publisherCalled){publisherRepeated=true;return false;}publisherCalled=true;
                if(result.draws!=plan.points.size() || result.byteCount!=plan.updates.bufferBytes || !unchanged())return false;
                published=publishNativeGpuOutputs(plan.updates,result.bytes.data(),result.byteCount,outputs.data(),outputs.size(),
                    {wb.physicalAddress,addressSpace_->translate(wb.virtualAddress),4},previous,writeWord);
                return published;
            });
            if(published && (!executed || publisherRepeated))std::_Exit(4);
            if(!executed || !published){reject(27);return;}
            completedRuntimeMaintenance_=receipt;ring.readWord=writeWord;nativeRuntimeMaintenanceFailed_=false;completed=true;
        });
        if(published && (!gated || reentered))std::_Exit(4);
        return gated && entered && !reentered && completed;
    } catch(...) {if(published)std::_Exit(4);return false;}
}

bool GuestImportRegistry::submitNativeMaintenancePoints(std::uint32_t writeWord) {
    using namespace recomp::gpu;
    RECOMP_REGISTRY_LOCK(lock);
    const bool warm=retiredGraphicsRingReadWord_.has_value();
    if(!nativeStateBatchesBound_ || !experimentalIdleCachePairBound_ ||
       (warm && !completedWarmRingCoherency_) || !completedExperimentalInitialization_ || !addressSpace_ ||
       !graphicsInitialized_ || !graphicsBackend_ || !graphicsBackend_->ready() || !graphicsRingConfiguration_ ||
       !graphicsRingAllocation_ || !graphicsWriteBackAllocation_ || experimentalIdleCachePairs_==UINT64_MAX)return false;
    auto& ring=*graphicsRingConfiguration_;
    // Same verified RPTR producer chain as ordinary native state batches. Real
    // maintenance completion joins this chain, with separate draw counters.
    const auto previous=warm?std::max({22u,
        completedWarmStateBatches_?completedWarmStateBatches_->readWord:0,
        completedWarmEmptyCacheCoherency_?completedWarmEmptyCacheCoherency_->readWord:0,
        completedOwnedColor_?completedOwnedColor_->readWord:0,
        completedPostOutputColor_?completedPostOutputColor_->color.readWord:0,
        completedWarmSideEffectFreeBatches_?completedWarmSideEffectFreeBatches_->readWord:0}):std::max({completedExperimentalInitialization_->readWord,
        completedEmptyCacheCoherency_?completedEmptyCacheCoherency_->readWord:0,
        completedNativeStateBatches_?completedNativeStateBatches_->readWord:0,
        completedSideEffectFreeBatches_?completedSideEffectFreeBatches_->readWord:0});
    if(!ring.writeBack || ring.readWord!=previous || std::uint64_t{previous}+3!=writeWord || writeWord>=ring.bytes/4)return false;
    noteNativeSubmissionMayCommitAt(15); // v183 site 15: may publish completion: the renderer's pending reads first
    const auto& wb=*ring.writeBack;
    const auto ownedRing=resolvePhysicalRange(ring.physicalAddress,ring.bytes,false);
    const auto ownedWb=resolvePhysicalRange(wb.physicalAddress,4,true);
    if(!ownedRing || ownedRing->allocationBase!=*graphicsRingAllocation_ || ownedRing->virtualAddress!=ring.virtualAddress ||
       !ownedWb || ownedWb->allocationBase!=*graphicsWriteBackAllocation_ || ownedWb->virtualAddress!=wb.virtualAddress)return false;
    std::uint32_t publishedRead=0;
    if(!readU32(wb.virtualAddress,publishedRead) || publishedRead!=previous)return false;
    bool published=false;
    try {
        const auto overlaps=[](auto a,auto n,auto b,auto m){return a<std::uint64_t{b}+m && b<std::uint64_t{a}+n;};
        Pm4Snapshot root;Pm4Error decoded;
        if(!decodePm4Ring(addressSpace_->translate(ring.virtualAddress),ring.bytes,previous,writeWord,{3,1},root,decoded) ||
           root.words().size()!=3 || root.words()[0]!=0xC0013F00)return false;
        Pm4IndirectGraph graph;Pm4GraphError graphError;
        if(!inspectPm4IndirectGraph(root,[&](auto physical,auto size)->std::optional<std::vector<std::uint8_t>> {
            if(overlaps(physical,size,ring.physicalAddress,ring.bytes) || overlaps(physical,size,wb.physicalAddress,4))return std::nullopt;
            const auto owned=resolvePhysicalRange(physical,size,false);
            if(!owned || (graphicsSystemIdentifier_ && overlaps(owned->virtualAddress,size,*graphicsSystemIdentifier_,4)))return std::nullopt;
            const auto* bytes=addressSpace_->translate(owned->virtualAddress);
            return std::vector<std::uint8_t>(bytes,bytes+size);
        },graph,graphError,{64,8,4096,4096}))return false;
        MaintenancePointPlan plan;MaintenancePointError error;
        if(!planMaintenancePoints(root,graph,plan,error,true))return false;
        auto& completion=warm?completedWarmStateBatches_:completedNativeStateBatches_;
        auto state=completion.value_or(NativeStateBatchCompletion{});
        if(state.batches==UINT32_MAX || state.maintenanceBatches==UINT32_MAX ||
           state.words>UINT32_MAX-plan.events.size() || state.stateWrites>UINT32_MAX-plan.registerWrites ||
           state.invalidations>UINT32_MAX-plan.invalidations || state.maintenanceDraws>UINT32_MAX-plan.points.size())return false;
        ++state.batches;++state.maintenanceBatches;state.readWord=writeWord;
        state.words+=static_cast<unsigned>(plan.events.size());state.stateWrites+=plan.registerWrites;
        state.invalidations+=plan.invalidations;state.maintenanceDraws+=static_cast<unsigned>(plan.points.size());
        std::array<NativeOutputWord,64> destinations{};
        for(std::size_t i=0;i<plan.events.size();++i) {
            const auto physical=plan.events[i].address;const auto owned=resolvePhysicalRange(physical,4,true);
            if(!owned || overlaps(physical,4,ring.physicalAddress,ring.bytes) || overlaps(physical,4,wb.physicalAddress,4) ||
               (graphicsSystemIdentifier_ && owned->virtualAddress==*graphicsSystemIdentifier_))return false;
            for(const auto& child:graph.nodes)
                if(overlaps(physical,4,child.physicalAddress,child.snapshot.words().size()*4))return false;
            destinations[i]={static_cast<unsigned>(i*4),{physical,addressSpace_->translate(owned->virtualAddress),4}};
        }
        bool publisherCalled=false;
        // Lifecycle lock pins all current owners; the caller's exclusive guest
        // executor token parks raw generated stores throughout the native fence.
        const bool completed=RECOMP_V_BACKEND()->executeExperimentalMaintenancePoints(root,graph,
            [&](const NativeInitializationReadback& readback) {
                if(publisherCalled)return false;publisherCalled=true;
                if(readback.draws!=plan.points.size() || readback.byteCount!=plan.updates.bufferBytes)return false;
                published=publishNativeGpuOutputs(plan.updates,readback.bytes.data(),readback.byteCount,
                    destinations.data(),plan.events.size(),{wb.physicalAddress,addressSpace_->translate(wb.virtualAddress),4},previous,writeWord);
                return published;
            });
        if(published && !completed)std::_Exit(4); // Contract violation: never retry published work.
        if(!completed || !published)return false;
        completion=state;++experimentalIdleCachePairs_;ring.readWord=writeWord;return true;
    } catch(...) {
        if(published)std::_Exit(4);
        return false;
    }
}

bool GuestImportRegistry::submitVerifiedSideEffectFreeBatch(std::uint32_t writeWord) {
    using namespace recomp::gpu;
    RECOMP_REGISTRY_LOCK(lock);
    const bool warm=retiredGraphicsRingReadWord_.has_value();
    if (!nativeStateBatchesBound_ || (warm && !completedWarmRingCoherency_) || !completedExperimentalInitialization_ || !addressSpace_ ||
        !graphicsInitialized_ || !graphicsBackend_ || !graphicsBackend_->ready() || !graphicsRingConfiguration_ ||
        !graphicsRingAllocation_ || !graphicsWriteBackAllocation_) return false;
    auto& ring=*graphicsRingConfiguration_;
    // Same shared ring-position chain submitNativeStateBatch uses (kept
    // consistent with it: each recognizes the other's completion record too),
    // so this may run whichever of the two functions the batch actually
    // needs, in either order.
    const auto previous=warm ? std::max({22u,
        completedWarmStateBatches_?completedWarmStateBatches_->readWord:0,
        completedWarmEmptyCacheCoherency_?completedWarmEmptyCacheCoherency_->readWord:0,
        completedOwnedColor_?completedOwnedColor_->readWord:0,
        completedPostOutputColor_?completedPostOutputColor_->color.readWord:0,
        completedWarmSideEffectFreeBatches_?completedWarmSideEffectFreeBatches_->readWord:0}) : std::max({completedExperimentalInitialization_->readWord,
        completedEmptyCacheCoherency_?completedEmptyCacheCoherency_->readWord:0,
        completedNativeStateBatches_?completedNativeStateBatches_->readWord:0,
        completedSideEffectFreeBatches_?completedSideEffectFreeBatches_->readWord:0});
    if (!ring.writeBack || ring.readWord!=previous || std::uint64_t{previous}+3!=writeWord || writeWord>=ring.bytes/4) return false;
    noteNativeSubmissionMayCommitAt(16); // v183 site 16: may publish completion: the renderer's pending reads first
    const auto& wb=*ring.writeBack;
    const auto ownedRing=resolvePhysicalRange(ring.physicalAddress,ring.bytes,false);
    const auto ownedWb=resolvePhysicalRange(wb.physicalAddress,4,true);
    if (!ownedRing || ownedRing->allocationBase!=*graphicsRingAllocation_ || ownedRing->virtualAddress!=ring.virtualAddress ||
        !ownedWb || ownedWb->allocationBase!=*graphicsWriteBackAllocation_ || ownedWb->virtualAddress!=wb.virtualAddress) return false;
    std::uint32_t publishedRead=0;
    if (!readU32(wb.virtualAddress,publishedRead) || publishedRead!=previous) return false;
    try {
        const auto overlaps=[](auto a,auto n,auto b,auto m) {return a<std::uint64_t{b}+m && b<std::uint64_t{a}+n;};
        std::array<std::uint32_t,3> root{};
        for (unsigned i=0;i<3;++i) if (!readU32(ring.virtualAddress+(previous+i)*4,root[i])) return false;
        constexpr std::uint32_t maxRootWords=4096;  // Matches SideEffectFreeBatchLimits::maxWords.
        // Only handle batches submitNativeStateBatch structurally cannot: its
        // own hard cap (root[2]>256 rejects it, see that function's identical
        // root check) is the dividing line. This function is reached in
        // bindGuestGraphicsSubmission's OR-chain only AFTER submitNativeStateBatch
        // has already rejected the same word (it runs first), so a batch this
        // small was never going to get submitNativeStateBatch's own
        // publishNativeGpuOutputs readback regardless of this gate. The real
        // reason for this bound: without it, a small batch rejected by
        // submitNativeStateBatch for CONTENT (e.g. it contains draws) could
        // still be claimed here ahead of a later, possibly more appropriate
        // handler further down the same OR-chain. A device replay of an
        // unqualified version of this function hung the guest very early
        // (worker threads blocked, 0% CPU); this bound was applied in
        // response, but the precise mechanism connecting the two was not
        // confirmed on hardware before this was written - see
        // docs/claude/CONTEXT.md §0.5 for the full, honest account, including
        // an earlier draft of this comment that incorrectly claimed this
        // function ran BEFORE submitNativeStateBatch.
        if (root[0]!=0xC0013F00 || !root[2] || root[2]<=256 || root[2]>maxRootWords || (root[1]&3) ||
            std::uint64_t{root[1]}+root[2]*4>0x20000000) return false;
        // Every physical range this touches - the root and every nested
        // indirect buffer the verifier follows - must be guest-owned and
        // disjoint from the ring, its writeback word and the system
        // identifier, exactly like submitNativeStateBatch's own root check.
        // Guest memory is read-only here; nothing is ever mutated.
        const auto resolver=[&](std::uint32_t address,std::uint32_t wordCount,std::vector<std::uint32_t>& out)->bool {
            if (!wordCount || wordCount>maxRootWords || (address&3) || std::uint64_t{address}+std::uint64_t{wordCount}*4>0x20000000) return false;
            const auto range=resolvePhysicalRange(address,wordCount*4,false);
            if (!range || overlaps(address,std::uint64_t{wordCount}*4,ring.physicalAddress,ring.bytes) ||
                overlaps(address,std::uint64_t{wordCount}*4,wb.physicalAddress,4) ||
                (graphicsSystemIdentifier_ && overlaps(range->virtualAddress,std::uint64_t{wordCount}*4,*graphicsSystemIdentifier_,4)))
                return false;
            out.resize(wordCount);
            for (std::uint32_t i=0;i<wordCount;++i) if (!readU32(range->virtualAddress+i*4,out[i])) return false;
            return true;
        };
        std::vector<std::uint32_t> rootWords;
        if (!resolver(root[1],root[2],rootWords)) return false;
        SideEffectFreeBatchResult verified; SideEffectFreeBatchError verifyError;
        if (!verifySideEffectFreeCommandBatch(rootWords,resolver,{},verified,verifyError)) return false;
        auto& completion=warm?completedWarmSideEffectFreeBatches_:completedSideEffectFreeBatches_;
        auto state=completion.value_or(SideEffectFreeBatchCompletion{});
        if (state.batches==UINT32_MAX || state.draws>UINT32_MAX-verified.drawsVerified ||
            state.indirectBuffers>UINT32_MAX-verified.indirectBuffersFollowed) return false;
        ++state.batches; state.readWord=writeWord;
        state.draws+=verified.drawsVerified; state.indirectBuffers+=verified.indirectBuffersFollowed;
        std::uint32_t publishedWrite=writeWord;
        if (!writeU32(wb.virtualAddress,publishedWrite)) return false;
        // Only non-throwing, private bookkeeping remains after guest publication.
        completion=state;
        ring.readWord=writeWord;
        return true;
    } catch (...) { return false; }
}
std::optional<GuestImportRegistry::SideEffectFreeBatchCompletion> GuestImportRegistry::completedSideEffectFreeBatches() const {
    RECOMP_REGISTRY_LOCK(lock); return completedSideEffectFreeBatches_;
}
std::optional<GuestImportRegistry::SideEffectFreeBatchCompletion> GuestImportRegistry::completedWarmSideEffectFreeBatches() const {
    RECOMP_REGISTRY_LOCK(lock); return completedWarmSideEffectFreeBatches_;
}

bool GuestImportRegistry::bindEmptyGuestCacheCoherency() {
    RECOMP_REGISTRY_LOCK(lock);
    if (!experimentalInitializationBound_ || graphicsInitialized_ || emptyCacheCoherencyBound_) return false;
    emptyCacheCoherencyBound_ = true; return true;
}
std::optional<GuestImportRegistry::EmptyCacheCoherencyCompletion>
GuestImportRegistry::completedEmptyGuestCacheCoherency() const {
    RECOMP_REGISTRY_LOCK(lock); return completedEmptyCacheCoherency_;
}
std::optional<GuestImportRegistry::EmptyCacheCoherencyCompletion>
GuestImportRegistry::completedWarmEmptyCacheCoherency() const {
    RECOMP_REGISTRY_LOCK(lock); return completedWarmEmptyCacheCoherency_;
}
bool GuestImportRegistry::submitEmptyGuestCacheCoherency(std::uint32_t writeWord) {
    RECOMP_REGISTRY_LOCK(lock);
    const bool warm=retiredGraphicsRingReadWord_.has_value();
    if (!emptyCacheCoherencyBound_ || (warm && !completedWarmRingCoherency_) || !completedExperimentalInitialization_ || !addressSpace_ ||
        !graphicsInitialized_ || !graphicsBackend_ || !graphicsBackend_->ready() || !graphicsRingConfiguration_ ||
        !graphicsRingAllocation_ || !graphicsWriteBackAllocation_) return false;
    auto& ring = *graphicsRingConfiguration_;
    const auto previous = warm ? std::max({22u,
        completedWarmStateBatches_?completedWarmStateBatches_->readWord:0,
        completedWarmEmptyCacheCoherency_?completedWarmEmptyCacheCoherency_->readWord:0}) : std::max({completedExperimentalInitialization_->readWord,
        completedEmptyCacheCoherency_?completedEmptyCacheCoherency_->readWord:0,
        completedNativeStateBatches_?completedNativeStateBatches_->readWord:0});
    if (!ring.writeBack || ring.readWord != previous || std::uint64_t{previous}+3 != writeWord || writeWord >= ring.bytes/4) return false;
    noteNativeSubmissionMayCommitAt(17); // v183 site 17: may publish completion: the renderer's pending reads first
    const auto& wb = *ring.writeBack;
    const auto ownedRing = resolvePhysicalRange(ring.physicalAddress,ring.bytes,false);
    const auto ownedWb = resolvePhysicalRange(wb.physicalAddress,4,true);
    if (!ownedRing || ownedRing->allocationBase != *graphicsRingAllocation_ || ownedRing->virtualAddress != ring.virtualAddress ||
        !ownedWb || ownedWb->allocationBase != *graphicsWriteBackAllocation_ || ownedWb->virtualAddress != wb.virtualAddress) return false;
    try {
        std::vector<std::uint32_t> root(3), child(11);
        for (unsigned i=0;i<3;++i) if (!readU32(ring.virtualAddress+(previous+i)*4,root[i])) return false;
        if (root[0] != 0xC0013F00 || root[2] != 11 || (root[1]&3) || std::uint64_t{root[1]}+44 > 0x20000000) return false;
        const auto ownedChild = resolvePhysicalRange(root[1],44,false);
        const auto overlaps = [](auto a, auto n, auto b, auto m) { return a < std::uint64_t{b}+m && b < std::uint64_t{a}+n; };
        if (!ownedChild || overlaps(root[1],44,ring.physicalAddress,ring.bytes) || overlaps(root[1],44,wb.physicalAddress,4) ||
            (graphicsSystemIdentifier_ && overlaps(ownedChild->virtualAddress,44,*graphicsSystemIdentifier_,4))) return false;
        for (unsigned i=0;i<11;++i) if (!readU32(ownedChild->virtualAddress+i*4,child[i])) return false;
        NativeInitialCoherency request;
        if (!parseNativeEmptyCacheCoherency(root,child,request) || !RECOMP_V_BACKEND()->completeEmptyGuestCacheCoherency(request)) return false;
        // All guest writers remain parked and all allocations lifecycle-locked.
        // No guest payload is uploaded/modified. Publish only this fenced batch.
        auto& completion=warm?completedWarmEmptyCacheCoherency_:completedEmptyCacheCoherency_;
        const auto batches = completion ? completion->batches+1 : 1;
        completion = EmptyCacheCoherencyCompletion{batches,writeWord,request};
        (void)writeU32(wb.virtualAddress,writeWord); ring.readWord = writeWord;
        return true;
    } catch (...) { return false; }
}

bool GuestImportRegistry::bindExperimentalNativeInitialization() {
    RECOMP_REGISTRY_LOCK(lock);
    if (!initialCoherencySubmissionBound_ || graphicsInitialized_ || experimentalInitializationBound_) return false;
    experimentalInitializationBound_ = true; return true;
}
std::optional<GuestImportRegistry::NativeInitializationCompletion>
GuestImportRegistry::completedExperimentalNativeInitialization() const {
    RECOMP_REGISTRY_LOCK(lock); return completedExperimentalInitialization_;
}
bool GuestImportRegistry::submitExperimentalNativeInitialization(std::uint32_t writeWord) {
    using namespace recomp::gpu;
    RECOMP_REGISTRY_LOCK(lock);
    if (!experimentalInitializationBound_ || completedExperimentalInitialization_ || !completedInitialCoherency_ ||
        !addressSpace_ || !graphicsInitialized_ || !graphicsBackend_ || !graphicsBackend_->ready() ||
        !graphicsRingConfiguration_ || !graphicsRingAllocation_ || !graphicsWriteBackAllocation_) return false;
    auto& ring = *graphicsRingConfiguration_;
    if (ring.readWord != 22 || !ring.writeBack || writeWord <= ring.readWord || writeWord >= ring.bytes/4) return false;
    noteNativeSubmissionMayCommitAt(18); // v183 site 18: may publish completion: the renderer's pending reads first
    const auto& wb = *ring.writeBack;
    const auto ownedRing = resolvePhysicalRange(ring.physicalAddress,ring.bytes,false);
    const auto ownedWb = resolvePhysicalRange(wb.physicalAddress,4,true);
    if (!ownedRing || ownedRing->allocationBase != *graphicsRingAllocation_ || ownedRing->virtualAddress != ring.virtualAddress ||
        !ownedWb || ownedWb->allocationBase != *graphicsWriteBackAllocation_ || ownedWb->virtualAddress != wb.virtualAddress) return false;
    try {
        const auto overlaps = [](std::uint32_t a, std::uint32_t n, std::uint32_t b, std::uint32_t m) {
            return a < std::uint64_t{b}+m && b < std::uint64_t{a}+n;
        };
        std::uint32_t publishedRead=0;
        if (!readU32(wb.virtualAddress,publishedRead) || publishedRead!=22) return false;
        Pm4Snapshot root; Pm4Error decoded;
        if (!decodePm4Ring(addressSpace_->translate(ring.virtualAddress),ring.bytes,ring.readWord,writeWord,{},root,decoded)) return false;
        Pm4IndirectGraph graph; Pm4GraphError graphError;
        if (!inspectPm4IndirectGraph(root,[&](auto physical, auto size) -> std::optional<std::vector<std::uint8_t>> {
                if (overlaps(physical,size,ring.physicalAddress,ring.bytes) || overlaps(physical,size,wb.physicalAddress,4))
                    return std::nullopt;
                const auto owned = resolvePhysicalRange(physical,size,false);
                if (!owned) return std::nullopt;
                const auto* bytes = addressSpace_->translate(owned->virtualAddress);
                return std::vector<std::uint8_t>(bytes,bytes+size);
            },graph,graphError)) return false;
        StoppedConstantInitialization plan; NativeInitializationError planError;
        if (!planExperimentalNativeInitialization(root,graph,plan,planError)) return false;
        std::array<NativeOutputWord,64> destinations{};
        for (std::size_t i = 0; i < plan.memory.destinationCount; ++i) {
            const auto physical = plan.memory.destinations[i].physicalAddress;
            const auto owned = resolvePhysicalRange(physical,4,true);
            if (!owned || (graphicsSystemIdentifier_ && owned->virtualAddress == *graphicsSystemIdentifier_) ||
                overlaps(physical,4,ring.physicalAddress,ring.bytes) || overlaps(physical,4,wb.physicalAddress,4)) return false;
            for (const auto& child : graph.nodes)
                if (overlaps(physical,4,child.physicalAddress,static_cast<std::uint32_t>(child.snapshot.words().size()*4))) return false;
            destinations[i] = {plan.memory.destinations[i].stagingByteOffset,
                {physical,addressSpace_->translate(owned->virtualAddress),4}};
        }
        // The registry lifecycle lock pins EVERY allocation (including children
        // and destinations) for the entire transaction. The calling scheduler
        // owns its execution token, so generated direct stores are parked too.
        // No async callback or guest code may run while the backend waits.
        NativeInitializationReadback readback;
        if (!RECOMP_V_BACKEND()->completeExperimentalConstantInitialization(plan,readback) ||
            readback.byteCount != plan.memory.updates.bufferBytes || readback.draws != plan.draws.size()) return false;
        if (!publishNativeGpuOutputs(plan.memory.updates,readback.bytes.data(),readback.byteCount,
                destinations.data(),plan.memory.destinationCount,
                {wb.physicalAddress,addressSpace_->translate(wb.virtualAddress),4},22,writeWord)) return false;
        ring.readWord = writeWord;
        completedExperimentalInitialization_ = NativeInitializationCompletion{writeWord,
            static_cast<std::uint32_t>(plan.draws.size()),static_cast<std::uint32_t>(plan.memory.destinationCount),
            static_cast<std::uint32_t>(plan.registers.size())};
        return true;
    } catch (...) { return false; }
}
}
