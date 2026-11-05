#include "guest_import_registry.h"
#include "guest_cooperative_executor.h"
#include "native_tile_lock_call.h"
#include "xenos_native_initialization.h"
#include "xenos_native_state_batch.h"
#include "xenos_copy_shader_fixture.h"
#include "native_color_batch_fixture.h"
#include "native_color_execution_result.h"
#include "native_color_state_commit.h"
#include "native_swap_image.h"
#include "native_display_information.h"
#include "native_identity_scaler_fixture.h"
#include "native_scaler_wrapper_fixture.h"
#include "native_display_gamma_fixture.h"
#include "native_display_output_fixture.h"
#include "native_post_output_swap_fixture.h"
#include "native_deferred_color_state_commit.h"
#include <cassert>
#include <cstring>
#include <stdexcept>
#include <future>
#include <thread>
#include <cstdio>
#include <sys/wait.h>
#include <unistd.h>
using namespace recomp::bootstrap;
using namespace recomp::gpu;
using namespace recomp::gpu::test;
namespace {
class Backend final : public GuestGraphicsBackend {
public:
    unsigned secondSwapCalls=0,secondSwapFault=UINT32_MAX,secondSwapActions=0;
    std::function<void()> secondSwapAfter=[]{};
    bool executeNativePostOutputSwapWrapper(const NativePostOutputSwapWrapper& plan,const std::function<bool()>& present,
            const NativePostOutputSwapInterrupt& interrupt,NativePostOutputSwapGpuResult& result) override {
        ++secondSwapCalls;
        if(secondSwapFault==0)return false;
        if(secondSwapFault==1)throw std::runtime_error("second swap preaccept");
        if(secondSwapFault==2)return true;
        const NativePostOutputSwapInterruptSequence::Action action=[&](const auto&){++secondSwapActions;return true;};
        if(secondSwapFault==11)assert(!interrupt(action));
        if(!present())return false;
        if(secondSwapFault==6)return false;
        if(secondSwapFault==7)throw std::runtime_error("second swap accepted");
        if(secondSwapFault==12)(void)present();
        if(secondSwapFault!=8 && !interrupt(action))return false;
        if(secondSwapFault==13)(void)interrupt(action);
        // Explicit CPU-only synthetic receipt; original shader/GPU not run.
        result.beforeCompletionNs=1000;result.afterCompletionNs=1010;
        result.nativeFences=6;result.interruptIdleFences=1;result.interruptAcknowledged=true;
        for(unsigned i=0;i<4;++i){const auto value=i<2?(i?505u:500u):*plan.events()[i].literal;
            for(unsigned b=0;b<4;++b)result.eventBytes[i*4+b]=value>>(i<2?8*b:24-8*b);}
        if(secondSwapFault==9)result.nativeFences=4;
        if(secondSwapFault==10)result.eventBytes[0]^=1;
        secondSwapAfter();return true;
    }
    unsigned nativeScratchBase=0,nativeScratchMask=0x20037;
    bool queryNativeScratchWriteback(unsigned& base,unsigned& mask) const noexcept override {
        if(!nativeScratchBase)return false;base=nativeScratchBase;mask=nativeScratchMask;return true;
    }
    std::optional<NativeScalerInterruptSequence> irqSequence;
    bool irqAccept=false;
    unsigned irqFences=0,irqAcks=0;
    bool tailAccept=false;
    unsigned tailFault=0,tailCalls=0;
    std::function<void()> tailBefore=[]{};
    unsigned gammaCalls=0,gammaFault=0;
    unsigned outputCalls=0,outputFault=0;
    bool outputAccept=false;
    std::function<void()> outputAfter=[]{};
    bool executeExperimentalNativeDisplayOutput(const NativeDisplayOutputWrapper& plan,const std::function<bool()>& present,NativeDisplayOutputResult& result) override {
        ++outputCalls;if(!outputAccept)return false;
        if(outputFault==1)throw std::runtime_error("output before present");
        if(outputFault==2)return true;
        if(!present())return false;
        if(outputFault==5)return false;
        if(outputFault==6)throw std::runtime_error("output after present");
        if(outputFault==10)(void)present();
        result.presentQueued=true;result.nativeFences=2;
        for(unsigned i=0;i<2;++i)for(unsigned b=0;b<4;++b)result.eventBytes[i*4+b]=plan.eventValue(i)>>(24-b*8);
        if(outputFault==7)result.presentQueued=false;
        if(outputFault==8)result.nativeFences=1;
        if(outputFault==9)result.eventBytes[0]^=1;
        outputAfter();return true;
    }
    bool gammaAccept=false;
    std::function<void()> gammaAfter=[]{};
    bool executeNativeDisplayGamma(const NativeDisplayGammaWrapper& plan,const std::function<bool()>& present,NativeDisplayGammaResult& result) override {
        ++gammaCalls;if(!gammaAccept)return false;
        if(gammaFault==1)throw std::runtime_error("synthetic gamma before present");
        if(gammaFault==2)return true;
        if(!present())return false;
        if(gammaFault==5)return false;
        if(gammaFault==6)throw std::runtime_error("synthetic gamma after present");
        if(gammaFault==10)(void)present();
        result.presentQueued=true;result.nativeFences=2;
        for(unsigned i=0;i<2;++i)for(unsigned b=0;b<4;++b)result.eventBytes[i*4+b]=plan.eventValue(i)>>(24-b*8);
        if(gammaFault==7)result.presentQueued=false;
        if(gammaFault==8)result.nativeFences=1;
        if(gammaFault==9)result.eventBytes[0]^=1;
        gammaAfter();return true;
    }
    bool postStorageAccept=false,postStorageThrow=false;
    bool postOutputAccept=false,postOutputThrow=false;
    unsigned postOutputCalls=0;
    unsigned runtimeStorageCalls=0;
    bool runtimeStorageAccept=false,runtimeStorageThrow=false;
    std::function<void()> runtimeStorageBefore=[]{};
    bool completeNativeRuntimeCommandStorageCoherency(const NativeInitialCoherency& request) override {
        ++runtimeStorageCalls;
        assert(request.cacheFlags==0x01000000 && request.byteCount==0x4000);
        if(runtimeStorageThrow)throw std::runtime_error("synthetic runtime command fence failure");
        if(!runtimeStorageAccept)return false;
        runtimeStorageBefore();return true;
    }
    unsigned swapStorageCalls=0;
    bool swapStorageAccept=false,swapStorageThrow=false;
    std::function<void()> swapStorageBefore=[]{};
    bool completeNativePostOutputSwapStorageCoherency(const NativeInitialCoherency& request) override {
        ++swapStorageCalls;assert(request.cacheFlags==0x01000000 && request.byteCount==0x4000 && postColorCalls==1);
        if(swapStorageThrow)throw std::runtime_error("synthetic second-swap storage failure");
        if(!swapStorageAccept)return false;
        swapStorageBefore();return true;
    }
    unsigned deferredStorageCalls=0;
    bool deferredStorageAccept=false,deferredStorageThrow=false;
    std::function<void()> deferredStorageAfter=[]{};
    bool completeNativeDeferredSwapStorageCoherency(const NativeInitialCoherency& request) override {
        ++deferredStorageCalls;assert(request.cacheFlags==0x01000000 && request.byteCount==0x4000 && secondSwapCalls==1);
        if(deferredStorageThrow)throw std::runtime_error("synthetic deferred-swap fence failure");
        if(!deferredStorageAccept)return false;
        deferredStorageAfter();return true;
    }
    std::function<void()> postOutputBefore=[]{ };
    bool completeNativePostOutputStorageCoherency(const NativeInitialCoherency& request) override {
        ++postOutputCalls;assert(request.cacheFlags==0x01000000 && request.byteCount==0x4000 && outputCalls==1);
        if(postOutputThrow)throw std::runtime_error("synthetic post-output storage failure");
        if(!postOutputAccept)return false;
        postOutputBefore();return true;
    }
    bool postDisplayAccept=false,postDisplayThrow=false;
    unsigned postDisplayCalls=0;
    std::function<void()> postDisplayBefore=[]{};
    bool completeNativePostDisplayStorageCoherency(const NativeInitialCoherency& request) override {
        ++postDisplayCalls;assert(request.cacheFlags==0x01000000 && request.byteCount==0x4000 && gammaCalls==1);
        if(postDisplayThrow)throw std::runtime_error("synthetic post-display storage failure");
        if(!postDisplayAccept)return false;
        postDisplayBefore();return true;
    }
    unsigned postStorageCalls=0;
    std::function<void()> postStorageBefore=[]{};
    bool completeNativePostScalerStorageCoherency(const NativeInitialCoherency& request) override {
        ++postStorageCalls;assert(request.cacheFlags==0x01000000 && request.byteCount==0x4000);
        if (postStorageThrow) throw std::runtime_error("synthetic post-scaler storage failure");
        if (!postStorageAccept) return false;
        postStorageBefore();return true;
    }
    bool completeNativeScalerTail(NativeScalerTailResult& result) override {
        ++tailCalls;if (!tailAccept) return false;
        if (tailFault==1) throw std::runtime_error("synthetic native tail failure");
        if (tailFault==2) return true; // No actual completion receipt.
        assert(irqSequence);NativeScalerDisplayState display;
        assert(irqSequence->completeTail([&](const auto& op) {
            if (op.wordOffset<=69) return display.apply(op,irqSequence->plan().command(),[]{return true;});
            return true; // Explicit CPU fixture: no GPU work or event publication.
        }));
        result.configured=display.configured();result.nativeFences=display.nativeFences()+1;
        for (unsigned i=0;i<2;++i) for (unsigned b=0;b<4;++b)
            result.eventBytes[i*4+b]=irqSequence->plan().operations()[24+i].value>>(24-b*8);
        if (tailFault==3) result.nativeFences=2;
        if (tailFault==4) result.eventBytes[0]^=1;
        if (tailFault==5) result.configured=false;
        tailBefore();return true;
    }
    bool beginNativeScalerInterrupt(const NativeScalerWrapper& plan,const NativeScalerInterruptSequence::Action& memory) override {
        if (!irqAccept || irqSequence) return false;
        irqSequence.emplace(plan);
        const bool ready=irqSequence->prefix(plan.scratchPhysical(),0x20037,[&](const auto& op) {
            using Kind=NativeScalerWrapper::Kind;
            if (op.kind==Kind::BinMaskLow) return true;
            if (op.kind==Kind::WaitIdle) {++irqFences;return true;} // Explicit synthetic CPU backend only.
            return memory(op);
        });
        return ready && irqSequence->enterGuest();
    }
    bool acknowledgeNativeScalerInterrupt(const NativeScalerInterruptSequence::Action& memory) override {
        if (!irqSequence || !irqSequence->guestReturned()) return false;
        return irqSequence->acknowledge([&](const auto& op) {
            if (op.kind==NativeScalerWrapper::Kind::CallbackAcknowledge) {++irqAcks;return true;}
            return memory(op);
        });
    }
    bool initialize() override { active = true; return true; }
    bool ready() const noexcept override { return active; }
    bool displayAccept=false,displaySelected=false,displayQuery=true;
    NativeDisplayConfiguration displayConfiguration{{1280,720,1280,720},{0,0,1280,720,1280,720,1,0,0,0,1,0,0,0}};
    bool selectNativeDisplayMode(std::uint32_t flags) noexcept override {
        if(!active || !displayAccept || displaySelected || flags!=0x40000000)return false;
        displaySelected=true;return true;
    }
    bool querySelectedDisplayConfiguration(NativeDisplayConfiguration& output) const noexcept override {
        if(!active || !displaySelected || !displayQuery)return false;
        output=displayConfiguration;return true;
    }
    EdramTrainingPolicy queryEdramTrainingPolicy() override {
        ++trainingCalls;
        if (throws) throw std::runtime_error("fixture");
        return trainingAvailable ? EdramTrainingPolicy::DriverManagedNoXenosLink : EdramTrainingPolicy::Unavailable;
    }
    bool trainingAvailable = false;
    unsigned trainingCalls = 0;
    bool completeInitialEmptyCacheCoherency(const NativeInitialCoherency&) override { return true; }
    bool commandAccept=false,commandThrow=false;
    bool scalerStorageAccept=false,scalerStorageThrow=false;
    unsigned scalerStorageCalls=0;
    std::function<void()> scalerStorageBefore=[]{};
    bool completeNativeScalerStorageCoherency(const NativeInitialCoherency&) override {
        ++scalerStorageCalls;
        if (scalerStorageThrow) throw std::runtime_error("synthetic scaler-storage refusal");
        if (!scalerStorageAccept) return false;
        scalerStorageBefore();return true;
    }
    unsigned wrapperCalls=0,wrapperFault=0;bool wrapperAccept=false;
    std::function<void()> wrapperAfter=[]{};
    bool executeNativeSwapWrapper(const NativeSwapWrapper& plan,const std::function<bool()>& present,NativeSwapGpuResult& result) override {
        ++wrapperCalls;
        if(wrapperFault==1)throw std::runtime_error("synthetic wrapper refusal");
        if(wrapperFault==2)return true; // True without callback is not completion.
        if(!wrapperAccept || !present())return false;
        // CPU transaction fixture only. No Vulkan work or real presentation.
        result.nativeFences=4;result.beforeCompletionNs=0x1fffffff0ULL;result.afterCompletionNs=0x200000020ULL;
        for(unsigned i=0;i<4;++i){const auto value=i<2?static_cast<std::uint32_t>((i==0?result.beforeCompletionNs:result.afterCompletionNs)/2):*plan.events()[i].literal;
            for(unsigned b=0;b<4;++b)result.eventBytes[i*4+b]=value>>(i<2?b*8:24-b*8);}
        wrapperAfter();
        if(wrapperFault==3)return false;
        if(wrapperFault==4)throw std::runtime_error("synthetic accepted contract violation");
        if(wrapperFault==5)--result.nativeFences;
        if(wrapperFault==6)result.eventBytes[0]^=1;
        if(wrapperFault==7)result.afterCompletionNs=result.beforeCompletionNs-1;
        if(wrapperFault==8)result.afterCompletionNs=result.beforeCompletionNs+8'589'934'592ULL;
        if(wrapperFault==9)(void)present(); // Must fail-stop before double callback.
        return true;
    }
    unsigned commandCalls=0;
    NativeInitialCoherency lastCommand;
    std::function<void()> commandBefore=[]{};
    bool completeNativeCommandStorageCoherency(const NativeInitialCoherency& request) override {
        ++commandCalls;lastCommand=request;commandBefore();
        if(commandThrow)throw std::runtime_error("synthetic command fence refusal");
        return commandAccept;
    }
    unsigned colorCalls=0,colorFault=0;bool colorAccept=false,colorCommitted=false;
    unsigned postColorCalls=0;bool postColorRendering=false;
    bool executeExperimentalPostOutputColorBatch(const NativeColorChainProgram& program,const NativeColorBatchCapture& capture,
            const NativeColorPublisher& publisher) override {
        ++postColorCalls;postColorRendering=true;
        assert(capture.synchronization().phase==NativeColorBatchPhase::PostOutput);
        return executeExperimentalOwnedColorBatch(program,capture,publisher); // Shared CPU fixture only.
    }
    std::function<void()> colorBefore=[]{};
    unsigned deferredColorCalls=0,deferredColorFault=0;
    bool deferredColorAccept=false;
    std::function<void()> deferredColorBefore=[]{};
    bool executeExperimentalDeferredColorPrefix(const NativeColorChainProgram&,const NativeDeferredColorCapture& capture,
            const NativeDeferredColorPublisher& publisher) override {
        ++deferredColorCalls;deferredColorBefore();
        if(deferredColorFault==1)throw std::runtime_error("synthetic deferred prefix failure");
        if(deferredColorFault==2)return true; // No publication is not completion.
        if(!deferredColorAccept)return false;
        // CPU-only registry contract test. No real GPU or completion evidence.
        using P=NativeColorChainProfile<true,true,true>;using E=NativeColorEdramSnapshot;
        std::vector<std::uint8_t> half(P::OutputBytes,0x5A),edram(P::BackingBytes,0x5A),tiled(P::CopyBytes,0x5A);
        std::fill_n(half.begin()+16,1280*720*8,0);std::fill_n(edram.begin()+16+E::Begin,E::Bytes,0);
        for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x){unsigned at=0;assert(colorCopyOffset(1280,720,x,y,at));
            std::fill_n(tiled.begin()+16+at,4,0);}
        NativeDeferredColorExecutionResult result;result.submission=capture.submission();
        result.edram=E::capture(half.data(),half.size(),edram.data(),edram.size(),tiled.data(),tiled.size());
        result.copy=NativeTiledCopyPublication::capture(capture.copy().copy,tiled.data(),tiled.size());assert(result.edram && result.copy);
        result.nativeFences=4;result.leadingSetupFences=1;result.auditedPrefixTimeline.emplace();
        assert(result.auditedPrefixTimeline->finishAuditedLeadingFence());
        for(unsigned i=0;i<4;++i)assert(result.auditedPrefixTimeline->begin(i) && result.auditedPrefixTimeline->finishAuditedStage(i));
        result.pendingRegisters.emplace();result.pendingState.emplace();
        if(deferredColorFault==3)result.submission.reset();
        if(deferredColorFault==4)result.auditedPrefixTimeline.reset();
        if(deferredColorFault==5)result.nativeFences=3;
        if(deferredColorFault==6)result.copy.reset();
        const bool published=publisher.publishCopy(publisher.context,result);
        if(published && deferredColorFault==7)(void)publisher.publishCopy(publisher.context,result);
        if(published && deferredColorFault==8)return false;
        if(published && deferredColorFault==9)throw std::runtime_error("synthetic prefix post-publication failure");
        return published; // Simulated backend commit, not a device state test.
    }
    bool executeExperimentalOwnedColorBatch(const NativeColorChainProgram&,const NativeColorBatchCapture& capture,
            const NativeColorPublisher& publisher) override {
        ++colorCalls;colorBefore();
        if(colorFault==1)throw std::runtime_error("synthetic color failure");
        if(colorFault==2)return true; // Invalid backend: success without publication must reject.
        if(!colorAccept)return false;
        // CPU registry-contract fixture ONLY: no GPU execution, fences or
        // executable native shader is claimed by these synthetic result bytes.
        using P=NativeColorChainProfile<true,true,true>;using E=NativeColorEdramSnapshot;
        std::vector<std::uint8_t> half(P::OutputBytes,0x5A),edram(P::BackingBytes,0x5A),tiled(P::CopyBytes,0x5A);
        std::fill_n(half.begin()+16,1280*720*8,0);std::fill_n(edram.begin()+16+E::Begin,E::Bytes,0);
        for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x){unsigned at=0;assert(colorCopyOffset(1280,720,x,y,at));
            std::fill_n(tiled.begin()+16+at,4,0);}
        NativeColorChainExecutionResult result;result.nativeFences=4;
        result.leadingSetupFences=postColorRendering?1:0;
        result.edram=E::capture(half.data(),half.size(),edram.data(),edram.size(),tiled.data(),tiled.size());
        result.copy=NativeTiledCopyPublication::capture(capture.copy().copy,tiled.data(),tiled.size());assert(result.edram && result.copy);
        result.auditedTimeline.emplace();for(unsigned stage=0;stage<4;++stage)
            assert(result.auditedTimeline->begin(stage) && result.auditedTimeline->finishAuditedStage(stage));
        result.eventBytes.fill(0x5A);
        for(std::size_t i=0;i<capture.events().staging.count;++i){const auto& w=capture.events().staging.words[i];
            std::memcpy(result.eventBytes.data()+w.byteOffset,w.bigEndianBytes.data(),4);}
        switch(colorFault){case 3:result.eventBytes[23]^=1;break;case 4:result.edram.reset();break;
            case 5:result.nativeFences=3;break;case 6:result.auditedTimeline.reset();break;case 7:result.copy.reset();break;
            case 10:result.leadingSetupFences=0;break;}
        const bool published=publisher.publish(publisher.context,result);
        if(published && colorFault==11)(void)publisher.publish(publisher.context,result);
        if(published)colorCommitted=true; // Simulated nonthrowing native commit.
        if(published && colorFault==8)return false;
        if(published && colorFault==9)throw std::runtime_error("synthetic post-publication contract violation");
        return published;
    }
    bool completeEmptyGuestCacheCoherency(const NativeInitialCoherency& request) override {
        ++cacheCalls; cacheBefore(); lastCache=request;
        if (throws) throw std::runtime_error("fixture");
        return cacheAccept;
    }
    bool cacheAccept=false;
    unsigned cacheCalls=0;
    NativeInitialCoherency lastCache;
    std::function<void()> cacheBefore=[]{};
    bool stateAccept=false;
    unsigned stateCalls=0;
    bool drainAccept=false;
    bool warmAccept=false;
    unsigned warmCalls=0;
    bool completeWarmRingInitialization(const std::vector<std::uint32_t>& root,
            const std::vector<std::uint32_t>& child) override {
        ++warmCalls;
        NativeInitialCoherency request; assert(parseNativeInitialCoherency(root,child,request));
        if (throws) throw std::runtime_error("fixture");
        return warmAccept;
    }
    unsigned drainCalls=0;
    bool drainForRingReconfiguration() override {
        ++drainCalls;
        if (throws) throw std::runtime_error("fixture");
        return drainAccept;
    }
    std::function<void()> stateBefore=[]{};
    unsigned pairCalls=0;
    bool completeExperimentalIdleCachePair(const std::vector<std::uint32_t>& words, NativeInitializationReadback& output) override {
        ++pairCalls; stateBefore();
        if(throws)throw std::runtime_error("fixture");
        if(!stateAccept)return false;
        NativeStateBatch plan; Pm4Error error;
        assert(planNativeStateBatch(words,plan,error,true) && plan.experimentalIdleCachePair);
        output.byteCount=badSize?8:4;output.draws=badDraws?1:0;output.bytes.fill(0x5A);
        return true;
    }
    bool completeNativeStateBatch(const std::vector<std::uint32_t>& words, NativeInitializationReadback& output) override {
        ++stateCalls; stateBefore();
        if (throws) throw std::runtime_error("fixture");
        if (!stateAccept) return false;
        NativeStateBatch plan; Pm4Error error; assert(planNativeStateBatch(words,plan,error));
        output.byteCount=plan.updates.bufferBytes;
        for (std::size_t i=0;i<plan.updates.count;++i) {
            const auto& w=plan.updates.words[i];
            std::copy(w.bigEndianBytes.begin(),w.bigEndianBytes.end(),output.bytes.begin()+w.byteOffset);
        }
        if (corrupt) output.bytes[0]^=1;
        if (badSize) ++output.byteCount;
        if (badDraws) output.draws=1;
        return true;
    }
    bool completeExperimentalConstantInitialization(const StoppedConstantInitialization& plan,
            NativeInitializationReadback& output) override {
        ++calls; before();
        if (throws) throw std::runtime_error("fixture");
        if (!accept) return false;
        output.byteCount = plan.memory.updates.bufferBytes;
        output.draws = plan.draws.size();
        for (std::size_t i = 0; i < plan.memory.updates.count; ++i) {
            const auto& w = plan.memory.updates.words[i];
            std::copy(w.bigEndianBytes.begin(),w.bigEndianBytes.end(),output.bytes.begin()+w.byteOffset);
        }
        if (corrupt) output.bytes[output.byteCount-1] ^= 1;
        if (badSize) ++output.byteCount;
        if (badDraws) ++output.draws;
        return true;
    }
    std::function<void()> before = []{};
    bool active = false, accept = false, throws = false, corrupt = false, badSize = false, badDraws = false;
    unsigned calls = 0;
};
void testRuntimeCommandStorage(GuestImportRegistry& registry,GuestAddressSpace& space,Backend& backend,
        unsigned ring,unsigned writeBack,unsigned commandStorage) {
    // Fork only the host fixture; parent retains its independent startup/color
    // history. Call once before and once AFTER another real registry consumer.
    std::fflush(nullptr);const auto child=fork();assert(child>=0);
    if(!child) {
        alarm(30);
        const auto put=[&](unsigned a,unsigned v){for(unsigned b=0;b<4;++b)space.translate(a)[b]=v>>(24-8*b);};
        const auto get=[&](unsigned a){const auto*p=space.translate(a);return (unsigned(p[0])<<24)|(unsigned(p[1])<<16)|(unsigned(p[2])<<8)|p[3];};
        const auto previous=registry.graphicsRingConfiguration()->readWord;
        const auto historical=*registry.completedNativePostOutputStorageCoherency();
        const auto color=registry.completedNativePostOutputColorBatch();
        const auto commands=commandStorage+0x3A00;
        const unsigned words[]{0xA31,0x01000000,0x10A2F,0x4000,commandStorage-0xA0000000,
            0xC0043C00,3,0xA31,0,0x80000000,8};
        for(unsigned i=0;i<11;++i)put(commands+4*i,words[i]);
        const auto root=[&](unsigned cursor){put(ring+cursor*4,0xC0013F00);
            put(ring+(cursor+1)*4,commands-0xA0000000);put(ring+(cursor+2)*4,11);};
        root(previous);
        const auto unchanged=[&]{assert(get(writeBack)==previous && registry.graphicsRingConfiguration()->readWord==previous &&
            !registry.completedNativeRuntimeCommandStorageCoherency() &&
            registry.completedNativePostOutputStorageCoherency()->readWord==historical.readWord);};
        for(unsigned cursor:{0u,previous,previous+1,previous+2,previous+4,UINT32_MAX}) {
            assert(!registry.submitNativeRuntimeCommandStorageCoherency(cursor));unchanged();
        }
        unsigned mutations=0;
        for(unsigned i=0;i<14;++i)if(i!=1)for(unsigned bit=0;bit<32;++bit) {
            const auto at=i<3?ring+(previous+i)*4:commands+(i-3)*4,saved=get(at);
            put(at,saved^(1u<<bit));assert(!registry.submitNativeRuntimeCommandStorageCoherency(previous+3));
            put(at,saved);unchanged();++mutations;
        }
        for(unsigned address:{0u,commands-0xA0000000+1,writeBack-0xA0000000,ring-0xA0000000,0x1FFFFFFCu}) {
            put(ring+(previous+1)*4,address);assert(!registry.submitNativeRuntimeCommandStorageCoherency(previous+3));
            root(previous);unchanged();
        }
        put(writeBack,previous^1);assert(!registry.submitNativeRuntimeCommandStorageCoherency(previous+3));put(writeBack,previous);unchanged();
        backend.active=false;assert(!registry.submitNativeRuntimeCommandStorageCoherency(previous+3));backend.active=true;unchanged();
        assert(!backend.runtimeStorageCalls && mutations==416);
        for(unsigned fault=0;fault<5;++fault) {
            std::fflush(nullptr);const auto failed=fork();assert(failed>=0);
            if(!failed) {
                backend.runtimeStorageAccept=fault!=0;backend.runtimeStorageThrow=fault==1;
                if(fault>=2)backend.runtimeStorageBefore=[&]{const unsigned at[]{ring+previous*4,commands,writeBack};put(at[fault-2],get(at[fault-2])^1);};
                assert(!registry.submitNativeRuntimeCommandStorageCoherency(previous+3));assert(fault<2);unchanged();
                backend.runtimeStorageAccept=true;backend.runtimeStorageThrow=false;
                assert(!registry.submitNativeRuntimeCommandStorageCoherency(previous+3) && backend.runtimeStorageCalls==1);
                std::_Exit(0);
            }
            int status=0;assert(waitpid(failed,&status,0)==failed && WIFEXITED(status) && WEXITSTATUS(status)==(fault<2?0:4));
        }
        backend.runtimeStorageAccept=true;
        for(unsigned i=0;i<3;++i) {
            root(previous+3*i);assert(registry.submitNativeRuntimeCommandStorageCoherency(previous+3*i+3));
            const auto now=registry.completedNativeRuntimeCommandStorageCoherency();
            assert(now && now->batches==i+1 && now->firstWord==previous && now->previousWord==previous+3*i &&
                now->readWord==previous+3*i+3 && get(writeBack)==now->readWord && backend.runtimeStorageCalls==i+1);
            assert(registry.completedNativePostOutputStorageCoherency()->readWord==historical.readWord);
            if(color)assert(registry.completedNativePostOutputColorBatch()->color.readWord==color->color.readWord);
            assert(!registry.submitNativeRuntimeCommandStorageCoherency(now->readWord) && backend.runtimeStorageCalls==i+1);
        }
        std::printf("RUNTIME_COMMAND_STORAGE_HOST_PASS prior=%u startup_cursor=%u interleaved_color=%u packet_mutations=416 failures=5 fresh_orders=3 history_preserved=1 GPU=0 gameplay=0\n",
            previous,historical.readWord,unsigned(color.has_value()));std::fflush(stdout);
        registry.reset();assert(!registry.completedNativeRuntimeCommandStorageCoherency());
        assert(!registry.submitNativeRuntimeCommandStorageCoherency(previous+12));
        std::_Exit(0);
    }
    int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
}
void testNativeScalerProducer(GuestImportRegistry& registry,GuestAddressSpace& space,Backend& backend,
        std::uint32_t ring,std::uint32_t writeBack,std::uint32_t commandStorage,std::uint32_t frontbuffer,
        std::function<bool(const GuestImportRegistry::NativeOwnedSwapImage&)>& consumer) {
    constexpr auto Name="__imp__VdInitializeScalerCommandBuffer";
    using Command=NativeScalerCommand;
    const auto put=[&](std::uint32_t at,std::uint32_t value) {for (unsigned b=0;b<4;++b) space.translate(at)[b]=value>>(24-b*8);};
    const auto get=[&](std::uint32_t at) {const auto* p=space.translate(at);
        return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];};
    GuestImportCall free;free.arguments={1,commandStorage};
    assert(!registry.dispatch("__imp__MmFreePhysicalMemory",free).handled()); // Lease begins BEFORE producer.
    GuestImportCall fill;fill.arguments={commandStorage,4,0};
    assert(!registry.dispatch("__imp__RtlFillMemoryUlong",fill).handled());
    const GuestThreadStorageConfig config{{0x64000000,0x100000},{0x4000,0x1000},{0x4000,0x1000},0,0,32,4,0x4000,2};
    const auto pacingMapping=std::make_shared<bool>(true);
    const auto mappingLive=std::make_shared<bool>(true);
    assert(registry.bindThreadStorageArena(config,{},[pacingMapping,mappingLive](auto a){return *mappingLive && (a==0x4000 || (*pacingMapping && a==0x83216908));}));
    const auto worker=registry.createNativeCallbackWorker(0x4000,0,[](const auto&){return true;});assert(worker);
    const auto other=registry.createNativeCallbackWorker(0x4000,0,[](const auto&){return true;});assert(other);
    assert(registry.bindGuestThreadToken(worker->layout.threadAddress));
    const auto sp=worker->layout.stackBase+0x1000;
    std::memset(space.translate(sp-16),0xCD,2496);
    put(sp+84,sp+172);put(sp+92,sp+992);put(sp+100,sp+192);put(sp+108,200);
    for (unsigned base:{156u,172u}) for (unsigned i=0;i<3;++i) put(sp+base+i*4,0);
    const auto record=identityScalerRecord();
    std::memcpy(space.translate(sp+992),record.data(),record.size());
    for (unsigned i=0;i<200;++i) put(sp+192+i*4,0x80000000);
    GuestImportCall call;call.stackPointer=sp;call.returnValue=0xFEDCBA9876543210ULL;
    call.arguments={0,0x050002D0,0,0x050002D0,0x050002D0,7,sp+156,7};
    const std::vector<std::uint8_t> original(space.translate(sp-16),space.translate(sp+2480));
    const auto before=backend.wrapperCalls;
    const auto reject=[&](GuestImportCall request) {
        const auto snapshot=std::vector<std::uint8_t>(space.translate(sp-16),space.translate(sp+2480));
        const auto priorWriteBack=get(writeBack);
        const auto arguments=request.arguments;const auto returnValue=request.returnValue;
        assert(!registry.dispatch(Name,request).handled() && request.arguments==arguments && request.returnValue==returnValue);
        assert(std::equal(snapshot.begin(),snapshot.end(),space.translate(sp-16)));
        assert(!registry.nativeScalerCommandStatus().issued && get(writeBack)==priorWriteBack &&
            registry.graphicsRingConfiguration()->readWord==46 && backend.wrapperCalls==before);
    };
    unsigned argumentBits=0;
    for (unsigned i=0;i<8;++i) for (unsigned bit=0;bit<32;++bit) {
        auto bad=call;bad.arguments[i]^=1U<<bit;reject(bad);++argumentBits;
    }
    for (unsigned i=0;i<4;++i) for (unsigned bit=0;bit<32;++bit) {
        const auto at=sp+84+i*8,saved=get(at);put(at,saved^(1U<<bit));reject(call);put(at,saved);++argumentBits;
    }
    for (unsigned i=0;i<352;++i) {const auto at=sp+992+i*4,saved=get(at);put(at,saved^1);reject(call);put(at,saved);}
    for (unsigned base:{156u,172u}) for (unsigned i=0;i<3;++i) {
        put(sp+base+i*4,0x80000000);reject(call);put(sp+base+i*4,0);
    }
    for (unsigned i=0;i<200;++i) {put(sp+192+i*4,0);reject(call);put(sp+192+i*4,0x80000000);}
    for (auto badSp:{0u,sp+1,0xFFFFFFF0u,ring,worker->layout.stackBase-16,
        worker->layout.stackBase+worker->layout.stackSize-2448,other->layout.stackBase}) {
        auto bad=call;bad.stackPointer=badSp;reject(bad);
    }
    assert(registry.bindGuestThreadToken(other->layout.threadAddress));reject(call);
    assert(registry.bindGuestThreadToken(worker->layout.threadAddress));
    backend.active=false;reject(call);backend.active=true;
    for (auto at:{0x4800u,0x7000u,writeBack}) {const auto saved=get(at);put(at,saved^1);reject(call);put(at,saved);}
    // A registered synchronization object cannot be reinterpreted as stack
    // command scratch. Fork retains the original parent fixture.
    std::fflush(nullptr);const auto sync=fork();assert(sync>=0);
    if (!sync) {GuestImportCall init;init.arguments={sp+192,10000};
        assert(registry.dispatch("__imp__RtlInitializeCriticalSectionAndSpinCount",init).handled());reject(call);std::_Exit(0);}
    int status=0;assert(waitpid(sync,&status,0)==sync && WIFEXITED(status) && WEXITSTATUS(status)==0);
    assert(argumentBits==384 && std::equal(original.begin(),original.end(),space.translate(sp-16)));
    const auto candidate=NativeIdentityScalerCandidate::inspect(identityScalerInputs(),record.data(),record.size());assert(candidate);
    const auto plan=NativeIdentityScalerPlan::make(*candidate);assert(plan);
    const auto command=Command::make({1,46,worker->layout.threadAddress},*plan);assert(command);
    const auto& encoded=command->bytes();
    const auto arguments=call.arguments;
    assert(registry.dispatch(Name,call).handled() && call.returnValue==64 && call.arguments==arguments && call.stackPointer==sp);
    assert(command->matches(space.translate(sp+192),256));
    for (unsigned i=0;i<original.size();++i)
        assert(space.translate(sp-16)[i]==(i>=208 && i<464?command->bytes()[i-208]:original[i]));
    const auto issued=registry.nativeScalerCommandStatus();
    assert(issued.issued && issued.previousReadWord==46 && issued.words==64 && issued.issuerThread==worker->layout.threadAddress);
    call.returnValue=0x123456789ULL;assert(!registry.dispatch(Name,call).handled() && call.returnValue==0x123456789ULL);
    // Temporary producer stack may be reused. It is not the execution payload.
    std::memset(space.translate(sp),0,2464);
    assert(registry.nativeScalerCommandStatus().issued && get(writeBack)==46 &&
        registry.graphicsRingConfiguration()->readWord==46 && backend.wrapperCalls==before);
    std::puts("NATIVE_SCALER_PRODUCER:384argument bits,352record words,200prefill words,owned worker/aliases/canaries/one-shot PASS;64DWORD generated, GPU work=0");
    const auto phys=[](std::uint32_t a){return a-0xA0000000;};
    const auto child=commandStorage+0x3D00,ticket=commandStorage+0x3200;
    const std::array<std::uint32_t,11> childWords{0xA31,0x01000000,0x10A2F,0x4000,phys(commandStorage),
        0xC0043C00,3,0xA31,0,0x80000000,8};
    put(ring+46*4,0xC0013F00);put(ring+47*4,phys(child));put(ring+48*4,11);
    for (unsigned i=0;i<11;++i) put(child+i*4,childWords[i]);
    const auto unchanged=[&] {
        assert(get(writeBack)==46 && registry.graphicsRingConfiguration()->readWord==46 &&
            registry.nativeScalerCommandStatus().issued && !registry.completedNativeScalerStorageCoherency() &&
            registry.nativeSwapCommandStatus().readWord==46 && backend.wrapperCalls==before);
    };
    const auto rejectFence=[&](unsigned next=49) {
        const auto calls=backend.scalerStorageCalls;
        assert(!registry.submitNativeScalerStorageCoherency(next) && backend.scalerStorageCalls==calls);unchanged();
    };
    rejectFence(); // Stack generation alone is not a copied owned command.
    std::memcpy(space.translate(ticket),encoded.data(),Command::Bytes);
    for (auto next:{0u,46u,47u,48u,50u,UINT32_MAX}) rejectFence(next);
    unsigned packetBits=0;
    for (unsigned at:{ring+46*4,ring+47*4,ring+48*4}) for (unsigned bit=0;bit<32;++bit) {
        const auto saved=get(at);put(at,saved^(1U<<bit));rejectFence();put(at,saved);++packetBits;
    }
    for (unsigned i=0;i<11;++i) for (unsigned bit=0;bit<32;++bit) {
        const auto at=child+i*4,saved=get(at);put(at,saved^(1U<<bit));rejectFence();put(at,saved);++packetBits;
    }
    for (unsigned i=0;i<Command::Bytes;++i) for (unsigned bit=0;bit<8;++bit) {
        space.translate(ticket)[i]^=1U<<bit;rejectFence();space.translate(ticket)[i]^=1U<<bit;
    }
    std::memcpy(space.translate(ticket+256),encoded.data(),Command::Bytes);rejectFence();
    std::memset(space.translate(ticket+256),0,Command::Bytes);
    for (auto at:{0x4800u,0x7000u}) {const auto saved=get(at);put(at,saved^1);rejectFence();put(at,saved);}
    backend.active=false;rejectFence();backend.active=true;
    assert(!registry.submitNativeScalerStorageCoherency(49) && backend.scalerStorageCalls==1);unchanged();
    backend.scalerStorageThrow=true;
    assert(!registry.submitNativeScalerStorageCoherency(49) && backend.scalerStorageCalls==2);unchanged();
    backend.scalerStorageThrow=false;backend.scalerStorageAccept=true;
    unsigned failStops=0;
    for (auto at:{ring+46*4,child,ticket,writeBack,0x4800u,0x7000u,ticket+256}) {
        std::fflush(nullptr);const auto violation=fork();assert(violation>=0);
        if (!violation) {backend.scalerStorageBefore=[&] {
            if (at==ticket+256) std::memcpy(space.translate(at),encoded.data(),256);
            else put(at,get(at)^1);
        };(void)registry.submitNativeScalerStorageCoherency(49);std::_Exit(99);}
        int result=0;assert(waitpid(violation,&result,0)==violation && WIFEXITED(result) && WEXITSTATUS(result)==4);
        unchanged();++failStops;
    }
    assert(registry.submitNativeScalerStorageCoherency(49) && backend.scalerStorageCalls==3);
    const auto fence=registry.completedNativeScalerStorageCoherency();assert(fence);
    assert(fence->previousWord==46 && fence->readWord==49 && fence->commandPhysical==phys(ticket) &&
        fence->request.physicalBase==phys(commandStorage) && fence->request.byteCount==0x4000 &&
        fence->request.cacheFlags==0x01000000 && get(writeBack)==49 &&
        registry.graphicsRingConfiguration()->readWord==49 && registry.nativeSwapCommandStatus().readWord==46);
    assert(!registry.submitNativeScalerStorageCoherency(52) && backend.scalerStorageCalls==3);
    assert(!registry.dispatch("__imp__MmFreePhysicalMemory",free).handled() && command->matches(space.translate(ticket),256));
    const auto wrapper=ticket-69*4;
    assert(!registry.inspectStoppedNativeScalerSubmission(52));
    put(ring+49*4,0xC0013F00);put(ring+50*4,phys(wrapper));put(ring+51*4,140);
    const auto inspected=registry.inspectStoppedNativeScalerSubmission(52);assert(inspected);
    assert(inspected->rootPhysical==phys(ring)+49*4 && inspected->childPhysical==phys(wrapper) && inspected->childWords==140 &&
        inspected->commandOffsetWords==69 && inspected->readWord==49 && inspected->writeWord==52);
    for (unsigned bit=0;bit<32;++bit) {put(ring+49*4,0xC0013F00^(1U<<bit));
        assert(!registry.inspectStoppedNativeScalerSubmission(52));}put(ring+49*4,0xC0013F00);
    for (unsigned words:{0u,63u,64u,132u,1025u,UINT32_MAX}) {put(ring+51*4,words);
        assert(!registry.inspectStoppedNativeScalerSubmission(52));}put(ring+51*4,140);
    for (auto address:{phys(wrapper)+1,phys(ticket)+4,0x20000000u}) {put(ring+50*4,address);
        assert(!registry.inspectStoppedNativeScalerSubmission(52));}put(ring+50*4,phys(wrapper));
    for (auto at:{ticket,writeBack}) {const auto saved=get(at);put(at,saved^1);
        assert(!registry.inspectStoppedNativeScalerSubmission(52));put(at,saved);}
    assert(registry.inspectStoppedNativeScalerSubmission(52) && get(writeBack)==49 && backend.scalerStorageCalls==3 &&
        backend.wrapperCalls==before && command->matches(space.translate(ticket),256));
    std::puts("NATIVE_SCALER_WRAPPER_INSPECTION:owned bounded root/child,32header bits,copy identity,no execution PASS");
    assert(!registry.inspectStoppedNativeScalerWrapper(52));
    const auto wrapperBytes=scalerWrapperBytes(*command,0x20000,0x80004000,0x1234);
    std::memcpy(space.translate(wrapper),wrapperBytes.data(),wrapperBytes.size());put(ring+51*4,NativeScalerWrapper::Words);
    const auto full=registry.inspectStoppedNativeScalerWrapper(52);assert(full && full->callbackAddress()==0x80004000);
    for (unsigned at:{wrapper,wrapper+35*4,wrapper+59*4,ticket,wrapper+143*4,writeBack,ring+51*4}) {
        const auto saved=get(at);put(at,saved^1);assert(!registry.inspectStoppedNativeScalerWrapper(52));put(at,saved);
    }
    assert(registry.inspectStoppedNativeScalerWrapper(52) && get(writeBack)==49 && backend.scalerStorageCalls==3 && backend.wrapperCalls==before);
    std::memset(space.translate(wrapper),0,wrapperBytes.size());
    assert(full->callbackContext()==0x1234 && full->command().matches(command->bytes().data(),256));
    std::puts("NATIVE_SCALER_FULL_WRAPPER_INSPECTION:owned plan,7refusals,no callbacks or writes PASS");
    unsigned enqueues=0;
    const auto forbidden=[&](const auto&,const auto&) {++enqueues;return false;};
    assert(!registry.runNativeCommandInterruptScope(52,[]{assert(false);}));
    assert(!registry.createNativeCommandInterruptWorker(52,forbidden));
    assert(registry.bindThreadResumeHandler([](const auto&,auto) {return false;}));
    assert(registry.bindVirtualCpuAffinity());
    put(0x7100,0);put(0x7104,0x6000);
    GuestImportCall deviceAllocation;deviceAllocation.arguments={0x7100,0x7104,0x3000,4,0};
    assert(registry.dispatch("__imp__NtAllocateVirtualMemory",deviceAllocation).handled() && deviceAllocation.returnValue==0);
    const auto device=get(0x7100);assert(device==0x10000000);
    GuestImportCall registration;registration.arguments={0x4000,device};
    assert(registry.dispatch("__imp__VdSetGraphicsInterruptCallback",registration).handled());
    assert(!registry.createNativeCommandInterruptWorker(52,forbidden)); // Cleared wrapper.
    std::memcpy(space.translate(wrapper),wrapperBytes.data(),wrapperBytes.size());
    assert(!registry.createNativeCommandInterruptWorker(52,forbidden)); // Posted function lacks exact mapping.
    const auto controlPhysical=phys(*registry.graphicsSystemIdentifierAddress());
    const auto irqBytes=scalerWrapperBytes(*command,phys(writeBack)+0x40,0x4000,0x208,
        {controlPhysical+0x100,controlPhysical+0x104});
    std::memcpy(space.translate(wrapper),irqBytes.data(),irqBytes.size());
    assert(!registry.createNativeCommandInterruptWorker(52,{}));
    assert(!registry.createNativeCommandInterruptWorker(55,forbidden));
    assert(registry.bindGuestThreadToken(other->layout.threadAddress));
    assert(!registry.createNativeCommandInterruptWorker(52,forbidden));
    assert(registry.bindGuestThreadToken(worker->layout.threadAddress));
    const auto id=get(worker->layout.threadAddress+0x14C);put(worker->layout.threadAddress+0x14C,id^1);
    assert(!registry.createNativeCommandInterruptWorker(52,forbidden));put(worker->layout.threadAddress+0x14C,id);
    assert(enqueues==0);
    const auto priorCount=registry.createdThreadCount();
    for (bool throws:{false,true}) {
        GuestPreparedThread attempted;bool caught=false;
        try {assert(!registry.createNativeCommandInterruptWorker(52,[&](const auto& thread,const auto& entry)->bool {
            attempted=thread;++enqueues;
            assert(entry.directEntry && entry.address==0x4000 && entry.argument0==1 && entry.argument1==device);
            assert(!thread.wrappedContextAddress && thread.startContext==device && thread.creationFlags==0);
            assert(*space.translate(thread.layout.pcrAddress+0x10C)==2 && *space.translate(thread.layout.threadAddress+0xBF)==2);
            assert(thread.layout.pcrAddress!=worker->layout.pcrAddress && thread.layout.stackBase!=worker->layout.stackBase);
            if (throws) throw std::runtime_error("fixture interrupt enqueue");return false;
        }));} catch (const std::runtime_error&) {caught=true;}
        assert(caught==throws && registry.createdThreadCount()==priorCount && attempted.allocation.size);
        assert(std::all_of(space.translate(attempted.allocation.base),space.translate(attempted.allocation.base)+attempted.allocation.size,
            [](auto value){return value==0;}));
    }
    const auto interrupt=registry.createNativeCommandInterruptWorker(52,[&](const auto& thread,const auto& entry) {
        ++enqueues;assert(entry.argument0==1 && entry.argument1==device && entry.pcrAddress==thread.layout.pcrAddress);return true;
    });assert(interrupt && enqueues==3 && registry.createdThreadCount()==priorCount+1);
    assert(!registry.createNativeCommandInterruptWorker(52,forbidden) && enqueues==3);
    GuestImportCall affinity;affinity.arguments={interrupt->layout.threadAddress,4,0x7004};
    assert(registry.dispatch("__imp__KeSetAffinityThread",affinity).handled() && get(0x7004)==4);
    assert(get(writeBack)==49 && backend.scalerStorageCalls==3 && backend.wrapperCalls==before);
    assert(std::memcmp(space.translate(wrapper),irqBytes.data(),irqBytes.size())==0);
    std::puts("NATIVE_COMMAND_INTERRUPT_WORKER: CPU2/direct source-context ABI,issuer and mapping checks,2rollbacks,one-shot PASS; enqueue only");
    const std::array<const char*,4> lockImports{"__imp__KfAcquireSpinLock","__imp__KfReleaseSpinLock",
        "__imp__KeAcquireSpinLockAtRaisedIrql","__imp__KeReleaseSpinLockFromRaisedIrql"};
    for (auto name:lockImports) {GuestImportCall c;c.arguments={device+17120,2};c.returnValue=0xFACE;
        assert(!registry.dispatch(name,c).handled() && c.returnValue==0xFACE);}
    assert(!registry.runNativeCommandInterruptScope(52,[]{assert(false);})); // Issuer is not IRQ worker.
    assert(registry.nativeScalerInterruptStatus().rejection==8);
    assert(registry.bindGuestThreadToken(interrupt->layout.threadAddress));
    assert(!registry.runNativeCommandInterruptScope(52,[]{assert(false);})); // Scratch pointer missing.
    assert(registry.nativeScalerInterruptStatus().rejection==14);
    put(device+11028,writeBack+0x40);
    const std::array<std::pair<unsigned,unsigned>,5> scopeRejections{{{device+11028,14},{device+11032,15},
        {device+17120,16},{interrupt->layout.threadAddress+0x14C,9},{ring+51*4,10}}};
    for (auto [at,reason]:scopeRejections) {
        const auto saved=get(at);put(at,saved^1);
        assert(!registry.runNativeCommandInterruptScope(52,[]{assert(false);}) && registry.nativeScalerInterruptStatus().rejection==reason);
        put(at,saved);
    }
    *space.translate(interrupt->layout.pcrAddress+0x10C)=1;
    assert(!registry.runNativeCommandInterruptScope(52,[]{assert(false);}));
    assert(registry.nativeScalerInterruptStatus().rejection==17);
    *space.translate(interrupt->layout.pcrAddress+0x10C)=2;
    assert(!registry.nativeCommandInterruptScopeStatus().attempted);
    const auto invokeLock=[&](unsigned operation,unsigned address,bool accepted,unsigned old=2) {
        GuestImportCall c;c.arguments={address,old};c.returnValue=0xFEDCBA9876543210ULL;
        assert(registry.dispatch(lockImports[operation],c).handled()==accepted);
        assert(c.returnValue==(accepted && operation==0?2:0xFEDCBA9876543210ULL));
    };
    const auto balancedLocks=[&] {
        invokeLock(0,device+17120,true);invokeLock(1,device+17120,true);
        invokeLock(2,device+11032,true);invokeLock(3,device+11032,true);
    };
    // The device also owns unrelated synchronization fields. Do not treat the
    // entire object as IRQ write scratch; do reject even partial header overlap.
    const std::array<std::pair<unsigned,unsigned>,5> touched{{
        {device+11028,8},{device+16944,180},{device+21964,4},{device+23788,4},{writeBack+0x40,32}}};
    unsigned synchronizationControls=0;
    for (unsigned type=0;type<2;++type) for (unsigned range=0;range<=touched.size();++range) {
        const bool unrelated=range==touched.size();
        for (unsigned placement=0;placement<(unrelated?1u:3u);++placement) {
            const auto bytes=type?28u:16u;
            const auto address=unrelated?device+512:placement==0?touched[range].first-bytes+4:
                placement==1?touched[range].first:touched[range].first+touched[range].second-4;
            std::fflush(nullptr);const auto child=fork();assert(child>=0);
            if (!child) {alarm(5);
                assert(registry.bindGuestThreadToken(other->layout.threadAddress));
                if (!type) {
                    put(address,0);put(address+4,0);put(address+8,address+8);put(address+12,address+8);
                    const std::vector<std::uint8_t> snapshot(space.translate(address),space.translate(address)+16);
                    bool parked=false;
                    assert(registry.bindThreadWaitHandler([&](const auto& ready) {
                        parked=true;assert(!ready());
                        assert(registry.bindGuestThreadToken(interrupt->layout.threadAddress));
                        assert(registry.runNativeCommandInterruptScope(52,balancedLocks)==unrelated);
                        assert(unrelated || (registry.nativeScalerInterruptStatus().rejection==13 &&
                            !registry.nativeCommandInterruptScopeStatus().attempted));
                        assert(std::equal(snapshot.begin(),snapshot.end(),space.translate(address)));
                        assert(!ready()); // IRQ neither signals nor consumes the unrelated wait.
                        assert(registry.bindGuestThreadToken(other->layout.threadAddress));
                        GuestImportCall signal;signal.arguments={address,0,0};
                        assert(registry.dispatch("__imp__KeSetEvent",signal).handled());
                        return ready();
                    }));
                    GuestImportCall wait;wait.arguments={address,3,1,0,0};
                    assert(registry.dispatch("__imp__KeWaitForSingleObject",wait).handled() && wait.returnValue==0 && parked);
                } else {
                    GuestImportCall cs;cs.arguments={address,0};
                    assert(registry.dispatch("__imp__RtlInitializeCriticalSectionAndSpinCount",cs).handled());
                    assert(registry.dispatch("__imp__RtlEnterCriticalSection",cs).handled());
                    const std::vector<std::uint8_t> snapshot(space.translate(address),space.translate(address)+28);
                    assert(registry.bindGuestThreadToken(interrupt->layout.threadAddress));
                    assert(registry.runNativeCommandInterruptScope(52,balancedLocks)==unrelated);
                    assert(unrelated || (registry.nativeScalerInterruptStatus().rejection==12 &&
                        !registry.nativeCommandInterruptScopeStatus().attempted));
                    assert(std::equal(snapshot.begin(),snapshot.end(),space.translate(address)));
                    assert(registry.bindGuestThreadToken(other->layout.threadAddress));
                    assert(registry.dispatch("__imp__RtlLeaveCriticalSection",cs).handled());
                }
                std::_Exit(0);
            } int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
            ++synchronizationControls;
        }
    }
    assert(synchronizationControls==32);
    std::puts("NATIVE_INTERRUPT_FIELD_EXCLUSION:30full/partial event-lock overlaps refused,2unrelated parked/held objects preserved PASS");
    assert(!registry.beginNativeScalerInterrupt(52) && registry.nativeScalerInterruptStatus().rejection==1);
    put(0x4868,0x7200);put(0x7200,device); // Dedicated exported pointer storage, not heap storage.
    for(unsigned fault=0;fault<10;++fault) {
        std::fflush(nullptr);const auto child=fork();assert(child>=0);
        if(!child) {
            alarm(10);put(device+13600,0);put(device+11152,1);put(device+11156,0);
            *space.translate(device+11072)=0;put(device+11080+112,0);
            for(unsigned cpu=0;cpu<6;++cpu)put(device+11592+cpu*108,0);
            if(fault==1)put(device+13600,1);
            if(fault==2)put(device+11152,0);
            if(fault==3)*space.translate(device+11072)=8;
            if(fault==4)put(device+11592+3*108,0xA0010000);
            if(fault==5)put(device+11080+112,1);
            GuestCooperativeExecutor executor;unsigned invoked=0;
            const auto outcome=executor.run(worker->layout.threadAddress,[&] {
                const auto kind=fault==6?static_cast<NativeTileLockCall>(99):
                    (fault==4 || fault==5 || fault==9?NativeTileLockCall::OrdinaryWorker:NativeTileLockCall::PendingProducer);
                GuestImportRegistry::NativeSerializedGate gate=[&](const auto& f){return executor.runNativeNonYielding(f,32);};
                if(fault==7)gate={};
                const auto result=invokeNativeTileLockCall(registry,gate,fault==8?device+0x10000:device,kind,[&] {
                    ++invoked;GuestExecutionGuard::checkpoint("original_tile_adapter_fixture");
                    GuestImportCall c;c.arguments={device+11156,0};
                    assert(registry.dispatch("__imp__KfAcquireSpinLock",c).handled() && !c.returnValue);
                    assert(registry.dispatch("__imp__KfReleaseSpinLock",c).handled());
                });
                const bool returns=fault==0 || fault==9;
                assert(invoked==unsigned(returns));
                assert(result==(returns?NativeTileLockCallResult::Returned:
                    fault==1 || fault==2?NativeTileLockCallResult::NotRequired:NativeTileLockCallResult::Refused));
                assert(!get(device+11156));
            },{64,1,2},[&](auto token){assert(registry.bindGuestThreadToken(token));});
            assert(outcome.execution.completed());std::_Exit(0);
        }
        int result=0;assert(waitpid(child,&result,0)==child && WIFEXITED(result) && !WEXITSTATUS(result));
    }
    std::puts("NATIVE_TILE_LOCK_CALL_ADAPTER_PASS cases=10 producer_worker_connected=1 unsupported_event_drain_refused_before_original=1 no_completion=1");
    // Ordinary producer/worker locks use a separate level0 scope, never the
    // dedicated source1 worker. Actual executor gate parks a queued peer.
    for(unsigned fault=0;fault<14;++fault) {
        std::fflush(nullptr);const auto child=fork();assert(child>=0);
        if(!child) {
            alarm(10);put(device+11156,0);
            GuestCooperativeExecutor executor;unsigned peers=0,invoked=0;bool threw=false;
            const auto token=worker->layout.threadAddress;
            const auto pcr=worker->layout.pcrAddress;
            const std::vector<std::uint8_t> oldPcr(space.translate(pcr),space.translate(pcr)+272);
            const auto cpu=*space.translate(token+0xBF);
            assert(!registry.runNativeTileLockScope(device,[&](const auto& f){return executor.runNativeNonYielding(f,32);},[]{assert(false);}));
            const auto outcome=executor.run(token,[&] {
                assert(executor.enqueue(other->layout.threadAddress,[&]{++peers;}));
                if(fault==11)put(0x7200,device^4);
                if(fault==12)put(device+11156,1);
                const auto lockCall=[&](const char* name,unsigned address,unsigned old,bool expected) {
                    GuestImportCall c;c.arguments={address,old};c.returnValue=0xFEDCBA9876543210ULL;
                    assert(registry.dispatch(name,c).handled()==expected);
                    assert(c.returnValue==(expected && std::string_view(name)=="__imp__KfAcquireSpinLock"?0:0xFEDCBA9876543210ULL));
                };
                bool balanced=false;
                try {
                    balanced=registry.runNativeTileLockScope(device,[&](const auto& f){return executor.runNativeNonYielding(f,32);},[&] {
                        ++invoked;GuestExecutionGuard::checkpoint("ordinary_tile_scope");assert(!peers);
                        assert(!registry.runNativeTileLockScope(device,[&](const auto& f){return executor.runNativeNonYielding(f,32);},[]{assert(false);}));
                        if(fault==1)return;
                        if(fault==2){lockCall("__imp__KfAcquireSpinLock",device+11160,0,false);return;}
                        if(fault==5){lockCall("__imp__KfReleaseSpinLock",device+11156,0,false);return;}
                        if(fault==13){lockCall("__imp__KeAcquireSpinLockAtRaisedIrql",device+11156,0,false);return;}
                        lockCall("__imp__KfAcquireSpinLock",device+11156,0,true);assert(get(device+11156)==1);
                        GuestExecutionGuard::checkpoint("ordinary_tile_held");assert(!peers);
                        if(fault==10)throw std::runtime_error("ordinary lock unwind fixture");
                        if(fault==4){lockCall("__imp__KfAcquireSpinLock",device+11156,0,false);return;}
                        if(fault==6){lockCall("__imp__KeReleaseSpinLockFromRaisedIrql",device+11156,0,false);return;}
                        if(fault==7)put(device+11156,2);
                        if(fault==8)assert(registry.bindGuestThreadToken(other->layout.threadAddress));
                        if(fault==9)put(0x7200,device^4);
                        lockCall("__imp__KfReleaseSpinLock",device+11156,fault==3?2:0,fault==0);
                    });
                }catch(const std::runtime_error&){threw=true;}
                assert(balanced==(fault==0) && threw==(fault==10) && !peers);
                assert(invoked==(fault==11 || fault==12?0u:1u));
                const auto status=registry.nativeTileLockScopeStatus();
                if(invoked)assert(status.attempted && status.balanced==(fault==0) && status.returned==(fault!=10));
                const auto expected=fault==7?2u:(fault==3 || fault==4 || fault==6 || fault==8 || fault==9 || fault==10 || fault==12?1u:0u);
                assert(get(device+11156)==expected); // Failure never force-unlocks.
                assert(registry.bindGuestThreadToken(token));
            },{128,1,4},[&](auto token){assert(registry.bindGuestThreadToken(token));});
            assert(outcome.execution.completed() && peers==1);
            assert(std::equal(oldPcr.begin(),oldPcr.end(),space.translate(pcr)) && *space.translate(token+0xBF)==cpu);
            std::_Exit(0);
        }
        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    }
    assert(!registry.runNativeTileLockScope(device,[](const auto& f){f();return true;},[]{assert(false);})); // IRQ worker excluded.
    std::puts("NATIVE_ORDINARY_TILE_LOCK_SCOPE_PASS cases=14 real_executor_exclusion=1 paired_saved_level0=1 irq_scope_distinct=1 no_force_unlock=1 pcr_unchanged=1");
    assert(registry.bindClockSources([] {return std::optional<std::uint64_t>{1000};},50000000));
    put(writeBack+0x40,0);put(writeBack+0x44,0);put(writeBack+0x48,0xABCD);put(writeBack+0x50,0x0BADF00D);
    // Exact reviewed optional callback needs owned writable image data, not
    // merely a mapped function. Synthetic preflight only; no game code here.
    for (unsigned variant=0;variant<6;++variant) {std::fflush(nullptr);const auto child=fork();assert(child>=0);
        if (!child) {alarm(5);
            constexpr unsigned header=0x7400,global=0x83BFBE40;
            if (variant!=0) {
                std::memset(space.translate(header),0,0x400);
                put(header,0x58455832);put(header+8,0x400);put(header+16,0x80);
                put(header+0x80,0x184+24);put(header+0x84,0x10000);
                put(header+0x190,0x83BF0000);put(header+0x200,1);put(header+0x204,variant==1?0x13:0x12);
                assert(registry.bindImagePageProtections(header,0x400,0x83BF0000,0x10000));
            }
            put(device+16944,0x83216908);
            if (variant==2) *pacingMapping=false;
            if (variant==3 || variant==4) {
                assert(registry.bindGuestThreadToken(other->layout.threadAddress));
                GuestImportCall cs;cs.arguments={variant==3?global-24:global+20,0};
                assert(registry.dispatch("__imp__RtlInitializeCriticalSectionAndSpinCount",cs).handled());
                assert(registry.dispatch("__imp__RtlEnterCriticalSection",cs).handled());
                assert(registry.bindGuestThreadToken(interrupt->layout.threadAddress));
            }
            assert(registry.runNativeCommandInterruptScope(52,[&] {
                if (variant==5) {
                    for (unsigned bit=0;bit<32;++bit) {put(device+16944,0x83216908^(1u<<bit));
                        assert(!registry.beginNativeScalerInterrupt(52) && registry.nativeScalerInterruptStatus().rejection==5);}
                    put(device+16944,0x83216908);backend.irqAccept=true;
                }
                assert(registry.beginNativeScalerInterrupt(52)==(variant==5));
                assert(variant==5 || (registry.nativeScalerInterruptStatus().rejection==5 && !backend.irqFences));
                balancedLocks();
            }));
            std::_Exit(0);
        }int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    }
    std::puts("NATIVE_PACING_CALLBACK_ADMISSION: exact leaf,32target bits,missing/readonly image,missing map,2partial lock overlaps PASS; CPU fixture only");
    // Each terminal experiment starts from the same pristine worker and lease.
    for (unsigned failure=0;failure<5;++failure) {std::fflush(nullptr);const auto child=fork();assert(child>=0);
        if (!child) {alarm(5);bool caught=false;try {
            const auto result=registry.runNativeCommandInterruptScope(52,[&] {
                if (failure==0) throw std::runtime_error("before lock");
                invokeLock(0,device+17120,true);
                if (failure==1) throw std::runtime_error("held lock");
                if (failure==2) return;
                invokeLock(1,device+17120,true);
                invokeLock(2,device+11032,true);invokeLock(3,device+11032,true);
                if (failure==3) put(device+11032,1);
                if (failure==4) *space.translate(interrupt->layout.pcrAddress+0x10C)=0;
            });assert(!result);
        } catch (const std::runtime_error&) {caught=true;}
            assert(caught==(failure<2));const auto status=registry.nativeCommandInterruptScopeStatus();
            assert(status.attempted && status.returned==(failure>=2) && !status.balanced);
            assert(status.heldLocks==((failure==1 || failure==2)?2u:0u));
            assert(get(device+17120)==((failure==1 || failure==2)?1u:0u));
            assert(!registry.runNativeCommandInterruptScope(52,[]{assert(false);}));
            invokeLock(1,device+17120,false);std::_Exit(0);
        } int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    }
    assert(registry.runNativeCommandInterruptScope(52,[&] {
        assert(!registry.runNativeCommandInterruptScope(52,[]{assert(false);}));
        GuestImportCall forbiddenImport;forbiddenImport.arguments={device+17120,4,0};
        assert(!registry.dispatch("__imp__RtlFillMemoryUlong",forbiddenImport).handled());
        assert(!registry.dispatch("__imp__KeWaitForSingleObject",forbiddenImport).handled());
        GuestImportCall query;assert(registry.dispatch("__imp__KeGetCurrentProcessType",query).handled() && query.returnValue==1);
        put(0x7200,device^4);assert(!registry.beginNativeScalerInterrupt(52) && registry.nativeScalerInterruptStatus().rejection==4);
        put(0x7200,device);put(device+16972,1);
        assert(!registry.beginNativeScalerInterrupt(52) && registry.nativeScalerInterruptStatus().rejection==5);put(device+16972,0);
        put(device+21964,50000001);assert(!registry.beginNativeScalerInterrupt(52));put(device+21964,0);
        put(device+16944,0x4000);assert(!registry.beginNativeScalerInterrupt(52) && registry.nativeScalerInterruptStatus().rejection==5);
        assert((registry.nativeScalerInterruptStatus().pacing==std::array<std::uint64_t,6>{1,0,0,0x4000,0x208,50000000}));
        put(device+16944,0);
        put(wrapper+14*4,0x1234);put(wrapper+26*4,0x1234);
        assert(!registry.beginNativeScalerInterrupt(52) && registry.nativeScalerInterruptStatus().rejection==5);
        put(wrapper+14*4,0x208);put(wrapper+26*4,0x208);
        put(writeBack+0x40,1);assert(!registry.beginNativeScalerInterrupt(52));put(writeBack+0x40,0);
        backend.irqAccept=true;
        assert(registry.beginNativeScalerInterrupt(52) && !registry.beginNativeScalerInterrupt(52));
        assert((registry.nativeScalerInterruptStatus().pacing==std::array<std::uint64_t,6>{1,0,0,0,0x208,50000000}));
        assert(backend.irqFences==1 && !backend.irqAcks && get(writeBack+0x40)==4 && get(writeBack+0x44)==1 &&
            get(writeBack+0x50)==0x4000 && get(writeBack+0x54)==0x208 && get(writeBack+0x48)==0xABCD);
        assert(!registry.acknowledgeNativeScalerInterrupt(52)); // Handler has not returned.
        invokeLock(1,device+17120,false);invokeLock(0,device+11032,false);
        invokeLock(0,device+17120+1,false);invokeLock(2,0x7FC80000,false);
        invokeLock(0,device+17120,true);assert(get(device+17120)==1 && space.translate(device+17120)[3]==1);
        invokeLock(0,device+17120,false);invokeLock(2,device+11032,false);invokeLock(1,device+17120,false,0);
        assert(registry.bindGuestThreadToken(other->layout.threadAddress));invokeLock(1,device+17120,false);
        assert(registry.bindGuestThreadToken(interrupt->layout.threadAddress));
        invokeLock(1,device+17120,true);invokeLock(0,device+17120,false);
        invokeLock(2,device+11032,true);invokeLock(3,device+17120,false);invokeLock(3,device+11032,true);
        // Stand-in for the title's real handler in this CPU-only fixture.
        put(writeBack+0x40,0);put(writeBack+0x44,0);
    }));
    const auto scopeStatus=registry.nativeCommandInterruptScopeStatus();
    assert(scopeStatus.attempted && scopeStatus.returned && scopeStatus.balanced && !scopeStatus.heldLocks &&
        scopeStatus.worker==interrupt->layout.threadAddress && (scopeStatus.lockPairs==std::array<std::uint32_t,2>{1,1}));
    assert(!registry.runNativeCommandInterruptScope(52,[]{assert(false);}));
    for (unsigned address:{writeBack+0x40,writeBack+0x44,writeBack+0x48,writeBack+0x50,wrapper+143*4,ring+51*4}) {
        std::fflush(nullptr);const auto child=fork();assert(child>=0);
        if (!child) {alarm(5);put(address,get(address)^1);
            assert(!registry.acknowledgeNativeScalerInterrupt(52) && !registry.nativeScalerInterruptStatus().acknowledged && !backend.irqAcks);
            put(address,get(address)^1);assert(!registry.acknowledgeNativeScalerInterrupt(52));std::_Exit(0);
        }int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    }
    assert(registry.acknowledgeNativeScalerInterrupt(52) && !registry.acknowledgeNativeScalerInterrupt(52));
    assert(registry.nativeScalerInterruptStatus().acknowledged && backend.irqAcks==1 &&
        get(writeBack+0x50)==0x0BADF00D && get(writeBack+0x48)==0xABCD && !get(writeBack+0x40) && !get(writeBack+0x44));
    assert(!registry.completeNativeScalerWrapper(52)); // IRQ worker is not the original issuer.
    assert(!registry.submitNativePostScalerStorageCoherency(52) && !backend.postStorageCalls);
    std::fflush(nullptr);const auto tailChild=fork();assert(tailChild>=0);
    if (!tailChild) {alarm(30); // D303 UBSan hit SIGALRM at20s; aggregate only, leaf bounds stay5s.
        assert(registry.bindGuestThreadToken(worker->layout.threadAddress));
        assert(!registry.completeNativeScalerWrapper(52)); // IRQ worker must have returned.
        assert(registry.bindGuestThreadToken(interrupt->layout.threadAddress));
        assert(registry.markGuestThreadReturned(interrupt->layout.threadAddress,0));
        assert(registry.bindGuestThreadToken(worker->layout.threadAddress));
        for (unsigned word:{0u,49u,50u,51u,53u,UINT32_MAX}) assert(!registry.completeNativeScalerWrapper(word));
        for (unsigned at:{ring+49*4,wrapper,ticket,writeBack,writeBack+0x44,writeBack+0x50}) {
            const auto saved=get(at);put(at,saved^1);assert(!registry.completeNativeScalerWrapper(52));put(at,saved);
        }
        assert(!backend.tailCalls && !registry.completedNativeScalerWrapper());
        const auto event0=*registry.graphicsSystemIdentifierAddress()+0x100,event1=event0+4;
        const auto old0=get(event0),old1=get(event1);
        for (unsigned fault=0;fault<10;++fault) {std::fflush(nullptr);const auto failed=fork();assert(failed>=0);
            if (!failed) {backend.tailAccept=fault!=0;backend.tailFault=fault<6?fault:0;
                if (fault>=6) backend.tailBefore=[&]{const std::array<unsigned,4> at{ring+49*4,ticket,event0,writeBack+0x48};put(at[fault-6],get(at[fault-6])^1);};
                assert(!registry.completeNativeScalerWrapper(52));
                assert(fault<2 && !registry.completedNativeScalerWrapper() && get(writeBack)==49 && get(event0)==old0 && get(event1)==old1);
                backend.tailAccept=true;backend.tailFault=0;
                assert(!registry.completeNativeScalerWrapper(52) && backend.tailCalls==1);std::_Exit(0);
            }int status=0;assert(waitpid(failed,&status,0)==failed && WIFEXITED(status) && WEXITSTATUS(status)==(fault<2?0:4));
        }
        backend.tailAccept=true;assert(registry.completeNativeScalerWrapper(52));
        const auto done=registry.completedNativeScalerWrapper();assert(done && done->previousWord==49 && done->readWord==52 &&
            done->gpu.nativeFences==3 && done->gpu.configured && done->eventPhysical[0]==controlPhysical+0x100 &&
            get(event0)==0x123 && get(event1)==0xABC && get(writeBack)==52 && registry.graphicsRingConfiguration()->readWord==52);
        assert(!registry.completeNativeScalerWrapper(52) && backend.tailCalls==1);
        // A fresh no-draw command-memory fence AFTER the scaler, not reuse of
        // its old completion. CPU-only mock verifies registry publication.
        put(ring+52*4,0xC0013F00);put(ring+53*4,phys(child));put(ring+54*4,11);
        for (unsigned i=0;i<11;++i) put(child+i*4,childWords[i]);
        const auto rejectPost=[&](unsigned word=55) {
            assert(!registry.submitNativePostScalerStorageCoherency(word));
            assert(!backend.postStorageCalls && !registry.completedNativePostScalerStorageCoherency() &&
                get(writeBack)==52 && registry.graphicsRingConfiguration()->readWord==52);
        };
        for (unsigned word:{0u,52u,53u,54u,56u,UINT32_MAX}) rejectPost(word);
        unsigned postBits=0;
        for (unsigned at:{ring+52*4,ring+54*4,child,child+4,child+8,child+12,child+16,child+20,child+24,child+28,child+32,child+36,child+40})
            for (unsigned bit=0;bit<32;++bit) {const auto saved=get(at);put(at,saved^(1u<<bit));rejectPost();put(at,saved);++postBits;}
        for (unsigned physical:{phys(ring),phys(writeBack),phys(child)+1,0x20000000u,phys(commandStorage)+0x3FE0}) {
            put(ring+53*4,physical);rejectPost();
        }put(ring+53*4,phys(child));
        put(writeBack,51);assert(!registry.submitNativePostScalerStorageCoherency(55) && !backend.postStorageCalls);put(writeBack,52);
        backend.active=false;rejectPost();backend.active=true;
        for (unsigned fault=0;fault<5;++fault) {std::fflush(nullptr);const auto failure=fork();assert(failure>=0);
            if (!failure) {
                backend.postStorageAccept=fault!=0;backend.postStorageThrow=fault==1;
                if (fault>=2) backend.postStorageBefore=[&]{const std::array<unsigned,3> at{ring+52*4,child+40,writeBack};put(at[fault-2],get(at[fault-2])^1);};
                assert(!registry.submitNativePostScalerStorageCoherency(55));
                assert(fault<2 && get(writeBack)==52 && !registry.completedNativePostScalerStorageCoherency());
                backend.postStorageAccept=true;backend.postStorageThrow=false;
                assert(!registry.submitNativePostScalerStorageCoherency(55) && backend.postStorageCalls==1);std::_Exit(0);
            }int result=0;assert(waitpid(failure,&result,0)==failure && WIFEXITED(result) && WEXITSTATUS(result)==(fault<2?0:4));
        }
        backend.postStorageAccept=true;
        for (unsigned count=1;count<=256;++count) {
            const unsigned previous=52+(count-1)*3,next=previous+3;
            put(ring+previous*4,0xC0013F00);put(ring+(previous+1)*4,phys(child));put(ring+(previous+2)*4,11);
            assert(registry.submitNativePostScalerStorageCoherency(next));
            const auto post=registry.completedNativePostScalerStorageCoherency();
            assert(post && post->batches==count && post->firstWord==52 && post->previousWord==previous && post->readWord==next &&
                backend.postStorageCalls==count && get(writeBack)==next && registry.graphicsRingConfiguration()->readWord==next);
            assert(!registry.submitNativePostScalerStorageCoherency(next) && backend.postStorageCalls==count);
            assert(registry.completedNativeScalerWrapper()->readWord==52 && get(event0)==0x123 && get(event1)==0xABC);
        }
        assert(!registry.submitNativePostScalerStorageCoherency(823) && backend.postStorageCalls==256 && postBits==416);
        std::puts("NATIVE_POST_SCALER_STORAGE:416packet bits,2one-shot failures,3late fail-stops,256fresh fences/history/cap PASS; CPU fixture only");
        NativeDisplayGammaRamp::Packed gamma;gamma.fill(1023u<<20); // All input colors -> opaque red; alpha unchanged.
        assert(!registry.submitNativePostDisplayStorageCoherency(823) && !backend.postDisplayCalls);
        assert(!registry.submitExperimentalNativeDisplayOutput(823) && !backend.outputCalls);
        assert(!registry.submitNativePostOutputStorageCoherency(823) && !backend.postOutputCalls);
        auto gammaBytes=gammaWrapperBytes(gamma,phys(frontbuffer));
        const auto gammaChild=commandStorage+0x800;
        const auto putGamma=[&](unsigned word,unsigned value){for(unsigned b=0;b<4;++b)gammaBytes[word*4+b]=value>>(24-b*8);};
        putGamma(2333,controlPhysical+0x102);putGamma(2337,controlPhysical+0x106);
        std::memcpy(space.translate(gammaChild),gammaBytes.data(),gammaBytes.size());
        put(ring+820*4,0xC0013F00);put(ring+821*4,phys(gammaChild));put(ring+822*4,NativeDisplayGammaWrapper::Words);
        const auto before0=get(event0),before1=get(event1);
        unsigned displays=0;
        consumer=[&](const auto& image) {
            ++displays;assert(image.rgba.size()==1280*720*4 && image.physicalAddress==phys(frontbuffer));
            for(unsigned i=0;i<image.rgba.size();i+=4)assert(image.rgba[i]==255 && !image.rgba[i+1] && !image.rgba[i+2] && image.rgba[i+3]==255);
            return true;
        };
        const auto unchangedGamma=[&] {assert(get(writeBack)==820 && get(event0)==before0 && get(event1)==before1 &&
            !registry.completedNativeDisplayGamma() && registry.completedNativePostScalerStorageCoherency()->readWord==820);};
        for(unsigned word:{0u,820u,821u,822u,824u,UINT32_MAX}){assert(!registry.submitNativeDisplayGamma(word));unchangedGamma();}
        for(unsigned at:{ring+820*4,ring+821*4,ring+822*4,gammaChild,gammaChild+7*4,gammaChild+2327*4,frontbuffer}) {
            const auto saved=get(at);put(at,saved^1);assert(!registry.submitNativeDisplayGamma(823));put(at,saved);unchangedGamma();
        }
        assert(!backend.gammaCalls && !displays);
        for(unsigned fault=0;fault<16;++fault) {std::fflush(nullptr);const auto failed=fork();assert(failed>=0);
            if(!failed) {
                backend.gammaAccept=fault!=0;backend.gammaFault=fault;
                if(fault==3 || fault==4)consumer=[&](const auto&)->bool {++displays;if(fault==4)throw std::runtime_error("consumer decline");return false;};
                if(fault>=11)backend.gammaAfter=[&]{const std::array<unsigned,5> at{gammaChild,ring+820*4,frontbuffer,event0,writeBack};put(at[fault-11],get(at[fault-11])^1);};
                assert(!registry.submitNativeDisplayGamma(823));assert(fault<5);unchangedGamma();
                assert(!registry.submitNativeDisplayGamma(823) && backend.gammaCalls==1);std::_Exit(0);
            }int result=0;assert(waitpid(failed,&result,0)==failed && WIFEXITED(result) && WEXITSTATUS(result)==(fault<5?0:4));
        }
        backend.gammaAccept=true;assert(registry.submitNativeDisplayGamma(823) && displays==1);
        const auto gammaDone=registry.completedNativeDisplayGamma();
        assert(gammaDone && gammaDone->previousWord==820 && gammaDone->readWord==823 && gammaDone->gpu.nativeFences==2 &&
            gammaDone->gpu.presentQueued && get(writeBack)==823 && get(event0)==0x1234 && get(event1)==0xABCD);
        assert(!registry.submitNativeDisplayGamma(823) && displays==1 && backend.gammaCalls==1);
        std::puts("NATIVE_DISPLAY_GAMMA_REGISTRY:13preflight controls,5nonretry failures,11post-present fail-stops,921600mapped pixels/events/RPTR-last PASS; CPU fixture only");
        // Explicit no-persisted-display capability, not successful persistence.
        constexpr auto Persist="__imp__VdPersistDisplay";
        GuestImportCall persist;persist.stackPointer=sp;persist.arguments={sp+184,sp+160};
        persist.returnValue=0xFEDCBA9876543210ULL;
        std::memset(space.translate(sp+156),0xA5,44);
        for(unsigned i=0;i<3;++i)put(sp+184+i*4,0);
        const std::vector<std::uint8_t> persistBefore(space.translate(sp+156),space.translate(sp+200));
        const auto rejectPersist=[&](GuestImportCall value){
            const auto result=value.returnValue;const auto arguments=value.arguments;
            assert(!registry.dispatch(Persist,value).handled() && value.returnValue==result && value.arguments==arguments);
            assert(get(sp+160)==0xA5A5A5A5 && get(writeBack)==823 && backend.gammaCalls==1 && displays==1);
        };
        for(unsigned arg=0;arg<2;++arg)for(unsigned bit=0;bit<32;++bit){auto bad=persist;bad.arguments[arg]^=1U<<bit;rejectPersist(bad);}
        for(unsigned word=0;word<3;++word)for(unsigned bit=0;bit<32;++bit){put(sp+184+word*4,1U<<bit);rejectPersist(persist);put(sp+184+word*4,0);}
        for(auto stack:{0U,sp+1,0xFFFFFFF0U,other->layout.stackBase,ring}){auto bad=persist;bad.stackPointer=stack;
            bad.arguments={std::uint32_t(stack+184),std::uint32_t(stack+160)};rejectPersist(bad);}
        assert(registry.bindGuestThreadToken(other->layout.threadAddress));rejectPersist(persist);
        assert(registry.bindGuestThreadToken(worker->layout.threadAddress));
        assert(std::equal(persistBefore.begin(),persistBefore.end(),space.translate(sp+156)));
        for(unsigned attempt=0;attempt<2;++attempt){
            persist.returnValue=0xFEDCBA9876543210ULL;put(sp+160,0xA5A5A5A5);
            assert(registry.dispatch(Persist,persist).handled() && !persist.returnValue && !get(sp+160));
            for(unsigned i=0;i<persistBefore.size();++i)if(i<4 || i>=8)assert(space.translate(sp+156)[i]==persistBefore[i]);
            assert(get(writeBack)==823 && backend.gammaCalls==1 && displays==1 && get(event0)==0x1234 && get(event1)==0xABCD);
        }
        std::puts("NATIVE_PERSISTED_DISPLAY_ABSENT:64argument bits,96request bits,5bad frames,wrong thread,Boolean false/null output/canaries/no GPU work PASS");
        const auto displayChild=commandStorage+0x3800;
        for(unsigned i=0;i<11;++i)put(displayChild+i*4,childWords[i]);
        const auto displayRoot=[&](unsigned previous){put(ring+previous*4,0xC0013F00);
            put(ring+(previous+1)*4,phys(displayChild));put(ring+(previous+2)*4,11);};
        displayRoot(823);
        const auto unchangedDisplay=[&]{assert(!registry.completedNativePostDisplayStorageCoherency() && get(writeBack)==823 &&
            registry.completedNativePostScalerStorageCoherency()->readWord==820 && registry.completedNativeDisplayGamma()->readWord==823 &&
            backend.postStorageCalls==256 && displays==1 && get(event0)==0x1234 && get(event1)==0xABCD);};
        for(unsigned word:{0u,823u,824u,825u,827u,UINT32_MAX}){assert(!registry.submitNativePostDisplayStorageCoherency(word));unchangedDisplay();}
        unsigned displayBits=0;
        for(unsigned i=0;i<14;++i)for(unsigned bit=0;bit<32;++bit){
            const auto at=i<3?ring+(823+i)*4:displayChild+(i-3)*4,saved=get(at);
            put(at,saved^(1u<<bit));assert(!registry.submitNativePostDisplayStorageCoherency(826));put(at,saved);unchangedDisplay();++displayBits;
        }
        assert(!backend.postDisplayCalls && !registry.submitNativePostScalerStorageCoherency(826));
        for(unsigned fault=0;fault<5;++fault){std::fflush(nullptr);const auto failed=fork();assert(failed>=0);
            if(!failed){backend.postDisplayAccept=fault!=0;backend.postDisplayThrow=fault==1;
                if(fault>=2)backend.postDisplayBefore=[&]{const std::array<unsigned,3> at{ring+823*4,displayChild,writeBack};put(at[fault-2],get(at[fault-2])^1);};
                assert(!registry.submitNativePostDisplayStorageCoherency(826));assert(fault<2);unchangedDisplay();
                backend.postDisplayAccept=true;backend.postDisplayThrow=false;
                assert(!registry.submitNativePostDisplayStorageCoherency(826) && backend.postDisplayCalls==1);std::_Exit(0);
            }int status=0;assert(waitpid(failed,&status,0)==failed && WIFEXITED(status) && WEXITSTATUS(status)==(fault<2?0:4));
        }
        backend.postDisplayAccept=true;
        for(unsigned i=0;i<256;++i){displayRoot(823+i*3);assert(registry.submitNativePostDisplayStorageCoherency(826+i*3));
            const auto receipt=registry.completedNativePostDisplayStorageCoherency();
            assert(receipt && receipt->batches==i+1 && receipt->firstWord==823 && receipt->previousWord==823+i*3 && receipt->readWord==826+i*3);
            assert(backend.postDisplayCalls==i+1 && get(writeBack)==826+i*3 && backend.postStorageCalls==256 && displays==1 &&
                registry.completedNativeDisplayGamma()->readWord==823 && registry.completedNativePostScalerStorageCoherency()->readWord==820);
        }
        assert(!registry.submitNativePostDisplayStorageCoherency(1594) && backend.postDisplayCalls==256 && displayBits==448);
        std::puts("NATIVE_POST_DISPLAY_STORAGE:448packet bits,2nonretry failures,3late fail-stops,256fresh fences,separate history/cap/no frames PASS");
        assert(!registry.submitNativePostOutputStorageCoherency(1594) && !backend.postOutputCalls);
        const auto outputChild=commandStorage+0x3900;
        const auto outputBytes=displayOutputBytes(controlPhysical+0x100,controlPhysical+0x104,0x5678,0xDCBA);
        std::memcpy(space.translate(outputChild),outputBytes.data(),outputBytes.size());
        put(ring+1591*4,0xC0013F00);put(ring+1592*4,phys(outputChild));put(ring+1593*4,16);
        const auto unchangedOutput=[&]{assert(get(writeBack)==1591 && !registry.completedNativeDisplayOutput() &&
            registry.completedNativePostDisplayStorageCoherency()->readWord==1591 &&
            registry.completedNativeDisplayGamma()->readWord==823 && get(event0)==0x1234 && get(event1)==0xABCD);};
        for(unsigned word:{0u,1591u,1592u,1593u,1595u,UINT32_MAX}){assert(!registry.submitExperimentalNativeDisplayOutput(word));unchangedOutput();}
        unsigned outputBits=0;
        for(unsigned i=0;i<19;++i)for(unsigned bit=0;bit<32;++bit){
            // Event fields are dynamic; parser tests cover their bounds/aliases.
            if(i==13 || i==14 || i==17 || i==18)continue;
            const auto at=i<3?ring+(1591+i)*4:outputChild+(i-3)*4,saved=get(at);
            put(at,saved^(1u<<bit));assert(!registry.submitExperimentalNativeDisplayOutput(1594));put(at,saved);unchangedOutput();++outputBits;
        }
        for(unsigned at:{phys(ring),phys(writeBack),controlPhysical,phys(outputChild),phys(frontbuffer),0x20000000u}){
            const auto saved=get(outputChild+40);put(outputChild+40,at|2u);
            assert(!registry.submitExperimentalNativeDisplayOutput(1594));put(outputChild+40,saved);unchangedOutput();
        }
        assert(outputBits==480 && !backend.outputCalls && displays==1);
        // The actual original owned image is decoded independently below;
        // this consumer must receive pre-gamma pixels, not the previous red LUT.
        consumer=[&](const auto& image){++displays;
            const auto p=NativeSwapImagePlan::make(image.request.fetch,frontbuffer,phys(frontbuffer),image.request.format,
                image.request.colorSpace,image.request.width,image.request.height);assert(p);
            std::vector<std::uint8_t> expected;assert(p->decode(space.translate(frontbuffer),NativeSwapImagePlan::Capacity,expected));
            assert(image.rgba==expected && image.rgba.size()==1280*720*4);
            assert(get(writeBack)==1591 && get(event0)==0x1234 && get(event1)==0xABCD);return true;};
        for(unsigned fault=0;fault<17;++fault){std::fflush(nullptr);const auto failed=fork();assert(failed>=0);
            if(!failed){backend.outputAccept=fault!=0;backend.outputFault=fault;
                if(fault==3 || fault==4)consumer=[&](const auto&)->bool{++displays;if(fault==4)throw std::runtime_error("output rejected");return false;};
                if(fault>=11)backend.outputAfter=[&]{const std::array<unsigned,6> at{outputChild,ring+1591*4,frontbuffer,event0,event1,writeBack};put(at[fault-11],get(at[fault-11])^1);};
                assert(!registry.submitExperimentalNativeDisplayOutput(1594));assert(fault<5);unchangedOutput();
                assert(!registry.submitExperimentalNativeDisplayOutput(1594) && backend.outputCalls==1);std::_Exit(0);
            }int status=0;assert(waitpid(failed,&status,0)==failed && WIFEXITED(status) && WEXITSTATUS(status)==(fault<5?0:4));
        }
        backend.outputAccept=true;assert(registry.submitExperimentalNativeDisplayOutput(1594));
        const auto outputDone=registry.completedNativeDisplayOutput();
        assert(outputDone && outputDone->previousWord==1591 && outputDone->readWord==1594 && outputDone->gpu.presentQueued &&
            outputDone->gpu.nativeFences==2 && get(writeBack)==1594 && get(event0)==0x5678 && get(event1)==0xDCBA && displays==2);
        assert(!registry.submitExperimentalNativeDisplayOutput(1594) && !registry.submitExperimentalNativeDisplayOutput(1597) && backend.outputCalls==1);
        assert(registry.completedNativePostDisplayStorageCoherency()->readWord==1591 && registry.completedNativeDisplayGamma()->readWord==823);
        std::puts("NATIVE_DISPLAY_OUTPUT_REGISTRY:480packet bits,6aliases,5nonretry failures,12post-present fail-stops,owned pre-gamma pixels/events/RPTR-last PASS");
        constexpr auto Retrain="__imp__VdRetrainEDRAM";
        constexpr unsigned TrainingLock=0x62004600;
        const auto control=[&](const char* name,unsigned arg=0){GuestImportCall c;c.arguments[0]=arg;assert(registry.dispatch(name,c).handled());};
        std::memset(space.translate(sp-16),0xC5,2176);put(sp+80,0);put(sp+84,0);
        const std::vector<std::uint8_t> trainingBefore(space.translate(sp-16),space.translate(sp+2160));
        const auto priorTraining=registry.nativeEdramTrainingState();
        GuestImportCall query;query.stackPointer=sp;query.arguments={0,sp+80,4096,sp+84,sp+96,2048,0xAABBCCDD,0x11223344};
        query.returnValue=0xFEDCBA9876543210ULL;
        const auto unchangedTraining=[&]{const auto s=registry.nativeEdramTrainingState();
            assert(s.workerChecks==priorTraining.workerChecks && s.noCommandQueries==priorTraining.noCommandQueries && !s.directNoCommandQueries &&
                std::equal(trainingBefore.begin(),trainingBefore.end(),space.translate(sp-16)) && get(writeBack)==1594 &&
                get(event0)==0x5678 && get(event1)==0xDCBA && displays==2 && backend.outputCalls==1);};
        const auto rejectTraining=[&](GuestImportCall c){assert(!registry.dispatch(Retrain,c).handled() && c.returnValue==query.returnValue);unchangedTraining();};
        rejectTraining(query);control("__imp__KeEnterCriticalRegion");rejectTraining(query);
        control("__imp__RtlEnterCriticalSection",TrainingLock);
        for(unsigned arg=0;arg<6;++arg)for(unsigned bit=0;bit<64;++bit){auto bad=query;bad.arguments[arg]^=1ULL<<bit;rejectTraining(bad);}
        for(unsigned at:{sp+80,sp+84})for(unsigned bit=0;bit<32;++bit){put(at,1u<<bit);
            auto bad=query;assert(!registry.dispatch(Retrain,bad).handled() && bad.returnValue==query.returnValue && get(at)==(1u<<bit));put(at,0);unchangedTraining();}
        for(unsigned badSp:{0u,sp+1,other->layout.stackBase+0x1000,worker->layout.stackBase+worker->layout.stackSize-1024,0xFFFFFFF0u}){
            auto bad=query;bad.stackPointer=badSp;bad.arguments[1]=std::uint64_t{badSp}+80;
            bad.arguments[3]=std::uint64_t{badSp}+84;bad.arguments[4]=std::uint64_t{badSp}+96;rejectTraining(bad);}
        assert(registry.bindGuestThreadToken(other->layout.threadAddress));rejectTraining(query);
        assert(registry.bindGuestThreadToken(worker->layout.threadAddress));
        backend.active=false;rejectTraining(query);backend.active=true;
        backend.trainingAvailable=false;rejectTraining(query);backend.trainingAvailable=true;
        backend.throws=true;rejectTraining(query);backend.throws=false;
        const auto prepared=fork();assert(prepared>=0);
        if(!prepared){control("__imp__VdRetrainEDRAMWorker");auto c=query;
            assert(!registry.dispatch(Retrain,c).handled() && !registry.nativeEdramTrainingState().directNoCommandQueries);
            c.arguments[0]=1;assert(registry.dispatch(Retrain,c).handled() && !c.returnValue &&
                !registry.nativeEdramTrainingState().directNoCommandQueries);std::_Exit(0);}
        int preparedStatus=0;assert(waitpid(prepared,&preparedStatus,0)==prepared && WIFEXITED(preparedStatus) && !WEXITSTATUS(preparedStatus));
        const auto calls=backend.trainingCalls;
        const auto capCase=fork();assert(capCase>=0);
        if(!capCase){
        for(unsigned i=0;i<256;++i){auto c=query;assert(registry.dispatch(Retrain,c).handled() && !c.returnValue);
            const auto s=registry.nativeEdramTrainingState();assert(s.workerChecks==priorTraining.workerChecks && s.statusQueries==priorTraining.statusQueries &&
                s.noCommandQueries==priorTraining.noCommandQueries+i+1 && s.directNoCommandQueries==i+1 && backend.trainingCalls==calls+i+1);
            assert(std::equal(trainingBefore.begin(),trainingBefore.end(),space.translate(sp-16)) && get(writeBack)==1594 &&
                get(event0)==0x5678 && get(event1)==0xDCBA && displays==2);}
        auto capped=query;assert(!registry.dispatch(Retrain,capped).handled() && capped.returnValue==query.returnValue && backend.trainingCalls==calls+256);
        std::_Exit(0);}
        int capStatus=0;assert(waitpid(capCase,&capStatus,0)==capCase && WIFEXITED(capStatus) && !WEXITSTATUS(capStatus));
        auto firstDirect=query;assert(registry.dispatch(Retrain,firstDirect).handled() && !firstDirect.returnValue &&
            registry.nativeEdramTrainingState().directNoCommandQueries==1 && backend.trainingCalls==calls+1);
        control("__imp__RtlLeaveCriticalSection",TrainingLock);control("__imp__KeLeaveCriticalRegion");
        std::puts("NATIVE_EDRAM_DIRECT_QUERY:384argument bits,64nonempty bits,5bad frames,lock/region/worker separation,256fresh capability queries/cap/no GPU writes PASS");
        // New post-output phase: none of the old fence receipts may be reused.
        const auto outputStorageChild=commandStorage+0x3A00;
        for(unsigned i=0;i<11;++i)put(outputStorageChild+i*4,childWords[i]);
        const auto outputStorageRoot=[&](unsigned previous){put(ring+previous*4,0xC0013F00);
            put(ring+(previous+1)*4,phys(outputStorageChild));put(ring+(previous+2)*4,11);};
        outputStorageRoot(1594);
        const auto unchangedOutputStorage=[&]{assert(!registry.completedNativePostOutputStorageCoherency() && get(writeBack)==1594 &&
            registry.completedNativeDisplayOutput()->readWord==1594 && registry.completedNativePostDisplayStorageCoherency()->readWord==1591 &&
            registry.completedNativePostScalerStorageCoherency()->readWord==820 && backend.postDisplayCalls==256 && backend.postStorageCalls==256 &&
            backend.outputCalls==1 && displays==2 && get(event0)==0x5678 && get(event1)==0xDCBA);};
        for(unsigned word:{0u,1594u,1595u,1596u,1598u,UINT32_MAX}){
            assert(!registry.submitNativePostOutputStorageCoherency(word));unchangedOutputStorage();}
        unsigned outputStorageBits=0;
        for(unsigned i=0;i<14;++i)for(unsigned bit=0;bit<32;++bit){
            // The IB address is dynamic. A bit flip can select another valid
            // identical child in this fixture; do not mislabel that as invalid.
            if(i==1)continue;
            const auto at=i<3?ring+(1594+i)*4:outputStorageChild+(i-3)*4,saved=get(at);
            put(at,saved^(1u<<bit));assert(!registry.submitNativePostOutputStorageCoherency(1597));put(at,saved);unchangedOutputStorage();++outputStorageBits;
        }
        for(unsigned target:{phys(outputStorageChild)+1,phys(outputStorageChild)+2,phys(commandStorage)+0x4000,
                phys(ring),phys(writeBack),0x20000000u}){
            put(ring+1595*4,target);assert(!registry.submitNativePostOutputStorageCoherency(1597));
            outputStorageRoot(1594);unchangedOutputStorage();}
        backend.active=false;assert(!registry.submitNativePostOutputStorageCoherency(1597));backend.active=true;unchangedOutputStorage();
        put(writeBack,1593);assert(!registry.submitNativePostOutputStorageCoherency(1597));put(writeBack,1594);unchangedOutputStorage();
        assert(!backend.postOutputCalls && !registry.submitNativePostDisplayStorageCoherency(1597) && !registry.submitNativePostScalerStorageCoherency(1597));
        for(unsigned fault=0;fault<5;++fault){std::fflush(nullptr);const auto failed=fork();assert(failed>=0);
            if(!failed){backend.postOutputAccept=fault!=0;backend.postOutputThrow=fault==1;
                if(fault>=2)backend.postOutputBefore=[&]{const std::array<unsigned,3> at{ring+1594*4,outputStorageChild,writeBack};put(at[fault-2],get(at[fault-2])^1);};
                assert(!registry.submitNativePostOutputStorageCoherency(1597));assert(fault<2);unchangedOutputStorage();
                backend.postOutputAccept=true;backend.postOutputThrow=false;
                assert(!registry.submitNativePostOutputStorageCoherency(1597) && backend.postOutputCalls==1);std::_Exit(0);
            }int status=0;assert(waitpid(failed,&status,0)==failed && WIFEXITED(status) && WEXITSTATUS(status)==(fault<2?0:4));
        }
        backend.postOutputAccept=true;
        for(unsigned i=0;i<256;++i){outputStorageRoot(1594+i*3);
            if(i&1)put(ring+(1595+i*3)*4,phys(displayChild)); // Another valid owned copy, same strict request.
            assert(registry.submitNativePostOutputStorageCoherency(1597+i*3));
            const auto receipt=registry.completedNativePostOutputStorageCoherency();
            assert(receipt && receipt->batches==i+1 && receipt->firstWord==1594 && receipt->previousWord==1594+i*3 && receipt->readWord==1597+i*3);
            assert(backend.postOutputCalls==i+1 && get(writeBack)==1597+i*3 && backend.postDisplayCalls==256 && backend.postStorageCalls==256 &&
                backend.outputCalls==1 && displays==2 && get(event0)==0x5678 && get(event1)==0xDCBA &&
                registry.completedNativeDisplayOutput()->readWord==1594 && registry.completedNativePostDisplayStorageCoherency()->readWord==1591 &&
                registry.completedNativeDisplayGamma()->readWord==823 && registry.completedNativePostScalerStorageCoherency()->readWord==820);
            assert(!registry.submitNativePostOutputStorageCoherency(1597+i*3) && backend.postOutputCalls==i+1);
        }
        assert(!registry.submitNativePostOutputStorageCoherency(2365) && backend.postOutputCalls==256 && outputStorageBits==416);
        std::puts("NATIVE_POST_OUTPUT_STORAGE:416packet bits,6invalid/two valid child addresses,2nonretry failures,3late fail-stops,256fresh fences/history/cap/no frames PASS");
        testRuntimeCommandStorage(registry,space,backend,ring,writeBack,commandStorage);
        // Read-only later-color inspection keeps the old startup receipt and
        // scanout image alive. This is NOT the second live publication path.
        const auto allocate=[&](unsigned size){GuestImportCall c;c.arguments={1,size,4,0,0xFFFFFFFF,0};
            assert(registry.dispatch("__imp__MmAllocatePhysicalMemoryEx",c).handled() && c.returnValue);return static_cast<unsigned>(c.returnValue);};
        const auto nextSource=allocate(0x200000),nextCopy=allocate(0x400000),nextChild=allocate(0x4000);
        NativeColorBatchFixture nextColor(phys(nextSource),phys(nextCopy),phys(event0),phys(event1),true,true);
        for(unsigned i=0;i<nextColor.words.size();++i)put(nextChild+i*4,nextColor.words[i]);
        for(unsigned i=0;i<8;++i)std::memcpy(space.translate(nextColor.addresses[i]+0xA0000000),nextColor.raw[i].data(),nextColor.raw[i].size());
        std::memset(space.translate(nextCopy),0xC7,0x400000);
        put(ring+2362*4,0xC0013F00);put(ring+2363*4,phys(nextChild));put(ring+2364*4,nextColor.words.size());
        const auto originalColor=registry.completedNativeOwnedColorBatch();assert(originalColor);
        unsigned visits=0,rejections=0;
        const auto inspect=[&](const NativeColorBatchCapture& capture){++visits;
            assert(capture.controls().previousWord==2362 && capture.controls().nextWord==2365 && capture.memory().inputCount==7 &&
                capture.synchronization().phase==NativeColorBatchPhase::PostOutput && capture.color().sourceRanges().constantZeroTextures==1);
            return true;};
        const auto intact=[&]{assert(get(writeBack)==2362 && registry.graphicsRingConfiguration()->readWord==2362 &&
            registry.completedNativePostOutputStorageCoherency()->readWord==2362 && backend.postOutputCalls==256 && displays==2 &&
            registry.completedNativeOwnedColorBatch()->readWord==originalColor->readWord && get(event0)==0x5678 && get(event1)==0xDCBA &&
            std::all_of(space.translate(nextCopy),space.translate(nextCopy)+0x400000,[](auto b){return b==0xC7;}));};
        const auto reject=[&](unsigned next=2365){const auto before=visits;
            assert(!registry.inspectStoppedPostOutputColorBatch(*nextColor.nativeProgram,{},next,inspect) && visits==before);++rejections;intact();};
        assert(registry.inspectStoppedPostOutputColorBatch(*nextColor.nativeProgram,{},2365,inspect) && visits==1);intact();
        assert(!registry.inspectStoppedNativeColorBatch(*nextColor.nativeProgram,{},2365,inspect));
        assert(!registry.submitExperimentalOwnedColorBatch(*nextColor.nativeProgram,{},2365));intact();
        for(unsigned next:{0u,2362u,2363u,2364u,2366u,UINT32_MAX})reject(next);
        for(unsigned bit=0;bit<32;++bit){put(ring+2362*4,0xC0013F00^(1u<<bit));reject();put(ring+2362*4,0xC0013F00);}
        for(unsigned i=0;i<3;++i){auto* bytes=space.translate(nextColor.addresses[i]+0xA0000000);bytes[0]^=1;reject();bytes[0]^=1;}
        backend.active=false;reject();backend.active=true;
        put(nextChild+nextColor.copyAddressWord*4,phys(writeBack));reject();put(nextChild+nextColor.copyAddressWord*4,phys(nextCopy));
        assert(!registry.inspectStoppedPostOutputColorBatch(*nextColor.nativeProgram,{},2365,{}));intact();
        assert(!registry.inspectStoppedPostOutputColorBatch(*nextColor.nativeProgram,{},2365,[](const auto&){return false;}));intact();
        assert(!registry.inspectStoppedPostOutputColorBatch(*nextColor.nativeProgram,{},2365,[](const auto&)->bool{throw 1;}));intact();
        assert(registry.inspectStoppedPostOutputColorBatch(*nextColor.nativeProgram,{},2365,inspect) && visits==2);intact();
        assert(!registry.runStoppedPostOutputColorDiagnostic(*nextColor.nativeProgram,{},2365,{}));intact();
        assert(!registry.runStoppedPostOutputColorDiagnostic(*nextColor.nativeProgram,{},2365,[](const auto&){return false;}));intact();
        assert(!registry.runStoppedPostOutputColorDiagnostic(*nextColor.nativeProgram,{},2365,[](const auto&)->bool{throw 1;}));intact();
        assert(registry.runStoppedPostOutputColorDiagnostic(*nextColor.nativeProgram,{},2365,inspect) && visits==3);intact();
        std::printf("STOPPED_POST_OUTPUT_COLOR_INSPECTION: %u preflight refusals,seven real owned reads,old receipts/image/events/RPTR unchanged,no execution PASS\n",rejections);
        assert(!registry.submitExperimentalPostOutputColorBatch(*nextColor.nativeProgram,{},2365) && !backend.postColorCalls);intact();
        // Live commands must be in the retained command allocation. Inspectors
        // may inspect more; a copy alone does not establish scanout continuity.
        const auto liveChild=commandStorage+0x1000;
        assert(nextColor.words.size()*4<=0x3000);
        nextColor.words[nextColor.copyAddressWord]=phys(frontbuffer);
        for(unsigned i=0;i<nextColor.words.size();++i)put(liveChild+i*4,nextColor.words[i]);
        put(ring+2363*4,phys(liveChild));
        const auto preservedPixels=std::vector<std::uint8_t>(space.translate(frontbuffer),space.translate(frontbuffer)+NativeSwapImagePlan::Capacity);
        const auto liveIntact=[&]{intact();assert(!registry.completedNativePostOutputColorBatch() &&
            !std::memcmp(preservedPixels.data(),space.translate(frontbuffer),preservedPixels.size()));};
        unsigned liveRejects=0;
        const auto liveReject=[&](unsigned next=2365){assert(!registry.submitExperimentalPostOutputColorBatch(*nextColor.nativeProgram,{},next) && !backend.postColorCalls);
            ++liveRejects;};
        for(unsigned next:{0u,2362u,2363u,2364u,2366u,UINT32_MAX})liveReject(next);
        for(unsigned bit=0;bit<32;++bit){put(ring+2362*4,0xC0013F00^(1u<<bit));liveReject();put(ring+2362*4,0xC0013F00);}
        space.translate(frontbuffer)[0]^=1;liveReject();assert(registry.nativePostOutputColorStatus()[1]==2);space.translate(frontbuffer)[0]^=1;
        put(liveChild+nextColor.copyAddressWord*4,phys(frontbuffer)+0x1000);liveReject();put(liveChild+nextColor.copyAddressWord*4,phys(frontbuffer));
        put(ring+2363*4,phys(nextChild));liveReject();put(ring+2363*4,phys(liveChild));
        backend.active=false;liveReject();backend.active=true;liveIntact();
        // Every backend failure is terminal, including success without publish,
        // invalid staged results and late source/control/old-image mutation.
        backend.colorBefore=[]{};
        const auto nextSwapSp=registry.nativeSystemContext()->stackPointer;
        const auto prepareNextSwap=[&](unsigned image){
            put(nextSwapSp+84,nextSwapSp+96);put(nextSwapSp+92,nextSwapSp+100);
            put(nextSwapSp+96,1280);put(nextSwapSp+100,720);put(nextSwapSp+104,0);
            put(nextSwapSp+108,image);put(nextSwapSp+112,6);
            const std::array<unsigned,6> fetch{0x8A000002,image|6,0x0059E4FF,0x1414,0,0x200};
            for(unsigned i=0;i<6;++i)put(nextSwapSp+128+i*4,fetch[i]);
            GuestImportCall call;call.stackPointer=nextSwapSp;
            call.arguments={commandStorage+0x3000,nextSwapSp+128,*registry.graphicsSystemIdentifierAddress(),nextSwapSp+208,
                0,nextSwapSp+108,nextSwapSp+112,nextSwapSp+104};return call;
        };
        // Separately owned backbuffer: preserve the old display, write only
        // active new pixels, retain arbitrary padding. Late new-buffer writes
        // (including inactive padding) refuse before publication/no retries.
        unsigned separateChecks=0;
        for(unsigned fault=0;fault<3;++fault){const auto child=fork();assert(child>=0);
            if(!child){put(liveChild+nextColor.copyAddressWord*4,phys(nextCopy));backend.colorAccept=true;backend.colorFault=0;
                const auto at=nextCopy+(fault==2?NativeSwapImagePlan::Capacity-4:0);
                if(fault)backend.colorBefore=[&]{put(at,0xC6C7C7C7);};
                const bool complete=registry.submitExperimentalPostOutputColorBatch(*nextColor.nativeProgram,{},2365);
                if(fault){assert(!complete);put(at,0xC7C7C7C7);liveIntact();}
                else{assert(complete && registry.completedNativePostOutputColorBatch()->color.readWord==2365 && get(writeBack)==2365 && displays==2);
                    std::vector<bool> active(NativeSwapImagePlan::Capacity/4);
                    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x){unsigned at=0;assert(colorCopyOffset(1280,720,x,y,at));active[at/4]=true;}
                    for(unsigned w=0;w<active.size();++w)assert(get(nextCopy+w*4)==(active[w]?0u:0xC7C7C7C7));
                    assert(!std::memcmp(preservedPixels.data(),space.translate(frontbuffer),preservedPixels.size()));
                    assert(registry.bindGuestThreadToken(0x31000000));
                    GuestImportCall system;system.stackPointer=nextSwapSp;system.arguments={nextSwapSp+208,nextSwapSp+116};
                    assert(registry.dispatch("__imp__VdGetSystemCommandBuffer",system).handled());
                    const auto call=prepareNextSwap(nextCopy);
                    assert(registry.inspectStoppedPostOutputSwapRequest(call));
                    assert(registry.visitStoppedPostOutputSwapImage(call,[&](const auto& image){
                        assert(image.physicalAddress==phys(nextCopy) && image.owner.virtualBase==nextCopy &&
                            image.rgba.size()==NativeSwapImagePlan::PixelBytes);return true;}));
                    put(nextSwapSp+108,frontbuffer);assert(!registry.visitStoppedPostOutputSwapImage(call,[](const auto&){return true;}));
                    auto issue=prepareNextSwap(nextCopy);assert(registry.dispatch("__imp__VdSwap",issue).handled());
                    const auto ticket=NativeSwapCommand::make({2,phys(issue.argument32(0)),phys(nextCopy),2365});assert(ticket &&
                        ticket->matches(phys(issue.argument32(0)),space.translate(issue.argument32(0)),256));
                    GuestImportCall free;free.arguments={1,nextCopy};assert(!registry.dispatch("__imp__MmFreePhysicalMemory",free).handled());
                    assert(get(writeBack)==2365 && displays==2 && !std::memcmp(preservedPixels.data(),space.translate(frontbuffer),preservedPixels.size()));}
                backend.colorBefore=[]{};
                assert(!registry.submitExperimentalPostOutputColorBatch(*nextColor.nativeProgram,{},2365) && backend.postColorCalls==1);std::_Exit(0);}
            int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));++separateChecks;
        }
        assert(separateChecks==3);
        std::puts("LIVE_POST_OUTPUT_BACKBUFFER: separate owner,921600active pixels,padding/old-display preserved,2late-buffer mutations,no flip PASS");
        unsigned lateChecks=0;
        for(unsigned fault=0;fault<=11;++fault){
            const auto child=fork();assert(child>=0);
            if(!child){backend.colorAccept=fault!=0;backend.colorFault=fault;
                assert(!registry.submitExperimentalPostOutputColorBatch(*nextColor.nativeProgram,{},2365));
                liveIntact();backend.colorAccept=true;backend.colorFault=0;
                assert(!registry.submitExperimentalPostOutputColorBatch(*nextColor.nativeProgram,{},2365) && backend.postColorCalls==1);std::_Exit(0);}
            int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==((fault==8 || fault==9 || fault==11)?4:0));
        }
        for(unsigned at:{ring+2362*4,liveChild,writeBack,frontbuffer,event0,event1,*registry.graphicsSystemIdentifierAddress(),nextSource}){
            const auto child=fork();assert(child>=0);
            if(!child){const auto before=get(at);backend.colorAccept=true;backend.colorFault=0;
                backend.colorBefore=[&]{put(at,before^1);};
                assert(!registry.submitExperimentalPostOutputColorBatch(*nextColor.nativeProgram,{},2365));put(at,before);liveIntact();
                backend.colorBefore=[]{};
                assert(!registry.submitExperimentalPostOutputColorBatch(*nextColor.nativeProgram,{},2365) && backend.postColorCalls==1);std::_Exit(0);}
            int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));++lateChecks;
        }
        backend.colorAccept=true;backend.colorFault=0;backend.colorBefore=[]{};
        assert(registry.submitExperimentalPostOutputColorBatch(*nextColor.nativeProgram,{},2365));
        testRuntimeCommandStorage(registry,space,backend,ring,writeBack,commandStorage);
        const auto liveReceipt=registry.completedNativePostOutputColorBatch();
        assert((registry.nativePostOutputColorStatus()==std::array<unsigned,3>{1,0,0}));
        assert(liveReceipt && liveReceipt->color.previousWord==2362 && liveReceipt->color.readWord==2365 &&
            liveReceipt->color.nativeFences==4 && liveReceipt->leadingSetupFences==1 && liveReceipt->color.copyBytes==3686400 &&
            get(writeBack)==2365 && registry.graphicsRingConfiguration()->readWord==2365 && backend.postColorCalls==1 && displays==2 &&
            registry.completedNativeOwnedColorBatch()->readWord==originalColor->readWord &&
            registry.completedNativePostOutputStorageCoherency()->readWord==2362 &&
            registry.completedNativeDisplayOutput()->readWord==1594);
        assert(!registry.submitExperimentalPostOutputColorBatch(*nextColor.nativeProgram,{},2365) && backend.postColorCalls==1);
        assert(!registry.inspectStoppedPostOutputColorBatch(*nextColor.nativeProgram,{},2365,inspect));
        // Equal synthetic pixels still cannot inherit the old scanout lease.
        auto staleDisplay=persist;staleDisplay.returnValue=123;
        assert(!registry.dispatch("__imp__VdPersistDisplay",staleDisplay).handled() && staleDisplay.returnValue==123);
        std::printf("LIVE_POST_OUTPUT_COLOR: %u preflight refusals,12terminal backend controls,%ulate mutations,old receipts preserved,no presentation PASS\n",liveRejects,lateChecks);
        const auto oldContext=*registry.nativeSystemContext();const auto systemSp=oldContext.stackPointer;
        GuestImportCall nextSystem;nextSystem.stackPointer=systemSp;nextSystem.arguments={systemSp+208,systemSp+116};
        nextSystem.returnValue=0xABCDEF0123456789ULL;
        std::memset(space.translate(systemSp),0xA7,480);
        const auto frameBefore=std::vector<std::uint8_t>(space.translate(systemSp),space.translate(systemSp)+480);
        unsigned contextRejects=0;
        const auto rejectContext=[&](GuestImportCall call){assert(!registry.dispatch("__imp__VdGetSystemCommandBuffer",call).handled() &&
            call.returnValue==nextSystem.returnValue && !registry.nativePostOutputSystemContext() &&
            !std::memcmp(frameBefore.data(),space.translate(systemSp),480));++contextRejects;};
        rejectContext(nextSystem); // Different current worker cannot borrow the initial issuer's stack.
        assert(registry.bindGuestThreadToken(0x31000000)); // Original initial-thread fixture, not an invented worker.
        for(unsigned arg=0;arg<2;++arg)for(unsigned bit=0;bit<32;++bit){auto bad=nextSystem;bad.arguments[arg]^=1u<<bit;rejectContext(bad);}
        for(unsigned frame:{0u,systemSp+16,sp,UINT32_MAX}){auto bad=nextSystem;bad.stackPointer=frame;
            bad.arguments={std::uint32_t(frame+208),std::uint32_t(frame+116)};rejectContext(bad);}
        put(writeBack,2364);rejectContext(nextSystem);put(writeBack,2365);
        space.translate(frontbuffer)[0]^=1;rejectContext(nextSystem);space.translate(frontbuffer)[0]^=1;
        backend.active=false;rejectContext(nextSystem);backend.active=true;
        for(unsigned offset:{0u,108u,200u,476u}){const auto child=fork();assert(child>=0);
            if(!child){GuestImportCall cs;cs.arguments={systemSp+offset,0};assert(registry.dispatch("__imp__RtlInitializeCriticalSectionAndSpinCount",cs).handled());
                const auto before=std::vector<std::uint8_t>(space.translate(systemSp),space.translate(systemSp)+504);
                auto call=nextSystem;assert(!registry.dispatch("__imp__VdGetSystemCommandBuffer",call).handled() &&
                    call.returnValue==nextSystem.returnValue && !std::memcmp(before.data(),space.translate(systemSp),before.size()));std::_Exit(0);}
            int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));}
        assert(registry.dispatch("__imp__VdGetSystemCommandBuffer",nextSystem).handled() && nextSystem.returnValue==0xABCDEF0123456789ULL);
        const auto context=registry.nativePostOutputSystemContext();assert(context && context->readWord==2365 &&
            context->stackPointer==systemSp && context->buffer==systemSp+208 && context->identifierOutput==systemSp+116 && context->policy==1 &&
            registry.nativeSystemContext()->readWord==oldContext.readWord && get(writeBack)==2365 && displays==2 && backend.postColorCalls==1);
        for(unsigned i=0;i<480;++i){const bool written=(i>=116 && i<120) || (i>=208 && i<356);
            assert(space.translate(systemSp)[i]==(written?0:frameBefore[i]));}
        assert(!registry.dispatch("__imp__VdGetSystemCommandBuffer",nextSystem).handled());
        assert(!registry.dispatch("__imp__VdSwap",nextSystem).handled()); // Context is NOT a swap completion.
        std::printf("POST_OUTPUT_SYSTEM_CONTEXT: %urefusal controls,4partial-lock overlaps,152output bytes/canaries,void ABI,old history,no GPU/swap PASS\n",contextRejects);
        const auto nextSwap=prepareNextSwap(frontbuffer);
        const auto nextFrame=std::vector<std::uint8_t>(space.translate(systemSp),space.translate(systemSp)+480);
        const auto nextCommand=std::vector<std::uint8_t>(space.translate(commandStorage+0x3000),space.translate(commandStorage+0x3100));
        const auto oldSwap=registry.nativeSwapCommandStatus();const auto oldIdentifier=get(*registry.graphicsSystemIdentifierAddress());
        const auto swapIntact=[&]{assert(get(writeBack)==2365 && get(*registry.graphicsSystemIdentifierAddress())==oldIdentifier &&
            registry.nativeSwapCommandStatus().readWord==oldSwap.readWord && !registry.nativeSwapCommandStatus().pending &&
            registry.nativeSystemContext()->readWord==oldContext.readWord && registry.nativePostOutputSystemContext()->readWord==2365 &&
            !std::memcmp(nextFrame.data(),space.translate(systemSp),480) &&
            !std::memcmp(nextCommand.data(),space.translate(commandStorage+0x3000),256) && displays==2 && backend.postColorCalls==1);};
        assert(!registry.inspectStoppedNativeSwapRequest(nextSwap) && !registry.visitNativeOwnedSwapImage(nextSwap,[](const auto&){return true;}));
        const auto request=registry.inspectStoppedPostOutputSwapRequest(nextSwap);assert(request &&
            request->commandVirtual==commandStorage+0x3000 && request->allocationBase==commandStorage &&
            request->frontbufferVirtual==frontbuffer && request->width==1280 && request->height==720 && request->identifierWord==oldIdentifier);
        unsigned nextVisits=0,nextRefusals=0;
        const auto nextVisit=[&](const auto& image){++nextVisits;assert(image.physicalAddress==phys(frontbuffer) &&
            image.rgba.size()==NativeSwapImagePlan::PixelBytes);
            for(unsigned i=0;i<image.rgba.size();++i)assert(image.rgba[i]==(i%4==3?255:0));return true;};
        const auto rejectNext=[&](const GuestImportCall& call){const auto before=nextVisits;
            assert(!registry.visitStoppedPostOutputSwapImage(call,nextVisit) && nextVisits==before);
            auto issue=call;issue.returnValue=0xABCDEF0123456789ULL;
            assert(!registry.dispatch("__imp__VdSwap",issue).handled() && issue.returnValue==0xABCDEF0123456789ULL &&
                !registry.nativePostOutputSwapStatus().issued && !std::memcmp(nextCommand.data(),space.translate(commandStorage+0x3000),256));++nextRefusals;};
        assert(registry.visitStoppedPostOutputSwapImage(nextSwap,nextVisit));swapIntact();
        for(unsigned arg=1;arg<8;++arg)for(unsigned bit=0;bit<32;++bit){auto bad=nextSwap;bad.arguments[arg]^=1u<<bit;rejectNext(bad);}
        for(unsigned address:{0u,commandStorage+0x3F80,commandStorage+0x3001,ring,writeBack,frontbuffer,nextCopy,nextChild,UINT32_MAX}){
            auto bad=nextSwap;bad.arguments[0]=address;rejectNext(bad);}
        auto bad=nextSwap;bad.stackPointer+=16;rejectNext(bad);
        for(unsigned offset:{84u,92u,96u,100u,104u,108u,112u,116u,208u,352u}){
            const auto before=get(systemSp+offset);put(systemSp+offset,before^1);rejectNext(nextSwap);put(systemSp+offset,before);}
        for(unsigned i=0;i<6;++i)for(unsigned bit=0;bit<32;++bit){const auto at=systemSp+128+i*4,before=get(at);
            put(at,before^(1u<<bit));rejectNext(nextSwap);put(at,before);}
        put(writeBack,2364);rejectNext(nextSwap);put(writeBack,2365);
        space.translate(frontbuffer)[0]^=1;rejectNext(nextSwap);space.translate(frontbuffer)[0]^=1;
        backend.active=false;rejectNext(nextSwap);backend.active=true;
        for(unsigned at:{systemSp+476,commandStorage+0x30FC,frontbuffer+NativeSwapImagePlan::Capacity-4}){
            const auto child=fork();assert(child>=0);if(!child){GuestImportCall cs;cs.arguments={at,0};
                assert(registry.dispatch("__imp__RtlInitializeCriticalSectionAndSpinCount",cs).handled());
                assert(!registry.visitStoppedPostOutputSwapImage(nextSwap,[](const auto&){return true;}));
                auto issue=nextSwap;assert(!registry.dispatch("__imp__VdSwap",issue).handled());std::_Exit(0);}
            int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));}
        assert(!registry.visitStoppedPostOutputSwapImage(nextSwap,{}));
        assert(!registry.visitStoppedPostOutputSwapImage(nextSwap,[](const auto&){return false;}));
        assert(!registry.visitStoppedPostOutputSwapImage(nextSwap,[](const auto&)->bool{throw 1;}));
        assert(registry.visitStoppedPostOutputSwapImage(nextSwap,nextVisit) && nextVisits==2);swapIntact();
        std::printf("STOPPED_POST_OUTPUT_SWAP: %urefusals,3partial-lock overlaps,separate/same image owners,old history/no commands/no presentation PASS\n",nextRefusals);
        assert(registry.bindGuestThreadToken(worker->layout.threadAddress));
        auto issue=nextSwap;issue.returnValue=0xABCDEF0123456789ULL;
        assert(!registry.dispatch("__imp__VdSwap",issue).handled());swapIntact();
        assert(registry.bindGuestThreadToken(0x31000000));
        const auto cmd=nextSwap.argument32(0),beforeCommand=get(cmd-4),afterCommand=get(cmd+256);
        assert(registry.dispatch("__imp__VdSwap",issue).handled() && issue.returnValue==0xABCDEF0123456789ULL && issue.arguments==nextSwap.arguments);
        const auto issued=registry.nativePostOutputSwapStatus();
        assert(issued.issued && issued.pending && !issued.presentQueued && issued.commandPhysical==phys(cmd) &&
            issued.previousWord==2365 && issued.readWord==2365 && issued.issuedReadWord==2365 && get(writeBack)==2365 &&
            registry.nativeSwapCommandStatus().readWord==oldSwap.readWord && registry.nativeSwapCommandStatus().presentQueued &&
            registry.nativeSystemContext()->readWord==oldContext.readWord && displays==2 && backend.postColorCalls==1 &&
            get(*registry.graphicsSystemIdentifierAddress())==oldIdentifier && get(cmd-4)==beforeCommand && get(cmd+256)==afterCommand &&
            !std::memcmp(nextFrame.data(),space.translate(systemSp),480));
        const auto ticket=NativeSwapCommand::make({2,phys(cmd),phys(frontbuffer),2365});assert(ticket);
        assert(ticket->matches(phys(cmd),space.translate(cmd),256) && !registry.dispatch("__imp__VdSwap",issue).handled());
        assert(!registry.inspectStoppedPostOutputSwapRequest(nextSwap) && !registry.visitStoppedPostOutputSwapImage(nextSwap,nextVisit));
        // Caller stack may change after issuance. The pending ticket owns its
        // metadata/image; validation must never read that borrowed frame again.
        std::memset(space.translate(systemSp),0xB1,480);
        const auto submissionRoot=[&]{put(ring+2365*4,0xC0013F00);put(ring+2366*4,phys(cmd)-52);put(ring+2367*4,90);};
        submissionRoot();const auto pending=registry.inspectStoppedPostOutputSwapSubmission(2368);assert(pending &&
            pending->childPhysical==phys(cmd)-52 && pending->childWords==90 && pending->commandOffsetWords==13 &&
            pending->readWord==2365 && pending->writeWord==2368);
        assert(!registry.inspectStoppedNativePendingSubmission(2368) && !registry.submitNativeWrappedSwapCommand(2368) &&
            !registry.submitExperimentalNativeSwapCommand(2368) && !registry.submitNativeCommandStorageCoherency(2368));
        unsigned pendingRejects=0;
        const auto rejectPending=[&](unsigned next=2368){assert(!registry.inspectStoppedPostOutputSwapSubmission(next));++pendingRejects;};
        for(unsigned bit=0;bit<2048;++bit){space.translate(cmd)[bit/8]^=1u<<(bit%8);rejectPending();space.translate(cmd)[bit/8]^=1u<<(bit%8);}
        for(unsigned bit=0;bit<32;++bit){put(ring+2365*4,0xC0013F00^(1u<<bit));rejectPending();}submissionRoot();
        for(unsigned words:{0u,11u,63u,64u,1025u,UINT32_MAX}){put(ring+2367*4,words);rejectPending();}submissionRoot();
        for(unsigned next:{0u,2365u,2366u,2367u,2369u,UINT32_MAX})rejectPending(next);
        put(writeBack,2364);rejectPending();put(writeBack,2365);
        space.translate(frontbuffer)[0]^=1;rejectPending();space.translate(frontbuffer)[0]^=1;
        backend.active=false;rejectPending();backend.active=true;
        for(unsigned owner:{commandStorage,frontbuffer,ring,writeBack,*registry.graphicsSystemIdentifierAddress()}){
            GuestImportCall free;free.arguments={1,owner};assert(!registry.dispatch("__imp__MmFreePhysicalMemory",free).handled());
            GuestImportCall fill;fill.arguments={owner,4,0};assert(!registry.dispatch("__imp__RtlFillMemoryUlong",fill).handled());}
        assert(registry.inspectStoppedPostOutputSwapSubmission(2368) && get(writeBack)==2365 && displays==2 &&
            registry.nativeSwapCommandStatus().readWord==oldSwap.readWord && ticket->matches(phys(cmd),space.translate(cmd),256));
        std::printf("POST_OUTPUT_SWAP_TICKET: 439request refusals,%upending refusals,ticket2/256exact bytes/canaries,owned metadata,5free/fill guards,old history,no GPU/presentation PASS\n",pendingRejects);
        std::fflush(stdout);
        const auto storageChild=commandStorage+0x3A00;
        const std::array<unsigned,11> storageWords{0xA31,0x01000000,0x10A2F,0x4000,phys(commandStorage),0xC0043C00,3,0xA31,0,0x80000000,8};
        for(unsigned i=0;i<11;++i)put(storageChild+i*4,storageWords[i]);
        put(ring+2365*4,0xC0013F00);put(ring+2366*4,phys(storageChild));put(ring+2367*4,11);
        const auto oldFence=*registry.completedNativeCommandStorageCoherency();
        const auto storageIntact=[&]{assert(get(writeBack)==2365 && registry.graphicsRingConfiguration()->readWord==2365 &&
            !registry.completedNativePostOutputSwapStorageCoherency() && registry.nativePostOutputSwapStatus().pending &&
            registry.nativePostOutputSwapStatus().readWord==2365 && registry.nativePostOutputSwapStatus().issuedReadWord==2365 &&
            registry.nativeSwapCommandStatus().readWord==oldSwap.readWord &&
            registry.completedNativeCommandStorageCoherency()->readWord==oldFence.readWord && displays==2 &&
            get(*registry.graphicsSystemIdentifierAddress())==oldIdentifier && ticket->matches(phys(cmd),space.translate(cmd),256));};
        unsigned storageRefusals=0;
        const auto rejectStorage=[&](unsigned next=2368){assert(!registry.submitNativePostOutputSwapStorageCoherency(next) && !backend.swapStorageCalls);++storageRefusals;storageIntact();};
        for(unsigned i=0;i<11;++i)for(unsigned bit=0;bit<32;++bit){put(storageChild+i*4,storageWords[i]^(1u<<bit));rejectStorage();put(storageChild+i*4,storageWords[i]);}
        for(unsigned bit=0;bit<32;++bit){put(ring+2365*4,0xC0013F00^(1u<<bit));rejectStorage();put(ring+2365*4,0xC0013F00);}
        for(unsigned bit=0;bit<32;++bit){put(ring+2367*4,11^(1u<<bit));rejectStorage();put(ring+2367*4,11);}
        for(unsigned next:{0u,2365u,2366u,2367u,2369u,UINT32_MAX})rejectStorage(next);
        for(unsigned physical:{0u,phys(storageChild)+1,phys(cmd),phys(nextChild),phys(commandStorage)+0x3FE0,0x20000000u}){
            put(ring+2366*4,physical);rejectStorage();put(ring+2366*4,phys(storageChild));}
        backend.active=false;rejectStorage();backend.active=true;
        std::printf("POST_OUTPUT_SWAP_STORAGE_PREFLIGHT: %urefusals, no backend calls\n",storageRefusals);std::fflush(stdout);
        for(unsigned at:{storageChild-24,storageChild+40}){const auto child=fork();assert(child>=0);if(!child){
            alarm(5);
            GuestImportCall cs;cs.arguments={at,0};assert(registry.dispatch("__imp__RtlInitializeCriticalSectionAndSpinCount",cs).handled());
            assert(!registry.submitNativePostOutputSwapStorageCoherency(2368) && !backend.swapStorageCalls);std::_Exit(0);}
            int status=0;const auto waited=waitpid(child,&status,0);
            if(waited!=child || !WIFEXITED(status) || WEXITSTATUS(status))std::fprintf(stderr,"POST_SWAP_STORAGE_ALIAS: address=%08X raw_status=%d\n",at,status);
            assert(waited==child && WIFEXITED(status) && !WEXITSTATUS(status));}
        for(unsigned fault=0;fault<8;++fault){const auto child=fork();assert(child>=0);if(!child){
            alarm(5);
            backend.swapStorageAccept=fault!=0;backend.swapStorageThrow=fault==1;
            if(fault>=2)backend.swapStorageBefore=[&]{const std::array<unsigned,6> addresses{
                ring+2365*4,storageChild,cmd,frontbuffer,writeBack,nextSwap.argument32(2)};
                put(addresses[fault-2],get(addresses[fault-2])^1);};
            assert(!registry.submitNativePostOutputSwapStorageCoherency(2368));assert(fault<2);storageIntact();
            backend.swapStorageAccept=true;backend.swapStorageThrow=false;
            assert(!registry.submitNativePostOutputSwapStorageCoherency(2368) && backend.swapStorageCalls==1);std::_Exit(0);}
            int status=0;const auto waited=waitpid(child,&status,0);
            if(waited!=child || !WIFEXITED(status) || WEXITSTATUS(status)!=(fault<2?0:4))
                std::fprintf(stderr,"POST_SWAP_STORAGE_BACKEND: fault=%u raw_status=%d\n",fault,status);
            assert(waited==child && WIFEXITED(status) && WEXITSTATUS(status)==(fault<2?0:4));}
        backend.swapStorageAccept=true;assert(registry.submitNativePostOutputSwapStorageCoherency(2368));
        const auto stored=registry.completedNativePostOutputSwapStorageCoherency();assert(stored && stored->previousWord==2365 && stored->readWord==2368 &&
            stored->request.physicalBase==phys(commandStorage) && stored->request.byteCount==0x4000 && stored->request.cacheFlags==0x01000000 &&
            get(writeBack)==2368 && backend.swapStorageCalls==1 && displays==2 && registry.nativePostOutputSwapStatus().pending &&
            registry.nativePostOutputSwapStatus().readWord==2368 && registry.nativePostOutputSwapStatus().issuedReadWord==2365 &&
            registry.completedNativeCommandStorageCoherency()->readWord==oldFence.readWord && registry.nativeSwapCommandStatus().readWord==oldSwap.readWord &&
            ticket->matches(phys(cmd),space.translate(cmd),256));
        assert(!registry.submitNativePostOutputSwapStorageCoherency(2368) && backend.swapStorageCalls==1);
        put(ring+2368*4,0xC0013F00);put(ring+2369*4,phys(cmd)-52);put(ring+2370*4,90);
        assert(registry.inspectStoppedPostOutputSwapSubmission(2371));
        std::printf("POST_OUTPUT_SWAP_STORAGE: %urefusals,2partial-lock overlaps,2nonretry failures,6late fail-stops,fresh fence/ticket2 preserved/RPTR-last/no presentation PASS\n",storageRefusals);
        assert(!registry.inspectStoppedPostOutputSwapInterrupt(2371)); // Old metadata-only90word shape.
        const auto secondWrapper=cmd-40;
        const auto secondBytes=postOutputSwapBytes(*ticket,phys(writeBack)+0x40,0x4000,
            controlPhysical+0x108,controlPhysical+0x100,controlPhysical+0x104);
        std::memcpy(space.translate(secondWrapper),secondBytes.data(),secondBytes.size());
        put(ring+2369*4,phys(secondWrapper));put(ring+2370*4,129);
        // Diagnostic snapshots report real fields, even outstanding locks or
        // counters. They neither execute the callback nor repair its state.
        for(unsigned i=0;i<32;++i)put(device+16980+i*4,0xCAFE0000+i);
        put(device+16972,7);put(device+17112,19);put(device+21964,60);
        const auto oldScope=registry.nativeCommandInterruptScopeStatus();
        const auto oldThreads=registry.createdThreadCount();
        const auto snapshot=registry.inspectStoppedPostOutputSwapInterrupt(2371);assert(snapshot &&
            snapshot->device==device && snapshot->handler==0x4000 && snapshot->submission.readWord==2368 &&
            snapshot->wrapper.callbackContext()==(phys(frontbuffer)|0x208));
        unsigned queuedReason=0;
        assert(!registry.inspectStoppedPostOutputSwapQueuedInterrupt(2371,&queuedReason) && queuedReason==22);
        std::array<std::uint8_t,23792> deviceBefore{};
        std::memcpy(deviceBefore.data(),space.translate(device),deviceBefore.size());
        std::array<std::uint8_t,32> scratchBefore{};std::memcpy(scratchBefore.data(),space.translate(writeBack+0x40),32);
        for(unsigned i=0;i<snapshot->DeviceOffsets.size();++i)assert(snapshot->deviceValues[i]==get(device+snapshot->DeviceOffsets[i]));
        for(unsigned i=0;i<32;++i)assert(snapshot->queuedSurfaces[i]==0xCAFE0000+i);
        for(unsigned i=0;i<8;++i)assert(snapshot->scratch[i]==get(writeBack+0x40+i*4));
        // Reproduce the report-lifetime bug: a new inspection after resolver
        // reset must refuse, but the earlier owned snapshot remains readable.
        *mappingLive=false;unsigned mappingReason=0;
        assert(!registry.inspectStoppedPostOutputSwapInterrupt(2371,&mappingReason) && mappingReason==4 &&
            snapshot->handler==0x4000 && snapshot->deviceValues[6]==7 && snapshot->queuedSurfaces[31]==0xCAFE001F);
        *mappingLive=true;mappingReason=999;
        assert(registry.inspectStoppedPostOutputSwapInterrupt(2371,&mappingReason) && !mappingReason);
        std::puts("STOPPED_POST_SWAP_SNAPSHOT_LIFETIME: capture before resolver reset,reason4 after reset,owned14fields retained,no dispatch reenabled PASS");
        unsigned inspectorRefusals=0;
        const auto refuseSnapshot=[&](unsigned next=2371){unsigned reason=0;
            assert(!registry.inspectStoppedPostOutputSwapInterrupt(next,&reason) && reason && reason<=21);++inspectorRefusals;};
        for(unsigned next:{0u,2368u,2370u,2372u,UINT32_MAX})refuseSnapshot(next);
        for(unsigned at:{ring+2368*4,ring+2369*4,ring+2370*4,cmd,writeBack,frontbuffer,device+11028,0x7200u}){
            const auto before=get(at);put(at,before^1);refuseSnapshot();put(at,before);}
        for(unsigned word=74;word<129;++word){if(word==123 || word==124 || word==127 || word==128)continue;
            const auto at=secondWrapper+word*4,before=get(at);put(at,before^1);refuseSnapshot();put(at,before);}
        backend.active=false;refuseSnapshot();backend.active=true;
        // Valid syntax is insufficient when event storage aliases root/cursor,
        // identifier, command owner, or lies outside any physical allocation.
        for(unsigned physical:{phys(ring),phys(writeBack),controlPhysical,phys(commandStorage)+0x3FFCu,0x1000u}){
            put(secondWrapper+123*4,physical+2);refuseSnapshot();put(secondWrapper+123*4,controlPhysical+0x102);}
        unsigned inspectorAliases=0;
        const std::array<std::pair<unsigned,unsigned>,7> inspectedRanges{{{secondWrapper,516},{device+11028,8},
            {device+16944,180},{device+21964,4},{device+23788,4},{writeBack+0x40,32},{event0,4}}};
        for(const auto& span:inspectedRanges)for(unsigned at:{span.first-24,span.first+span.second-4}){
            std::fflush(nullptr);const auto child=fork();assert(child>=0);if(!child){alarm(5);
                GuestImportCall cs;cs.arguments={at,0};assert(registry.dispatch("__imp__RtlInitializeCriticalSectionAndSpinCount",cs).handled());
                assert(!registry.inspectStoppedPostOutputSwapInterrupt(2371));std::_Exit(0);}
            int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));++inspectorAliases;}
        for(unsigned offset:{11032u,17120u}){put(device+offset,1);const auto observed=registry.inspectStoppedPostOutputSwapInterrupt(2371);
            assert(observed && get(device+offset)==1);put(device+offset,0);}
        assert(registry.inspectStoppedPostOutputSwapInterrupt(2371) && !std::memcmp(deviceBefore.data(),space.translate(device),deviceBefore.size()) &&
            !std::memcmp(scratchBefore.data(),space.translate(writeBack+0x40),32) && !std::memcmp(secondBytes.data(),space.translate(secondWrapper),516) &&
            registry.createdThreadCount()==oldThreads && registry.nativeCommandInterruptScopeStatus().worker==oldScope.worker &&
            registry.nativeCommandInterruptScopeStatus().lockPairs==oldScope.lockPairs && backend.swapStorageCalls==1 && displays==2 &&
            get(writeBack)==2368 && registry.nativePostOutputSwapStatus().pending && registry.nativeSwapCommandStatus().readWord==oldSwap.readWord &&
            get(*registry.graphicsSystemIdentifierAddress())==oldIdentifier);
        assert(inspectorRefusals==70 && inspectorAliases==14);
        std::puts("STOPPED_POST_OUTPUT_SWAP_INTERRUPT:70refusals,14partial-lock overlaps,owned device/16queue pairs/8scratch slots,old IRQ and display history preserved,no execution PASS");
        unsigned secondEnqueues=0;
        const auto forbiddenWorker=[&](const auto&,const auto&)->bool{++secondEnqueues;assert(false);return false;};
        assert(!registry.nativePostOutputSwapInterruptWorker() && !registry.createNativePostOutputSwapInterruptWorker(2371,{}));
        for(unsigned next:{0u,2368u,2370u,2372u,UINT32_MAX})assert(!registry.createNativePostOutputSwapInterruptWorker(next,forbiddenWorker));
        constexpr auto issuerToken=0x31000000u; // Actual system-context/swap issuer in this fixture.
        assert(registry.bindGuestThreadToken(other->layout.threadAddress));
        assert(!registry.createNativePostOutputSwapInterruptWorker(2371,forbiddenWorker));
        assert(registry.bindGuestThreadToken(interrupt->layout.threadAddress));
        assert(!registry.createNativePostOutputSwapInterruptWorker(2371,forbiddenWorker));
        assert(registry.bindGuestThreadToken(worker->layout.threadAddress));
        assert(!registry.createNativePostOutputSwapInterruptWorker(2371,forbiddenWorker));
        assert(registry.bindGuestThreadToken(issuerToken));
        *mappingLive=false;assert(!registry.createNativePostOutputSwapInterruptWorker(2371,forbiddenWorker));*mappingLive=true;
        put(writeBack,2367);assert(!registry.createNativePostOutputSwapInterruptWorker(2371,forbiddenWorker));put(writeBack,2368);
        assert(secondEnqueues==0 && registry.createdThreadCount()==oldThreads);
        for(bool throws:{false,true}){
            GuestPreparedThread prepared;bool caught=false;
            try{assert(!registry.createNativePostOutputSwapInterruptWorker(2371,[&](const auto& candidate,const auto& entry)->bool{
                ++secondEnqueues;prepared=candidate;
                assert(entry.address==0x4000 && entry.argument0==1 && entry.argument1==device && entry.directEntry &&
                    entry.pcrAddress==candidate.layout.pcrAddress && candidate.layout.pcrAddress!=interrupt->layout.pcrAddress &&
                    !candidate.wrappedContextAddress && *space.translate(candidate.layout.pcrAddress+0x10C)==2 &&
                    *space.translate(candidate.layout.threadAddress+0xBF)==2);
                if(throws)throw std::runtime_error("second IRQ enqueue");return false;
            }));}catch(const std::runtime_error&){caught=true;}
            assert(caught==throws && prepared.allocation.size && registry.createdThreadCount()==oldThreads && !registry.nativePostOutputSwapInterruptWorker());
            assert(std::all_of(space.translate(prepared.allocation.base),space.translate(prepared.allocation.base)+prepared.allocation.size,[](auto v){return !v;}));
        }
        const auto freshWorker=registry.createNativePostOutputSwapInterruptWorker(2371,[&](const auto& candidate,const auto& entry){
            ++secondEnqueues;return candidate.layout.threadAddress!=interrupt->layout.threadAddress && entry.argument0==1 && entry.argument1==device;
        });assert(freshWorker && secondEnqueues==3 && registry.createdThreadCount()==oldThreads+1 &&
            registry.nativePostOutputSwapInterruptWorker()==freshWorker->layout.threadAddress);
        assert(!registry.createNativePostOutputSwapInterruptWorker(2371,forbiddenWorker));
        assert(registry.bindGuestThreadToken(freshWorker->layout.threadAddress));
        assert(!registry.runNativeCommandInterruptScope(2371,[]{assert(false);})); // Old scaler scope cannot execute ticket2.
        assert(registry.bindGuestThreadToken(issuerToken));
        assert(registry.nativeCommandInterruptScopeStatus().worker==oldScope.worker &&
            registry.nativeCommandInterruptScopeStatus().lockPairs==oldScope.lockPairs && registry.nativeCommandInterruptScopeStatus().balanced &&
            !std::memcmp(deviceBefore.data(),space.translate(device),deviceBefore.size()) &&
            !std::memcmp(scratchBefore.data(),space.translate(writeBack+0x40),32) && displays==2 && get(writeBack)==2368);
        std::puts("POST_OUTPUT_SWAP_INTERRUPT_WORKER: freshCPU2/direct source1 ABI,issuer/mapping/cursor guards,2enqueue rollbacks,old scope refused/history preserved; no guest/GPU/presentation PASS");
        assert(!registry.runNativePostOutputSwapInterruptScope(2371,[]{assert(false);}));
        assert(registry.bindGuestThreadToken(freshWorker->layout.threadAddress));
        assert(!registry.runNativePostOutputSwapInterruptScope(2371,[]{assert(false);}) &&
            registry.nativePostOutputSwapInterruptScopeStatus().rejection==4);
        // Explicit synthetic empty-queue profile, not a claim that guest code
        // performed these writes. D-289 separately executes original bodies.
        for(unsigned i=0;i<32;++i)put(device+16980+i*4,0);
        for(unsigned offset:{16952u,16956u,17112u,17108u})put(device+offset,0);
        for(unsigned offset:{16960u,16972u,16976u})put(device+offset,1);
        put(device+16944,0x83216908);put(writeBack+0x44,0x11223344);
        backend.nativeScratchBase=phys(writeBack)+0x40;
        assert(!registry.runNativePostOutputSwapInterruptScope(2371,[]{assert(false);}) &&
            registry.nativePostOutputSwapInterruptScopeStatus().rejection==5);
        assert(!registry.inspectStoppedPostOutputSwapQueuedInterrupt(2371,&queuedReason) && queuedReason==23);
        constexpr unsigned secondHeader=0x7400,secondRecord=0x83BFBE40;
        std::memset(space.translate(secondHeader),0,0x400);
        put(secondHeader,0x58455832);put(secondHeader+8,0x400);put(secondHeader+16,0x80);
        put(secondHeader+0x80,0x184+24);put(secondHeader+0x84,0x10000);
        put(secondHeader+0x190,0x83BF0000);put(secondHeader+0x200,1);put(secondHeader+0x204,0x12);
        assert(registry.bindImagePageProtections(secondHeader,0x400,0x83BF0000,0x10000));
        const auto queuedCandidate=registry.inspectStoppedPostOutputSwapQueuedInterrupt(2371,&queuedReason);
        assert(queuedCandidate && !queuedReason && !registry.nativePostOutputSwapInterruptPhase() &&
            queuedCandidate->wrapper.sameRequest(snapshot->wrapper) && registry.createdThreadCount()==oldThreads+1 && displays==2);
        for(unsigned i=0;i<4;++i){const auto savedBase=backend.nativeScratchBase,savedMask=backend.nativeScratchMask;
            if(i==0)backend.nativeScratchBase=0;
            if(i==1)backend.nativeScratchBase^=4;
            if(i==2)backend.nativeScratchMask^=1;
            if(i==3)backend.nativeScratchMask^=0x20000;
            assert(!registry.inspectStoppedPostOutputSwapQueuedInterrupt(2371,&queuedReason) && queuedReason==24);
            backend.nativeScratchBase=savedBase;backend.nativeScratchMask=savedMask;
        }
        for(unsigned at:{device+11032,device+16972,device+17112,device+16980,writeBack+0x40,writeBack+0x50}){
            const auto value=get(at);put(at,value^1);
            assert(registry.inspectStoppedPostOutputSwapInterrupt(2371) &&
                !registry.inspectStoppedPostOutputSwapQueuedInterrupt(2371,&queuedReason) && queuedReason==22);
            put(at,value);
        }
        put(device+16944,0x4000);
        assert(!registry.inspectStoppedPostOutputSwapQueuedInterrupt(2371,&queuedReason) && queuedReason==23);
        put(device+16944,0x83216908);put(writeBack,2367);
        assert(!registry.inspectStoppedPostOutputSwapQueuedInterrupt(2371,&queuedReason) && queuedReason==1);put(writeBack,2368);
        assert(registry.inspectStoppedPostOutputSwapQueuedInterrupt(2371,&queuedReason) && !queuedReason &&
            queuedCandidate->deviceValues[6]==1 && get(writeBack)==2368 && get(writeBack+0x40)==0 && get(writeBack+0x44)==0x11223344);
        std::puts("STOPPED_POST_SWAP_QUEUED_ADMISSION:raw/profile/leaf/scratch reasons,12mutation refusals,owned snapshot/no lease,no guest/GPU/presentation PASS");
        unsigned newScopeRefusals=0;
        const auto refuseFresh=[&](unsigned next=2371){
            assert(!registry.runNativePostOutputSwapInterruptScope(next,[]{assert(false);}) &&
                !registry.nativePostOutputSwapInterruptScopeStatus().scope.attempted);++newScopeRefusals;};
        for(unsigned next:{0u,2368u,2370u,2372u,UINT32_MAX})refuseFresh(next);
        for(unsigned offset:{11028u,11032u,16944u,16952u,16956u,16960u,16972u,16976u,17112u,17120u,21964u,17108u}){
            const auto value=get(device+offset);put(device+offset,value^1);refuseFresh();put(device+offset,value);}
        for(unsigned i=0;i<32;++i){put(device+16980+i*4,1);refuseFresh();put(device+16980+i*4,0);}
        for(unsigned at:{writeBack+0x40,writeBack+0x50,writeBack+0x54,secondWrapper,secondWrapper+124*4,writeBack,0x7200u}){
            const auto value=get(at);put(at,value^1);
            // A changed literal remains valid before any scope captures it.
            if(at!=secondWrapper+124*4)refuseFresh();
            put(at,value);}
        for(unsigned token:{issuerToken,worker->layout.threadAddress,interrupt->layout.threadAddress,other->layout.threadAddress}){
            assert(registry.bindGuestThreadToken(token));refuseFresh();}
        assert(registry.bindGuestThreadToken(freshWorker->layout.threadAddress));
        for(unsigned at:{freshWorker->layout.pcrAddress+0x10C,freshWorker->layout.threadAddress+0xBF}){
            *space.translate(at)=3;refuseFresh();*space.translate(at)=2;}
        *pacingMapping=false;refuseFresh();*pacingMapping=true;
        *mappingLive=false;refuseFresh();*mappingLive=true;
        for(unsigned at:{secondRecord-24,secondRecord+20}){std::fflush(nullptr);const auto nested=fork();assert(nested>=0);
            if(!nested){alarm(5);GuestImportCall cs;cs.arguments={at,0};
                assert(registry.dispatch("__imp__RtlInitializeCriticalSectionAndSpinCount",cs).handled());
                GuestImportCall enter;enter.arguments={at};assert(registry.dispatch("__imp__RtlEnterCriticalSection",enter).handled());
                refuseFresh();std::_Exit(0);}
            int result=0;assert(waitpid(nested,&result,0)==nested && WIFEXITED(result) && !WEXITSTATUS(result));}
        unsigned nativeActions=0;
        const NativePostOutputSwapInterruptSequence::Action nativeAction=[&](const NativePostOutputSwapWrapper::Operation&){++nativeActions;return true;};
        const auto simulateQueuedCallback=[&](unsigned fault=UINT32_MAX,const NativePostOutputSwapInterruptSequence::Action* routed=nullptr){
            const auto& command=routed?*routed:nativeAction;
            assert(!registry.inspectStoppedPostOutputSwapInterrupt(2371)); // Public snapshot is still stopped-only.
            assert(!registry.inspectStoppedPostOutputSwapQueuedInterrupt(2371));
            assert(!registry.runNativeCommandInterruptScope(2371,[]{assert(false);}));
            assert(!registry.runNativePostOutputSwapInterruptScope(2371,[]{assert(false);}));
            GuestImportCall forbidden;forbidden.arguments={0};
            assert(!registry.dispatch("__imp__KeSetEvent",forbidden).handled());
            GuestImportCall query;assert(registry.dispatch("__imp__KeQueryPerformanceFrequency",query).handled() && query.returnValue==50000000);
            if(fault==1)throw std::runtime_error("fresh IRQ guest stand-in");
            // Real registry prefix, mock native fence/action and guest body.
            assert(registry.beginNativePostOutputSwapInterrupt(2371,command));
            assert(!registry.beginNativePostOutputSwapInterrupt(2371,command));
            assert(get(writeBack+0x40)==4 && get(writeBack+0x50)==0x4000 && get(writeBack+0x54)==(phys(frontbuffer)|0x208));
            assert(!registry.acknowledgeNativePostOutputSwapInterrupt(2371,command));
            if(fault==2){invokeLock(0,device+17120,true);return;}
            if(fault==10)return;
            balancedLocks();
            if(fault==0)return;
            put(device+16972,2);put(device+16960,2);put(device+17112,1);
            put(device+16980,phys(frontbuffer));put(device+16984,2);
            const std::array<unsigned,6> record{1,2,0,1,50,2};
            for(unsigned i=0;i<record.size();++i)put(secondRecord+i*4,record[i]);
            put(writeBack+0x40,0);
            if(fault==3)put(device+16984,3);
            if(fault==4)put(writeBack+0x40,4);
            if(fault==5)put(writeBack+0x44,0);
            if(fault==6)put(secondRecord+16,101);
            if(fault==7)put(secondWrapper+124*4,get(secondWrapper+124*4)^1);
            if(fault==8)assert(registry.bindGuestThreadToken(other->layout.threadAddress));
            if(fault==9)put(device+23788,get(device+23788)^1);
            if(fault==11)put(device+17112,0);
            if(fault==12)put(writeBack+0x54,0x208);
            if(fault==13)*mappingLive=false;
            if(fault==14)put(0x7200,device^4);
        };
        // Full transaction in fork-isolated CPU fixtures. The production
        // runner must perform this handoff with its real exclusive executor.
        const GuestGraphicsBackend::NativePostOutputSwapInterrupt completeIrq=[&](const auto& action){
            assert(registry.bindGuestThreadToken(freshWorker->layout.threadAddress));
            const bool ran=registry.runNativePostOutputSwapInterruptScope(2371,[&]{simulateQueuedCallback(UINT32_MAX,&action);});
            const bool acknowledged=ran && registry.acknowledgeNativePostOutputSwapInterrupt(2371,action);
            const bool returned=acknowledged && registry.markGuestThreadReturned(freshWorker->layout.threadAddress,0);
            assert(registry.bindGuestThreadToken(issuerToken));return returned;
        };
        assert(!registry.submitNativePostOutputWrappedSwapCommand(2371,completeIrq)); // Not original issuer.
        assert(registry.bindGuestThreadToken(issuerToken));
        put(sp+80,0);put(sp+84,0);
        assert(registry.bindGuestThreadToken(worker->layout.threadAddress));
        control("__imp__KeEnterCriticalRegion");control("__imp__RtlEnterCriticalSection",TrainingLock);
        auto noNewScanout=query;const auto beforeNewQuery=backend.trainingCalls;
        assert(!registry.dispatch(Retrain,noNewScanout).handled() && noNewScanout.returnValue==query.returnValue &&
            backend.trainingCalls==beforeNewQuery); // Pending ticket is not current scanout.
        control("__imp__RtlLeaveCriticalSection",TrainingLock);control("__imp__KeLeaveCriticalRegion");
        assert(registry.bindGuestThreadToken(issuerToken));
        unsigned transactionRefusals=0;
        const auto refuseTransaction=[&](unsigned word=2371){
            const auto unchangedRead=get(writeBack);
            const auto submitted=registry.submitNativePostOutputWrappedSwapCommand(word,completeIrq);
            if(submitted || backend.secondSwapCalls || registry.completedNativePostOutputWrappedSwap() ||
                    get(writeBack)!=unchangedRead || displays!=2)
                std::fprintf(stderr,"POST_SWAP_TRANSACTION_REFUSAL: case=%u submitted=%u calls=%u read=%u expected=%u displays=%u\n",
                    transactionRefusals,submitted,backend.secondSwapCalls,get(writeBack),unchangedRead,displays);
            assert(!submitted && !backend.secondSwapCalls && !registry.completedNativePostOutputWrappedSwap() &&
                get(writeBack)==unchangedRead && displays==2);++transactionRefusals;};
        for(unsigned word:{0u,2368u,2370u,2372u,UINT32_MAX})refuseTransaction(word);
        for(unsigned at:{ring+2368*4,ring+2369*4,ring+2370*4,secondWrapper,cmd,frontbuffer,writeBack,
                device+11028,device+16972,device+17112,device+16980,writeBack+0x40,writeBack+0x50,0x7200u}){
            const auto value=get(at);put(at,value^1);refuseTransaction();put(at,value);}
        const auto oldBase=backend.nativeScratchBase;backend.nativeScratchBase=0;refuseTransaction();backend.nativeScratchBase=oldBase;
        const auto oldMask=backend.nativeScratchMask;backend.nativeScratchMask^=1;refuseTransaction();backend.nativeScratchMask=oldMask;
        for(unsigned physical:{phys(ring),phys(writeBack),controlPhysical,phys(commandStorage)+0x3FFCu,0x1000u}){
            put(secondWrapper+123*4,physical+2);refuseTransaction();put(secondWrapper+123*4,controlPhysical+0x102);}
        for(unsigned fault=0;fault<25;++fault){std::fflush(nullptr);const auto nested=fork();assert(nested>=0);
            if(!nested){alarm(10);backend.secondSwapFault=fault;
                consumer=[&](const auto& image)->bool{++displays;
                    assert(image.physicalAddress==phys(frontbuffer) && image.request.commandVirtual==cmd &&
                        image.rgba.size()==NativeSwapImagePlan::PixelBytes && get(writeBack)==2368 && get(writeBack+0x40)==0);
                    for(unsigned i=0;i<image.rgba.size();++i)assert(image.rgba[i]==(i%4==3?255:0));
                    if(fault==5)throw std::runtime_error("second swap consumer");return fault!=4;
                };
                if(fault>=15)backend.secondSwapAfter=[&]{const std::array<unsigned,10> changed{
                    secondWrapper+124*4,ring+2368*4,frontbuffer,event0,writeBack,device+16980,writeBack+0x44,secondRecord+16,device+23788,cmd};
                    put(changed[fault-15],get(changed[fault-15])^1);};
                const auto complete=registry.submitNativePostOutputWrappedSwapCommand(2371,completeIrq);
                const bool preaccept=fault==0 || fault==1 || fault==2 || fault==4 || fault==5 || fault==11;
                assert(fault==3 || fault==14 || preaccept); // All other accepted contradictions must fail-stop4.
                if(preaccept){assert(!complete && !registry.completedNativePostOutputWrappedSwap() && get(writeBack)==2368 &&
                    registry.nativePostOutputSwapStatus().pending && !registry.nativePostOutputSwapStatus().presentQueued);
                }else{const auto receipt=registry.completedNativePostOutputWrappedSwap();
                    assert(complete && receipt && receipt->previousWord==2368 && receipt->readWord==2371 &&
                        receipt->gpu.nativeFences==6 && receipt->gpu.interruptAcknowledged && receipt->imageRequest.commandVirtual==cmd &&
                        receipt->imageOwner.virtualBase==frontbuffer && get(writeBack)==2371 && registry.graphicsRingConfiguration()->readWord==2371 &&
                        !registry.nativePostOutputSwapStatus().pending && registry.nativePostOutputSwapStatus().presentQueued &&
                        registry.nativePostOutputSwapStatus().issuedReadWord==2365 && backend.secondSwapActions==11 && displays==3 &&
                        get(event0)==0x12345678 && get(event1)==0xABCDEF01 && get(*registry.graphicsSystemIdentifierAddress())==oldIdentifier &&
                        registry.nativeSwapCommandStatus().readWord==oldSwap.readWord && registry.nativeCommandInterruptScopeStatus().worker==oldScope.worker);
                    const auto trainingStart=registry.nativeEdramTrainingState();const auto queryStart=backend.trainingCalls;
                    const std::vector<std::uint8_t> savedStack(space.translate(sp),space.translate(sp+2144));
                    unsigned refusals=0,refusedProbes=0;
                    const auto refuse=[&](GuestImportCall c,bool probesBackend=false){
                        refusedProbes+=probesBackend;
                        assert(!registry.dispatch(Retrain,c).handled() && c.returnValue==query.returnValue &&
                            backend.trainingCalls==queryStart+refusedProbes && registry.nativeEdramTrainingState().directNoCommandQueries==trainingStart.directNoCommandQueries);
                        ++refusals;};
                    refuse(query);assert(registry.bindGuestThreadToken(worker->layout.threadAddress));
                    control("__imp__KeEnterCriticalRegion");refuse(query);
                    control("__imp__RtlEnterCriticalSection",TrainingLock);
                    for(unsigned arg=0;arg<6;++arg)for(unsigned bit=0;bit<64;++bit){auto bad=query;bad.arguments[arg]^=1ULL<<bit;refuse(bad);}
                    for(unsigned at:{sp+80,sp+84})for(unsigned bit=0;bit<32;++bit){put(at,1u<<bit);refuse(query);assert(get(at)==(1u<<bit));put(at,0);}
                    for(unsigned at:{frontbuffer,frontbuffer+4,writeBack}){const auto saved=get(at);put(at,saved^1);refuse(query);put(at,saved);}
                    backend.active=false;refuse(query);backend.active=true;
                    backend.trainingAvailable=false;refuse(query,true);backend.trainingAvailable=true;
                    backend.throws=true;refuse(query,true);backend.throws=false;
                    assert(registry.bindGuestThreadToken(other->layout.threadAddress));refuse(query);
                    assert(registry.bindGuestThreadToken(worker->layout.threadAddress));
                    auto directAgain=query;const auto directResult=registry.dispatch(Retrain,directAgain);
                    if(!directResult.handled())std::fprintf(stderr,"POST_SWAP_EDRAM_REFUSAL: %s\n",directResult.diagnostic.c_str());
                    assert(directResult.handled() && !directAgain.returnValue &&
                        registry.nativeEdramTrainingState().directNoCommandQueries==trainingStart.directNoCommandQueries+1 &&
                        backend.trainingCalls==queryStart+refusedProbes+1 && std::equal(savedStack.begin(),savedStack.end(),space.translate(sp)) &&
                        get(writeBack)==2371 && displays==3 && backend.secondSwapActions==11 &&
                        registry.nativeSwapCommandStatus().readWord==oldSwap.readWord);
                    const auto reused=fork();assert(reused>=0);
                    if(!reused){
                        const auto callsBeforeFree=backend.trainingCalls;
                        const auto bytes=receipt->imageOwner.byteCount;
                        const std::vector<std::uint8_t> savedImage(space.translate(frontbuffer),space.translate(frontbuffer)+bytes);
                        GuestImportCall free;free.arguments={1,frontbuffer};
                        assert(registry.dispatch("__imp__MmFreePhysicalMemory",free).handled());
                        auto absent=query;assert(!registry.dispatch(Retrain,absent).handled() && backend.trainingCalls==callsBeforeFree);
                        assert(allocate(bytes)==frontbuffer);std::memcpy(space.translate(frontbuffer),savedImage.data(),bytes);
                        auto same=query;assert(!registry.dispatch(Retrain,same).handled() && same.returnValue==query.returnValue &&
                            backend.trainingCalls==callsBeforeFree && registry.completedNativePostOutputWrappedSwap() && get(writeBack)==2371);
                        std::_Exit(0);
                    }
                    int reusedStatus=0;assert(waitpid(reused,&reusedStatus,0)==reused && WIFEXITED(reusedStatus) && !WEXITSTATUS(reusedStatus));
                    control("__imp__RtlLeaveCriticalSection",TrainingLock);control("__imp__KeLeaveCriticalRegion");
                    assert(registry.bindGuestThreadToken(issuerToken));
                    std::printf("POST_OUTPUT_SWAP_EDRAM_QUERY: %urefusals,2free/reuse controls,independent new-image lease,owned active pixels/RPTR/stack,one fresh capability query,no commands/frames/old receipt changes PASS\n",refusals);
                    std::fflush(nullptr);
                    std::memset(space.translate(systemSp),0xD3,480);
                    const std::vector<std::uint8_t> oldFrame(space.translate(systemSp),space.translate(systemSp)+480);
                    const auto oldPostContext=*registry.nativePostOutputSystemContext();unsigned contextRefusals=0;
                    const auto refuseNextContext=[&](GuestImportCall call){
                        assert(!registry.dispatch("__imp__VdGetSystemCommandBuffer",call).handled() && call.returnValue==nextSystem.returnValue &&
                            !registry.nativePostSwapSystemContext() && !std::memcmp(oldFrame.data(),space.translate(systemSp),480));++contextRefusals;};
                    for(unsigned arg=0;arg<2;++arg)for(unsigned bit=0;bit<64;++bit){auto bad=nextSystem;bad.arguments[arg]^=1ULL<<bit;refuseNextContext(bad);}
                    for(unsigned frame:{0u,systemSp+16,sp,UINT32_MAX}){auto bad=nextSystem;bad.stackPointer=frame;
                        bad.arguments={std::uint32_t(frame+208),std::uint32_t(frame+116)};refuseNextContext(bad);}
                    for(unsigned at:{frontbuffer,writeBack}){const auto saved=get(at);put(at,saved^1);refuseNextContext(nextSystem);put(at,saved);}
                    backend.active=false;refuseNextContext(nextSystem);backend.active=true;
                    assert(registry.bindGuestThreadToken(worker->layout.threadAddress));refuseNextContext(nextSystem);
                    assert(registry.bindGuestThreadToken(issuerToken));
                    for(unsigned offset:{0u,108u,200u,476u}){const auto alias=fork();assert(alias>=0);
                        if(!alias){GuestImportCall cs;cs.arguments={systemSp+offset,0};
                            assert(registry.dispatch("__imp__RtlInitializeCriticalSectionAndSpinCount",cs).handled());
                            const std::vector<std::uint8_t> aliased(space.translate(systemSp),space.translate(systemSp)+504);
                            auto call=nextSystem;assert(!registry.dispatch("__imp__VdGetSystemCommandBuffer",call).handled() &&
                                !registry.nativePostSwapSystemContext() && !std::memcmp(aliased.data(),space.translate(systemSp),aliased.size()));std::_Exit(0);}
                        int status=0;assert(waitpid(alias,&status,0)==alias && WIFEXITED(status) && !WEXITSTATUS(status));}
                    auto thirdContext=nextSystem;assert(registry.dispatch("__imp__VdGetSystemCommandBuffer",thirdContext).handled() &&
                        thirdContext.returnValue==nextSystem.returnValue && registry.nativePostSwapSystemContext()->readWord==2371 &&
                        registry.nativePostOutputSystemContext()->readWord==oldPostContext.readWord &&
                        registry.nativeSystemContext()->readWord==oldContext.readWord && get(writeBack)==2371 && displays==3);
                    for(unsigned i=0;i<480;++i){const bool changed=(i>=116 && i<120) || (i>=208 && i<356);
                        assert(space.translate(systemSp)[i]==(changed?0:oldFrame[i]));}
                    const std::vector<std::uint8_t> afterContext(space.translate(systemSp),space.translate(systemSp)+480);
                    assert(!registry.dispatch("__imp__VdGetSystemCommandBuffer",thirdContext).handled() &&
                        !std::memcmp(afterContext.data(),space.translate(systemSp),480));
                    auto thirdSwap=nextSwap;assert(!registry.dispatch("__imp__VdSwap",thirdSwap).handled());
                    std::printf("POST_SECOND_SWAP_CONTEXT: %urefusals,4partial-lock aliases,152bytes/canaries,one-shot,three separate histories,no third swap/frame PASS\n",contextRefusals);
                    std::fflush(nullptr);
                    const auto inspectCall=prepareNextSwap(nextCopy); // Different image: raw metadata, NOT published pixels.
                    const auto snapshot=registry.inspectStoppedPostSwapRequest(inspectCall);
                    assert(snapshot && snapshot->frontbufferVirtual==nextCopy && snapshot->commandVirtual==cmd &&
                        !registry.inspectStoppedPostOutputSwapRequest(inspectCall));
                    const std::vector<std::uint8_t> frameSnapshot(space.translate(systemSp),space.translate(systemSp)+480);
                    const std::vector<std::uint8_t> ticketSnapshot(space.translate(cmd),space.translate(cmd)+256);
                    unsigned requestRefusals=0;
                    const auto refuseRequest=[&](const GuestImportCall& call){assert(!registry.inspectStoppedPostSwapRequest(call));++requestRefusals;};
                    for(unsigned arg=1;arg<8;++arg)for(unsigned bit=0;bit<32;++bit){auto bad=inspectCall;bad.arguments[arg]^=1u<<bit;refuseRequest(bad);}
                    auto shifted=inspectCall;shifted.stackPointer+=16;refuseRequest(shifted);
                    for(unsigned at:{systemSp+84,systemSp+92,systemSp+116,writeBack,frontbuffer}){
                        const auto saved=get(at);put(at,saved^1);refuseRequest(inspectCall);put(at,saved);}
                    for(unsigned i=0;i<148;++i){space.translate(systemSp+208)[i]=1;refuseRequest(inspectCall);space.translate(systemSp+208)[i]=0;}
                    for(unsigned command:{0u,ring,writeBack,0x7FC80000u,cmd+1,commandStorage+0x3FFCu}){
                        auto bad=inspectCall;bad.arguments[0]=command;refuseRequest(bad);}
                    assert(registry.inspectStoppedPostSwapRequest(inspectCall) && snapshot->frontbufferVirtual==nextCopy &&
                        !std::memcmp(frameSnapshot.data(),space.translate(systemSp),480) && !std::memcmp(ticketSnapshot.data(),space.translate(cmd),256) &&
                        get(writeBack)==2371 && displays==3 && registry.nativePostOutputSwapStatus().issuedReadWord==2365);
                    auto notIssued=inspectCall;assert(!registry.dispatch("__imp__VdSwap",notIssued).handled());
                    std::printf("STOPPED_POST_SWAP_REQUEST: %urefusals,owned raw metadata/different requested image,no decode/ticket3/writes/presentation PASS\n",requestRefusals);
                    std::fflush(nullptr);
                    auto deferred=inspectCall;deferred.arguments[0]=cmd+0x600;deferred.returnValue=0x1122334455667788ull;
                    const auto newCommand=deferred.argument32(0);
                    std::memset(space.translate(newCommand-4),0x91,264);
                    const std::vector<std::uint8_t> newBytes(space.translate(newCommand-4),space.translate(newCommand)+260);
                    const std::vector<std::uint8_t> unrendered(space.translate(nextCopy),space.translate(nextCopy)+0x400000);
                    unsigned deferredRefusals=0;
                    const auto refuseDeferred=[&](GuestImportCall call){
                        const std::vector<std::uint8_t> before(space.translate(systemSp),space.translate(systemSp)+480);
                        const auto wb=get(writeBack);
                        assert(!registry.dispatch("__imp__VdSwap",call).handled() && !registry.nativeDeferredSwapRequest() &&
                            call.returnValue==deferred.returnValue && get(writeBack)==wb && displays==3 &&
                            !std::memcmp(before.data(),space.translate(systemSp),480) &&
                            !std::memcmp(newBytes.data(),space.translate(newCommand-4),264));++deferredRefusals;};
                    for(unsigned arg=1;arg<8;++arg)for(unsigned bit=0;bit<64;++bit){auto bad=deferred;bad.arguments[arg]^=1ULL<<bit;refuseDeferred(bad);}
                    for(unsigned bit=32;bit<64;++bit){auto bad=deferred;bad.arguments[0]^=1ULL<<bit;refuseDeferred(bad);}
                    for(unsigned at:{systemSp+96,systemSp+100,systemSp+104,systemSp+112,systemSp+128,systemSp+132,
                            systemSp+136,systemSp+140,systemSp+144,systemSp+148}){
                        const auto saved=get(at);for(unsigned bit=0;bit<32;++bit){put(at,saved^(1u<<bit));refuseDeferred(deferred);}put(at,saved);}
                    for(unsigned command:{cmd,commandStorage+0x3F80,ring,writeBack,newCommand+1,UINT32_MAX}){
                        auto bad=deferred;bad.arguments[0]=command;refuseDeferred(bad);}
                    for(unsigned image:{frontbuffer,ring,nextCopy+4096,writeBack,0u,0xFFFFF000u}){
                        auto bad=prepareNextSwap(image);bad.arguments[0]=newCommand;bad.returnValue=deferred.returnValue;refuseDeferred(bad);}
                    (void)prepareNextSwap(nextCopy);
                    backend.active=false;refuseDeferred(deferred);backend.active=true;
                    assert(registry.bindGuestThreadToken(other->layout.threadAddress));refuseDeferred(deferred);
                    assert(registry.bindGuestThreadToken(issuerToken));
                    for(unsigned offset:{0u,4u,NativeSwapImagePlan::Capacity-4}){
                        const auto alias=fork();assert(alias>=0);
                        if(!alias){GuestImportCall cs;cs.arguments={nextCopy+offset,0};
                            assert(registry.dispatch("__imp__RtlInitializeCriticalSectionAndSpinCount",cs).handled());
                            refuseDeferred(deferred);std::_Exit(0);}
                        int status=0;assert(waitpid(alias,&status,0)==alias && WIFEXITED(status) && !WEXITSTATUS(status));}
                    assert(registry.dispatch("__imp__VdSwap",deferred).handled() && deferred.returnValue==0x1122334455667788ull);
                    const auto fresh=registry.nativeDeferredSwapRequest();
                    const auto envelope=NativeSwapCommand::make({3,phys(newCommand),phys(nextCopy),2371});
                    assert(fresh && fresh->request.commandVirtual==newCommand && fresh->imagePhysical==phys(nextCopy) &&
                        fresh->imageOwner.virtualBase==nextCopy && fresh->issuedReadWord==2371 && envelope &&
                        envelope->matches(phys(newCommand),space.translate(newCommand),256) &&
                        get(newCommand-4)==0x91919191 && get(newCommand+256)==0x91919191 &&
                        !std::memcmp(unrendered.data(),space.translate(nextCopy),unrendered.size()) &&
                        !std::memcmp(ticketSnapshot.data(),space.translate(cmd),256) &&
                        registry.nativePostOutputSwapStatus().issuedReadWord==2365 && get(writeBack)==2371 && displays==3);
                    assert(!registry.dispatch("__imp__VdSwap",deferred).handled());
                    for(unsigned allocation:{nextCopy,commandStorage,ring,writeBack}){GuestImportCall free;free.arguments={1,allocation};
                        assert(!registry.dispatch("__imp__MmFreePhysicalMemory",free).handled());}
                    GuestImportCall fill;fill.arguments={nextCopy,4,0};assert(!registry.dispatch("__imp__RtlFillMemoryUlong",fill).handled());
                    assert(!registry.submitExperimentalNativeSwapCommand(2374) &&
                        !registry.submitNativeWrappedSwapCommand(2374) &&
                        !registry.submitNativePostOutputWrappedSwapCommand(2374,completeIrq) &&
                        get(writeBack)==2371 && displays==3 && !std::memcmp(unrendered.data(),space.translate(nextCopy),unrendered.size()));
                    std::printf("DEFERRED_NATIVE_SWAP_REQUEST: %urefusals,3partial-image-lock aliases,pinned ticket3,256bytes/canaries,no pixel decode/render/presentation/RPTR/history changes PASS\n",deferredRefusals);
                    std::fflush(nullptr);
                    const auto deferredChild=commandStorage+0x3800;
                    assert(!registry.inspectStoppedDeferredSwapSubmission(2374)); // No fresh fence yet.
                    const std::array<unsigned,11> deferredCoherency{0xA31,0x01000000,0x10A2F,0x4000,phys(commandStorage),
                        0xC0043C00,3,0xA31,0,0x80000000,8};
                    for(unsigned i=0;i<11;++i)put(deferredChild+i*4,deferredCoherency[i]);
                    put(ring+2371*4,0xC0013F00);put(ring+2372*4,phys(deferredChild));put(ring+2373*4,11);
                    unsigned fenceRefusals=0;
                    const auto refuseFence=[&](unsigned word=2374){const auto before=get(writeBack);
                        const auto accepted=registry.submitNativeDeferredSwapStorageCoherency(word);
                        if(accepted || backend.deferredStorageCalls)std::fprintf(stderr,"DEFERRED_FENCE_CONTROL: case=%u root=%08X,%08X,%08X calls=%u accepted=%u\n",
                            fenceRefusals,get(ring+2371*4),get(ring+2372*4),get(ring+2373*4),backend.deferredStorageCalls,accepted);
                        assert(!accepted && !backend.deferredStorageCalls &&
                            !registry.completedNativeDeferredSwapStorageCoherency() && get(writeBack)==before && displays==3);++fenceRefusals;};
                    for(unsigned i=0;i<3;++i)for(unsigned bit=0;bit<32;++bit){const auto at=ring+(2371+i)*4,saved=get(at);
                        if(i==1 && bit==9){
                            // This fixture already contains the SAME valid
                            // request at +0x3A00. Reusing packet bytes is legal;
                            // reusing the previous fence receipt is not.
                            for(unsigned w=0;w<11;++w)assert(get(commandStorage+0x3A00+w*4)==deferredCoherency[w]);
                            const auto replay=fork();assert(replay>=0);
                            if(!replay){put(at,saved^(1u<<bit));backend.deferredStorageAccept=true;
                                assert(registry.submitNativeDeferredSwapStorageCoherency(2374) && backend.deferredStorageCalls==1 &&
                                    registry.nativeDeferredSwapRequest()->issuedReadWord==2371 && displays==3);std::_Exit(0);}
                            int status=0;assert(waitpid(replay,&status,0)==replay && WIFEXITED(status) && !WEXITSTATUS(status));continue;
                        }
                        put(at,saved^(1u<<bit));refuseFence();put(at,saved);}
                    for(unsigned i=0;i<11;++i)for(unsigned bit=0;bit<32;++bit){const auto at=deferredChild+i*4,saved=get(at);
                        put(at,saved^(1u<<bit));refuseFence();put(at,saved);}
                    for(unsigned word:{0u,1u,5u,6u,7u,8u,12u,63u})for(unsigned bit=0;bit<32;++bit){
                        const auto at=newCommand+word*4,saved=get(at);put(at,saved^(1u<<bit));refuseFence();put(at,saved);}
                    for(unsigned word:{0u,2371u,2373u,2375u,UINT32_MAX})refuseFence(word);
                    for(unsigned address:{newCommand,newCommand+252,commandStorage+0x3FFC,nextCopy,ring,writeBack}){
                        put(ring+2372*4,phys(address));refuseFence();put(ring+2372*4,phys(deferredChild));}
                    backend.active=false;refuseFence();backend.active=true;
                    assert(registry.bindGuestThreadToken(other->layout.threadAddress));refuseFence();
                    assert(registry.bindGuestThreadToken(issuerToken));
                    for(unsigned at:{writeBack,frontbuffer}){const auto saved=get(at);put(at,saved^1);refuseFence();put(at,saved);}
                    for(unsigned at:{deferredChild+40,newCommand+252,nextCopy+NativeSwapImagePlan::Capacity-4}){
                        const auto alias=fork();assert(alias>=0);
                        if(!alias){GuestImportCall cs;cs.arguments={at,0};
                            assert(registry.dispatch("__imp__RtlInitializeCriticalSectionAndSpinCount",cs).handled());
                            refuseFence();std::_Exit(0);}
                        int status=0;assert(waitpid(alias,&status,0)==alias && WIFEXITED(status) && !WEXITSTATUS(status));}
                    const auto deferredIdentifier=*registry.graphicsSystemIdentifierAddress();
                    for(unsigned failure=0;failure<9;++failure){std::fflush(nullptr);const auto child=fork();assert(child>=0);
                        if(!child){alarm(5);backend.deferredStorageAccept=failure!=0;backend.deferredStorageThrow=failure==1;
                            backend.deferredStorageAfter=[&]{
                                if(failure>=2 && failure<=7){const auto at=std::array<unsigned,6>{newCommand,ring+2371*4,deferredChild,
                                    writeBack,deferredIdentifier,frontbuffer}[failure-2];put(at,get(at)^1);}
                                if(failure==8)backend.active=false;
                            };
                            assert(!registry.submitNativeDeferredSwapStorageCoherency(2374) && failure<2 && backend.deferredStorageCalls==1 &&
                                !registry.completedNativeDeferredSwapStorageCoherency() && get(writeBack)==2371 && displays==3);
                            assert(!registry.submitNativeDeferredSwapStorageCoherency(2374) && backend.deferredStorageCalls==1);
                            std::_Exit(0);}
                        int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==(failure<2?0:4));}
                    backend.deferredStorageAccept=true;
                    assert(registry.submitNativeDeferredSwapStorageCoherency(2374) && backend.deferredStorageCalls==1 &&
                        registry.completedNativeDeferredSwapStorageCoherency()->previousWord==2371 &&
                        registry.completedNativeDeferredSwapStorageCoherency()->readWord==2374 && get(writeBack)==2374 && displays==3 &&
                        registry.nativePostOutputSwapStatus().readWord==2371 && registry.nativeDeferredSwapRequest()->issuedReadWord==2371 &&
                        envelope->matches(phys(newCommand),space.translate(newCommand),256) &&
                        !std::memcmp(unrendered.data(),space.translate(nextCopy),unrendered.size()));
                    assert(!registry.submitNativeDeferredSwapStorageCoherency(2374) && backend.deferredStorageCalls==1);
                    std::printf("DEFERRED_SWAP_STORAGE: %upreflight refusals,3partial-lock aliases,2nonretry failures,7late fail-stops,fresh fence/RPTR-last,ticket3 pending,no image execution PASS\n",fenceRefusals);
                    std::fflush(nullptr);
                    const auto mixedStart=newCommand-320;
                    put(ring+2374*4,0xC0013F00);put(ring+2375*4,phys(mixedStart));put(ring+2376*4,192);
                    const auto mixed=registry.inspectStoppedDeferredSwapSubmission(2377);
                    assert(mixed && mixed->rootPhysical==phys(ring)+2374*4 && mixed->childPhysical==phys(mixedStart) &&
                        mixed->childWords==192 && mixed->commandOffsetWords==80 && mixed->readWord==2374 && mixed->writeWord==2377);
                    unsigned mixedRefusals=0;
                    const auto refuseMixed=[&](unsigned word=2377){assert(!registry.inspectStoppedDeferredSwapSubmission(word));++mixedRefusals;};
                    for(unsigned bit=0;bit<32;++bit){const auto at=ring+2374*4,saved=get(at);put(at,saved^(1u<<bit));refuseMixed();put(at,saved);}
                    for(unsigned address:{mixedStart+1,newCommand+4,nextCopy,ring,writeBack,commandStorage-4,0xFFFFFFFCu}){
                        put(ring+2375*4,phys(address));refuseMixed();put(ring+2375*4,phys(mixedStart));}
                    for(unsigned words:{0u,63u,64u,143u,0x400000u,0xFFFFFFFFu}){
                        put(ring+2376*4,words);refuseMixed();put(ring+2376*4,192);}
                    for(unsigned word:{0u,2374u,2376u,2378u,UINT32_MAX})refuseMixed(word);
                    for(unsigned at:{writeBack,frontbuffer,newCommand,newCommand+252}){const auto saved=get(at);put(at,saved^1);refuseMixed();put(at,saved);}
                    for(unsigned at:{mixedStart-4,mixedStart+192*4-4}){const auto alias=fork();assert(alias>=0);
                        if(!alias){GuestImportCall cs;cs.arguments={at,0};assert(registry.dispatch("__imp__RtlInitializeCriticalSectionAndSpinCount",cs).handled());
                            refuseMixed();std::_Exit(0);}
                        int status=0;assert(waitpid(alias,&status,0)==alias && WIFEXITED(status) && !WEXITSTATUS(status));}
                    assert(registry.inspectStoppedDeferredSwapSubmission(2377) && get(writeBack)==2374 && displays==3 && backend.deferredStorageCalls==1 &&
                        envelope->matches(phys(newCommand),space.translate(newCommand),256) &&
                        !std::memcmp(unrendered.data(),space.translate(nextCopy),unrendered.size()));
                    std::printf("STOPPED_DEFERRED_SUBMISSION: %urefusals,2partial-lock aliases,owned root/child/ticket identity,current cursor,unrendered pixels/history unchanged,no execution PASS\n",mixedRefusals);
                    std::fflush(nullptr);
                    // Separate live PREFIX transaction. Keep the full IB at
                    // RPTR2374 until a future ticket3 tail actually completes.
                    NativeColorBatchFixture thirdColor(phys(nextSource),phys(nextCopy),phys(event0),phys(event1),true,true);
                    auto prefix=thirdColor.words;prefix.resize(prefix.size()-14);prefix.insert(prefix.end(),{0x2007,0,0x2318,0,0x231B,0});
                    const auto prefixStart=newCommand-40-static_cast<unsigned>(prefix.size()*4);
                    assert(prefixStart>=commandStorage);
                    for(unsigned i=0;i<prefix.size();++i)put(prefixStart+i*4,prefix[i]);
                    const auto thirdTail=postOutputSwapBytes(*envelope,phys(writeBack)+0x40,0x4000,
                        controlPhysical+0x108,controlPhysical+0x100,controlPhysical+0x104);
                    std::memcpy(space.translate(newCommand-40),thirdTail.data(),thirdTail.size());
                    const auto wholeWords=static_cast<unsigned>(prefix.size()+129);
                    put(ring+2375*4,phys(prefixStart));put(ring+2376*4,wholeWords);
                    const auto runPrefix=[&](unsigned word=2377){return registry.executeExperimentalDeferredColorPrefix(*thirdColor.nativeProgram,{},word);};
                    assert(!registry.inspectStoppedDeferredSwapInterrupt(2377)); // No completed prefix yet.
                    const auto prefixIntact=[&]{assert(get(writeBack)==2374 && registry.graphicsRingConfiguration()->readWord==2374 && displays==3 &&
                        !registry.completedNativeDeferredColorPrefix() && registry.nativePostOutputSwapStatus().readWord==2371 &&
                        !std::memcmp(unrendered.data(),space.translate(nextCopy),unrendered.size()));};
                    unsigned prefixRefusals=0;
                    const auto refusePrefix=[&](unsigned word=2377){assert(!runPrefix(word) && !backend.deferredColorCalls);
                        ++prefixRefusals;prefixIntact();};
                    for(unsigned word:{0u,2374u,2376u,2378u,UINT32_MAX})refusePrefix(word);
                    for(unsigned at:{writeBack,ring+2374*4,prefixStart,newCommand,newCommand+252,frontbuffer}){
                        const auto saved=get(at);put(at,saved^1);assert(!runPrefix() && !backend.deferredColorCalls);++prefixRefusals;put(at,saved);prefixIntact();}
                    backend.active=false;refusePrefix();backend.active=true;
                    const auto scratchBase=backend.nativeScratchBase;backend.nativeScratchBase=0;refusePrefix();backend.nativeScratchBase=scratchBase;
                    assert(registry.bindGuestThreadToken(other->layout.threadAddress));refusePrefix();assert(registry.bindGuestThreadToken(issuerToken));
                    for(unsigned at:{nextSource, prefixStart-4, nextCopy+NativeSwapImagePlan::Capacity-4,writeBack+0x40,event0}){
                        std::fflush(nullptr);const auto nested=fork();assert(nested>=0);
                        if(!nested){alarm(5);GuestImportCall cs;cs.arguments={at,0};
                            assert(registry.dispatch("__imp__RtlInitializeCriticalSectionAndSpinCount",cs).handled());
                            assert(!runPrefix() && !backend.deferredColorCalls);std::_Exit(0);}
                        int status=0;assert(waitpid(nested,&status,0)==nested && WIFEXITED(status) && !WEXITSTATUS(status));}
                    // False/throw/invalid receipts and stale input/output/IRQ
                    // after backend entry cannot be retried. Post-copy failure
                    // and duplicate publisher calls must fail-stop4.
                    for(unsigned failure=0;failure<14;++failure){std::fflush(nullptr);const auto nested=fork();assert(nested>=0);
                        if(!nested){alarm(5);backend.deferredColorFault=failure;backend.deferredColorAccept=failure!=0;
                            if(failure>=10)backend.deferredColorBefore=[&]{const std::array<unsigned,4> targets{nextSource,nextCopy,writeBack+0x40,frontbuffer};
                                const auto at=targets[failure-10];put(at,get(at)^1);};
                            assert(!runPrefix() && backend.deferredColorCalls==1 && !registry.completedNativeDeferredColorPrefix());
                            assert(!runPrefix() && backend.deferredColorCalls==1 && get(writeBack)==2374 && displays==3);
                            if(failure<7)prefixIntact();std::_Exit(0);}
                        int status=0;const auto waited=waitpid(nested,&status,0);const auto expected=failure>=7 && failure<=9?4:0;
                        if(waited!=nested || !WIFEXITED(status) || WEXITSTATUS(status)!=expected)
                            std::fprintf(stderr,"DEFERRED_PREFIX_FAULT: case=%u raw=%d reason=%u\n",failure,status,registry.nativeDeferredColorPrefixStatus()[1]);
                        assert(waited==nested && WIFEXITED(status) && WEXITSTATUS(status)==expected);}
                    backend.deferredColorAccept=true;
                    std::promise<void> trying,freed;auto attempted=trying.get_future(),finished=freed.get_future();std::thread freer;
                    backend.deferredColorBefore=[&]{freer=std::thread([&]{trying.set_value();GuestImportCall c;c.arguments={1,nextSource};
                            assert(!registry.dispatch("__imp__MmFreePhysicalMemory",c).handled());freed.set_value();});
                        attempted.wait();assert(finished.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout);};
                    const bool prefixDone=runPrefix();
                    if(!prefixDone)std::fprintf(stderr,"DEFERRED_PREFIX_ADMISSION: reason=%u detail=%08X calls=%u\n",
                        registry.nativeDeferredColorPrefixStatus()[1],registry.nativeDeferredColorPrefixStatus()[2],backend.deferredColorCalls);
                    if(freer.joinable())freer.join();assert(prefixDone && backend.deferredColorCalls==1);
                    const auto completion=registry.completedNativeDeferredColorPrefix();assert(completion && completion->previousWord==2374 &&
                        completion->pendingWriteWord==2377 && completion->prefixWords==prefix.size() && completion->nativeFences==4 &&
                        completion->leadingSetupFences==1 && completion->copyBytes==1280*720*4 && get(writeBack)==2374 &&
                        registry.graphicsRingConfiguration()->readWord==2374 && displays==3 && registry.nativePostOutputSwapStatus().readWord==2371);
                    assert(!runPrefix() && backend.deferredColorCalls==1);
                    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x){unsigned at=0;assert(colorCopyOffset(1280,720,x,y,at));
                        for(unsigned b=0;b<4;++b)assert(space.translate(nextCopy)[at+b]==0);}
                    for(unsigned owner:{nextSource,nextCopy,commandStorage,frontbuffer}){GuestImportCall c;c.arguments={1,owner};
                        assert(!registry.dispatch("__imp__MmFreePhysicalMemory",c).handled());
                        c.arguments={owner,4,0};assert(!registry.dispatch("__imp__RtlFillMemoryUlong",c).handled());}
                    assert(!std::memcmp(thirdTail.data(),space.translate(newCommand-40),thirdTail.size()));
                    std::printf("DEFERRED_COLOR_PREFIX_REGISTRY: %upreflight refusals,5lock aliases,11nonretry failures,3post-copy fail-stops,lifecycle lock/pins,copy retained,RPTR/tail/history unchanged; CPU synthetic only PASS\n",prefixRefusals);
                    std::fflush(nullptr);
                    unsigned irqReason=99;
                    const auto thirdIrq=registry.inspectStoppedDeferredSwapInterrupt(2377,&irqReason);
                    assert(thirdIrq && !irqReason && thirdIrq->wrapper.command().identity().ticket==3 &&
                        thirdIrq->wrapper.callbackContext()==(phys(nextCopy)|0x208) && thirdIrq->submission.readWord==2374 &&
                        thirdIrq->submission.commandOffsetWords==prefix.size()+10 && thirdIrq->queuedSurfaces[0]==phys(frontbuffer) &&
                        thirdIrq->queuedSurfaces[1]==2 && thirdIrq->deviceValues[8]==1 && thirdIrq->pacingRecord[1]==2);
                    static_assert(!std::is_convertible_v<GuestImportRegistry::StoppedDeferredSwapInterrupt,
                        GuestImportRegistry::StoppedPostOutputSwapInterrupt>);
                    unsigned snapshotRefusals=0;
                    for(unsigned word:{0u,2374u,2376u,2378u,UINT32_MAX}){
                        assert(!registry.inspectStoppedDeferredSwapInterrupt(word,&irqReason) && irqReason);++snapshotRefusals;}
                    for(unsigned at:{prefixStart,newCommand,nextCopy,frontbuffer,device+11028,writeBack}){
                        const auto saved=get(at);put(at,saved^1);
                        assert(!registry.inspectStoppedDeferredSwapInterrupt(2377,&irqReason) && irqReason);++snapshotRefusals;put(at,saved);}
                    *mappingLive=false;
                    assert(!registry.inspectStoppedDeferredSwapInterrupt(2377,&irqReason) && irqReason==4 && thirdIrq->queuedSurfaces[1]==2);
                    *mappingLive=true;++snapshotRefusals;
                    // Raw observations, not empty-queue admission or a replay
                    // of second-swap history. Preserve current nonempty words.
                    for(unsigned i:{0u,1u,30u,31u}){const auto at=device+16980+i*4,saved=get(at);put(at,0xCAFE0000+i);
                        const auto now=registry.inspectStoppedDeferredSwapInterrupt(2377);assert(now && now->queuedSurfaces[i]==0xCAFE0000+i);
                        put(at,saved);}
                    for(unsigned i=0;i<6;++i){const auto at=0x83BFBE40+i*4,saved=get(at);put(at,0xCAFE0000+i);
                        const auto now=registry.inspectStoppedDeferredSwapInterrupt(2377);assert(now && now->pacingRecord[i]==0xCAFE0000+i);
                        put(at,saved);}
                    for(unsigned at:{device+16980,writeBack+0x40,0x83BFBE3Cu,prefixStart-4}){std::fflush(nullptr);const auto nested=fork();assert(nested>=0);
                        if(!nested){alarm(5);GuestImportCall cs;cs.arguments={at,0};
                            assert(registry.dispatch("__imp__RtlInitializeCriticalSectionAndSpinCount",cs).handled());
                            assert(!registry.inspectStoppedDeferredSwapInterrupt(2377,&irqReason) && irqReason);std::_Exit(0);}
                        int status=0;assert(waitpid(nested,&status,0)==nested && WIFEXITED(status) && !WEXITSTATUS(status));}
                    const auto finalIrq=registry.inspectStoppedDeferredSwapInterrupt(2377);
                    assert(finalIrq && finalIrq->deviceValues==thirdIrq->deviceValues && finalIrq->queuedSurfaces==thirdIrq->queuedSurfaces &&
                        finalIrq->scratch==thirdIrq->scratch && finalIrq->pacingRecord==thirdIrq->pacingRecord &&
                        get(writeBack)==2374 && displays==3 && backend.deferredColorCalls==1);
                    std::printf("STOPPED_DEFERRED_IRQ: %urefusals,4lock aliases,10raw mutations observed,typed ticket3/nonempty queue/pacing,pre-teardown snapshot,no dispatch or writes PASS\n",snapshotRefusals);
                    std::fflush(nullptr);
                    const auto priorScope=registry.nativePostOutputSwapInterruptScopeStatus();
                    const auto thirdThreadCount=registry.createdThreadCount();unsigned thirdEnqueues=0,thirdRefusals=0;
                    const auto noThirdEnqueue=[&](const auto&,const auto&)->bool{++thirdEnqueues;assert(false);return false;};
                    assert(!registry.nativeDeferredSwapInterruptWorker() && !registry.createNativeDeferredSwapInterruptWorker(2377,{}));
                    const auto refuseThirdWorker=[&](unsigned word=2377){
                        assert(!registry.createNativeDeferredSwapInterruptWorker(word,noThirdEnqueue) && !thirdEnqueues &&
                            registry.createdThreadCount()==thirdThreadCount && !registry.nativeDeferredSwapInterruptWorker());++thirdRefusals;};
                    for(unsigned word:{0u,2374u,2376u,2378u,UINT32_MAX})refuseThirdWorker(word);
                    std::vector<unsigned> thirdFields;
                    for(unsigned offset:thirdIrq->DeviceOffsets)thirdFields.push_back(device+offset);
                    for(unsigned i=0;i<32;++i)thirdFields.push_back(device+16980+i*4);
                    for(unsigned i=0;i<8;++i)thirdFields.push_back(writeBack+0x40+i*4);
                    for(unsigned i=0;i<6;++i)thirdFields.push_back(secondRecord+i*4);
                    for(unsigned at:thirdFields){const auto saved=get(at);put(at,saved^1);refuseThirdWorker();put(at,saved);}
                    for(unsigned token:{worker->layout.threadAddress,freshWorker->layout.threadAddress,other->layout.threadAddress}){
                        assert(registry.bindGuestThreadToken(token));refuseThirdWorker();}
                    assert(registry.bindGuestThreadToken(issuerToken));
                    *mappingLive=false;refuseThirdWorker();*mappingLive=true;
                    *pacingMapping=false;refuseThirdWorker();*pacingMapping=true;
                    const auto savedMask=backend.nativeScratchMask;backend.nativeScratchMask^=1;refuseThirdWorker();backend.nativeScratchMask=savedMask;
                    for(bool throws:{false,true}){GuestPreparedThread prepared;bool caught=false;
                        try{assert(!registry.createNativeDeferredSwapInterruptWorker(2377,[&](const auto& candidate,const auto& entry)->bool{
                            ++thirdEnqueues;prepared=candidate;assert(entry.argument0==1 && entry.argument1==device && entry.directEntry &&
                                !candidate.wrappedContextAddress && *space.translate(entry.pcrAddress+0x10C)==2);
                            if(throws)throw std::runtime_error("third IRQ enqueue");return false;}));}catch(const std::runtime_error&){caught=true;}
                        assert(caught==throws && registry.createdThreadCount()==thirdThreadCount && !registry.nativeDeferredSwapInterruptWorker());
                        assert(std::all_of(space.translate(prepared.allocation.base),space.translate(prepared.allocation.base)+prepared.allocation.size,[](auto v){return !v;}));}
                    const auto thirdWorker=registry.createNativeDeferredSwapInterruptWorker(2377,[&](const auto& candidate,const auto& entry){
                        ++thirdEnqueues;return entry.argument0==1 && entry.argument1==device &&
                            candidate.layout.threadAddress!=freshWorker->layout.threadAddress && candidate.layout.pcrAddress!=freshWorker->layout.pcrAddress;});
                    assert(thirdWorker && thirdEnqueues==3 && registry.createdThreadCount()==thirdThreadCount+1 &&
                        registry.nativeDeferredSwapInterruptWorker()==thirdWorker->layout.threadAddress);
                    assert(!registry.createNativeDeferredSwapInterruptWorker(2377,noThirdEnqueue));
                    assert(!registry.runNativeDeferredSwapInterruptScope(2377,[]{assert(false);}));
                    assert(registry.bindGuestThreadToken(thirdWorker->layout.threadAddress));
                    unsigned thirdScopeRefusals=0,thirdActions=0;
                    const NativeDeferredSwapInterruptSequence::Action thirdAction=[&](const auto&){++thirdActions;return true;};
                    const auto refuseThirdScope=[&](unsigned word=2377){
                        assert(!registry.runNativeDeferredSwapInterruptScope(word,[]{assert(false);}) &&
                            !registry.nativeDeferredSwapInterruptScopeStatus().scope.attempted);++thirdScopeRefusals;};
                    for(unsigned word:{0u,2374u,2376u,2378u,UINT32_MAX})refuseThirdScope(word);
                    for(unsigned at:thirdFields){const auto saved=get(at);put(at,saved^1);refuseThirdScope();put(at,saved);}
                    for(unsigned at:{thirdWorker->layout.pcrAddress+0x10C,thirdWorker->layout.threadAddress+0xBF}){
                        *space.translate(at)=3;refuseThirdScope();*space.translate(at)=2;}
                    const auto simulateThird=[&](unsigned failure=UINT32_MAX){
                        assert(!registry.inspectStoppedDeferredSwapInterrupt(2377)); // Never expose an active scope as stopped.
                        assert(!registry.runNativePostOutputSwapInterruptScope(2371,[]{assert(false);}));
                        assert(!registry.runNativeDeferredSwapInterruptScope(2377,[]{assert(false);}));
                        GuestImportCall forbidden;assert(!registry.dispatch("__imp__KeSetEvent",forbidden).handled());
                        GuestImportCall freq;assert(registry.dispatch("__imp__KeQueryPerformanceFrequency",freq).handled() && freq.returnValue==50000000);
                        if(failure==0)throw std::runtime_error("third mock guest");
                        assert(registry.beginNativeDeferredSwapInterrupt(2377,thirdAction));
                        assert(!registry.beginNativeDeferredSwapInterrupt(2377,thirdAction));
                        assert(!registry.acknowledgeNativeDeferredSwapInterrupt(2377,thirdAction));
                        assert(get(writeBack+0x40)==4 && get(writeBack+0x44)==thirdIrq->scratch[1] &&
                            get(writeBack+0x54)==(phys(nextCopy)|0x208));
                        if(failure==1)return;
                        if(failure==2){invokeLock(0,device+17120,true);return;}
                        balancedLocks();
                        put(device+16960,3);put(device+16972,3);put(device+17112,2);
                        put(device+16988,phys(nextCopy));put(device+16992,3);
                        const std::array<unsigned,6> record{1,3,0,2,75,3};
                        for(unsigned i=0;i<6;++i)put(secondRecord+i*4,record[i]);
                        put(writeBack+0x40,0); // Test stand-in only; production must execute original CPU2 handler.
                        const std::array<unsigned,12> targets{device+16980,device+16984,device+16988,device+16992,
                            device+17108,device+17112,device+16960,device+16972,device+17004,writeBack+0x40,writeBack+0x44,writeBack+0x54};
                        if(failure>=3 && failure<15){const auto at=targets[failure-3];put(at,get(at)^1);}
                        if(failure==15)put(secondRecord+16,101);
                        if(failure==16)put(prefixStart,get(prefixStart)^1);
                        if(failure==17)*mappingLive=false;
                        if(failure==18)assert(registry.bindGuestThreadToken(other->layout.threadAddress));
                    };
                    for(unsigned failure=0;failure<19;++failure){std::fflush(nullptr);const auto test=fork();assert(test>=0);
                        if(!test){alarm(5);bool caught=false;
                            try{assert(!registry.runNativeDeferredSwapInterruptScope(2377,[&]{simulateThird(failure);}));}
                            catch(const std::runtime_error&){caught=true;}
                            const auto status=registry.nativeDeferredSwapInterruptScopeStatus();
                            assert(caught==(failure==0) && status.scope.attempted && status.rejection && !status.queuedCallbackValidated);
                            assert(!registry.runNativeDeferredSwapInterruptScope(2377,[]{assert(false);}) &&
                                !registry.acknowledgeNativeDeferredSwapInterrupt(2377,thirdAction) && get(writeBack)==2374 && displays==3);
                            std::_Exit(0);}
                        int status=0;assert(waitpid(test,&status,0)==test && WIFEXITED(status) && !WEXITSTATUS(status));}
                    for(unsigned at:{device+16980,device+16992,writeBack+0x40,writeBack+0x44,secondRecord+16,prefixStart}){
                        std::fflush(nullptr);const auto test=fork();assert(test>=0);if(!test){alarm(5);
                            assert(registry.runNativeDeferredSwapInterruptScope(2377,[&]{simulateThird();}));
                            put(at,get(at)^1);assert(!registry.acknowledgeNativeDeferredSwapInterrupt(2377,thirdAction));
                            assert(!registry.acknowledgeNativeDeferredSwapInterrupt(2377,thirdAction) && get(writeBack)==2374 && displays==3);std::_Exit(0);}
                        int status=0;assert(waitpid(test,&status,0)==test && WIFEXITED(status) && !WEXITSTATUS(status));}
                    assert(registry.runNativeDeferredSwapInterruptScope(2377,[&]{simulateThird();}));
                    assert(registry.acknowledgeNativeDeferredSwapInterrupt(2377,thirdAction) && thirdActions==11 &&
                        !registry.acknowledgeNativeDeferredSwapInterrupt(2377,thirdAction));
                    const auto thirdStatus=registry.nativeDeferredSwapInterruptScopeStatus();
                    const auto thirdAfter=registry.inspectStoppedDeferredSwapInterrupt(2377);
                    assert(thirdAfter && thirdStatus.scope.balanced && thirdStatus.queuedCallbackValidated &&
                        thirdStatus.scope.worker==thirdWorker->layout.threadAddress &&
                        thirdStatus.scope.lockPairs==(std::array<unsigned,2>{1,1}) &&
                        thirdAfter->queuedSurfaces[0]==thirdIrq->queuedSurfaces[0] && thirdAfter->queuedSurfaces[1]==2 &&
                        thirdAfter->queuedSurfaces[2]==phys(nextCopy) && thirdAfter->queuedSurfaces[3]==3 &&
                        thirdAfter->deviceValues[8]==2 && !thirdAfter->deviceValues[12] && thirdAfter->scratch[4]==0x0BADF00D &&
                        thirdAfter->scratch[1]==thirdIrq->scratch[1] && get(writeBack)==2374 && displays==3 && backend.deferredColorCalls==1 &&
                        registry.nativePostOutputSwapInterruptScopeStatus().scope.worker==priorScope.scope.worker);
                    assert(registry.markGuestThreadReturned(thirdWorker->layout.threadAddress,0));
                    assert(registry.bindGuestThreadToken(issuerToken));
                    std::printf("DEFERRED_SWAP_FRESH_SCOPE: %uworker/%uscope preflight refusals,2enqueue rollbacks,19terminal/6late-ACK faults,11actions,old queue preserved,new target3,CPU2; mock guest/no GPU/presentation/RPTR PASS\n",thirdRefusals,thirdScopeRefusals);
                    std::fflush(nullptr);
                }
                assert(!registry.submitNativePostOutputWrappedSwapCommand(2371,completeIrq) && backend.secondSwapCalls==1);
                std::_Exit(0);}
            int result=0;const auto waited=waitpid(nested,&result,0);
            const bool ordinary=fault==0 || fault==1 || fault==2 || fault==3 || fault==4 || fault==5 || fault==11 || fault==14;
            if(waited!=nested || !WIFEXITED(result) || WEXITSTATUS(result)!=(ordinary?0:4))
                std::fprintf(stderr,"POST_SWAP_TRANSACTION_FAULT: case=%u raw=%d\n",fault,result);
            assert(waited==nested && WIFEXITED(result) && WEXITSTATUS(result)==(ordinary?0:4));}
        assert(registry.bindGuestThreadToken(freshWorker->layout.threadAddress));
        std::printf("POST_OUTPUT_SWAP_TRANSACTION: %upreflight refusals,6preaccept failures,17postaccept fail-stops,2mock completions,unlocked exclusive IRQ seam,events/RPTR-last,old history preserved; no real GPU/game PASS\n",transactionRefusals);
        for(unsigned fault=0;fault<15;++fault){std::fflush(nullptr);const auto nested=fork();assert(nested>=0);
            if(!nested){alarm(5);bool caught=false;
                try{assert(!registry.runNativePostOutputSwapInterruptScope(2371,[&]{simulateQueuedCallback(fault);}));}
                catch(const std::runtime_error&){caught=true;}
                const auto failed=registry.nativePostOutputSwapInterruptScopeStatus();
                assert(caught==(fault==1) && failed.scope.attempted && failed.rejection && !failed.queuedCallbackValidated);
                assert(!registry.runNativePostOutputSwapInterruptScope(2371,[]{assert(false);}));
                assert(get(writeBack)==2368 && displays==2 && registry.nativePostOutputSwapStatus().pending);
                if(fault==2)assert(get(device+17120)==1 && get(writeBack+0x40)==4); // No host cleanup.
                if(fault==4)assert(get(writeBack+0x40)==4); // No manufactured acknowledgement.
                assert(registry.nativeCommandInterruptScopeStatus().worker==oldScope.worker && registry.nativeCommandInterruptScopeStatus().balanced);
                std::_Exit(0);}
            int result=0;assert(waitpid(nested,&result,0)==nested && WIFEXITED(result) && !WEXITSTATUS(result));}
        for(unsigned fault=0;fault<16;++fault){std::fflush(nullptr);const auto nested=fork();assert(nested>=0);
            if(!nested){alarm(5);unsigned calls=0;bool caught=false;
                try{assert(!registry.runNativePostOutputSwapInterruptScope(2371,[&]{
                    assert(!registry.beginNativePostOutputSwapInterrupt(2371,[&](const auto&){
                        if(calls++==fault/2){if(fault%2)throw std::runtime_error("prefix action");return false;}return true;
                    }));
                }));}catch(const std::runtime_error&){caught=true;}
                assert(caught==bool(fault%2) && calls==fault/2+1 &&
                    registry.nativePostOutputSwapInterruptPhase()==NativePostOutputSwapInterruptSequence::Phase::Failed);
                assert(!registry.beginNativePostOutputSwapInterrupt(2371,nativeAction) && get(writeBack)==2368 && displays==2);
                std::_Exit(0);}
            int result=0;assert(waitpid(nested,&result,0)==nested && WIFEXITED(result) && !WEXITSTATUS(result));}
        for(unsigned fault=0;fault<4;++fault){std::fflush(nullptr);const auto nested=fork();assert(nested>=0);
            if(!nested){alarm(5);
                if(fault==0)backend.nativeScratchBase=0;
                if(fault==1)backend.nativeScratchBase^=4;
                if(fault==2)backend.nativeScratchMask^=1;
                if(fault==3)backend.nativeScratchMask^=0x20000;
                assert(!registry.runNativePostOutputSwapInterruptScope(2371,[&]{
                    assert(!registry.beginNativePostOutputSwapInterrupt(2371,nativeAction) && !nativeActions);
                    balancedLocks();
                }) && !get(writeBack+0x40) && get(writeBack+0x50)==0x0BADF00D && get(writeBack+0x44)==0x11223344);
                std::_Exit(0);}
            int result=0;assert(waitpid(nested,&result,0)==nested && WIFEXITED(result) && !WEXITSTATUS(result));}
        assert(registry.runNativePostOutputSwapInterruptScope(2371,[&]{simulateQueuedCallback();}));
        const auto newScope=registry.nativePostOutputSwapInterruptScopeStatus();
        assert(newScope.scope.attempted && newScope.scope.returned && newScope.scope.balanced && !newScope.rejection &&
            newScope.queuedCallbackValidated && newScope.scope.worker==freshWorker->layout.threadAddress &&
            newScope.scope.lockPairs==oldScope.lockPairs && registry.nativeCommandInterruptScopeStatus().worker==oldScope.worker &&
            get(writeBack+0x40)==0 && get(writeBack+0x44)==0x11223344 && get(writeBack+0x50)==0x4000 &&
            get(device+16980)==phys(frontbuffer) && get(device+16984)==2 && get(writeBack)==2368 && displays==2);
        assert(!registry.runNativePostOutputSwapInterruptScope(2371,[]{assert(false);}));
        std::printf("POST_OUTPUT_SWAP_FRESH_SCOPE: %upreflight refusals,2partial-leaf-lock aliases,15terminal controls,distinctCPU2/locks,queued-page/record/scratch audits,old history; mock guest/no GPU/no retirement PASS\n",newScopeRefusals);
        assert(nativeActions==8 && registry.nativePostOutputSwapInterruptPhase()==NativePostOutputSwapInterruptSequence::Phase::GuestReturned);
        {std::fflush(nullptr);const auto nested=fork();assert(nested>=0);if(!nested){alarm(5);
            assert(registry.markGuestThreadReturned(freshWorker->layout.threadAddress,0));
            assert(!registry.acknowledgeNativePostOutputSwapInterrupt(2371,nativeAction) && nativeActions==8);std::_Exit(0);}
            int result=0;assert(waitpid(nested,&result,0)==nested && WIFEXITED(result) && !WEXITSTATUS(result));}
        for(unsigned fault=0;fault<4;++fault){std::fflush(nullptr);const auto nested=fork();assert(nested>=0);
            if(!nested){alarm(5);
                if(fault==0)backend.nativeScratchBase=0;
                if(fault==1)backend.nativeScratchBase^=4;
                if(fault==2)backend.nativeScratchMask^=1;
                if(fault==3)backend.nativeScratchMask^=0x20000;
                assert(!registry.acknowledgeNativePostOutputSwapInterrupt(2371,nativeAction) && nativeActions==8 &&
                    registry.nativePostOutputSwapInterruptPhase()==NativePostOutputSwapInterruptSequence::Phase::Failed);
                std::_Exit(0);}
            int result=0;assert(waitpid(nested,&result,0)==nested && WIFEXITED(result) && !WEXITSTATUS(result));}
        for(unsigned at:{writeBack+0x40,writeBack+0x44,writeBack+0x50,writeBack+0x54,device+16980,device+16984,
                device+16972,device+17112,device+23788,secondRecord,secondRecord+16,secondWrapper+124*4,writeBack}){
            std::fflush(nullptr);const auto nested=fork();assert(nested>=0);if(!nested){alarm(5);
                const auto saved=get(at);put(at,saved^1);
                // Even a different VALID timing percentage must not replace
                // the exact record captured when the guest returned.
                assert(!registry.acknowledgeNativePostOutputSwapInterrupt(2371,nativeAction));put(at,saved);
                assert(!registry.acknowledgeNativePostOutputSwapInterrupt(2371,nativeAction) && nativeActions==8 &&
                    registry.nativePostOutputSwapInterruptPhase()==NativePostOutputSwapInterruptSequence::Phase::Failed);
                std::_Exit(0);}
            int result=0;assert(waitpid(nested,&result,0)==nested && WIFEXITED(result) && !WEXITSTATUS(result));}
        for(unsigned fault=0;fault<6;++fault){std::fflush(nullptr);const auto nested=fork();assert(nested>=0);
            if(!nested){alarm(5);unsigned calls=0;bool caught=false;
                try{assert(!registry.acknowledgeNativePostOutputSwapInterrupt(2371,[&](const auto&){
                    if(calls++==fault/2){if(fault%2)throw std::runtime_error("ack action");return false;}return true;
                }));}catch(const std::runtime_error&){caught=true;}
                assert(caught==bool(fault%2) && calls==fault/2+1 && get(writeBack+0x44)==0x11223344 && get(writeBack)==2368 && displays==2 &&
                    registry.nativePostOutputSwapInterruptPhase()==NativePostOutputSwapInterruptSequence::Phase::Failed);
                assert(!registry.acknowledgeNativePostOutputSwapInterrupt(2371,nativeAction));std::_Exit(0);}
            int result=0;assert(waitpid(nested,&result,0)==nested && WIFEXITED(result) && !WEXITSTATUS(result));}
        assert(registry.bindGuestThreadToken(issuerToken));assert(!registry.acknowledgeNativePostOutputSwapInterrupt(2371,nativeAction));
        assert(registry.bindGuestThreadToken(freshWorker->layout.threadAddress));
        assert(registry.acknowledgeNativePostOutputSwapInterrupt(2371,nativeAction) && nativeActions==11 &&
            registry.nativePostOutputSwapInterruptPhase()==NativePostOutputSwapInterruptSequence::Phase::Acknowledged &&
            get(writeBack+0x50)==0x0BADF00D && get(writeBack+0x40)==0 && get(writeBack+0x44)==0x11223344 &&
            get(writeBack)==2368 && displays==2 && registry.nativePostOutputSwapStatus().pending);
        assert(!registry.acknowledgeNativePostOutputSwapInterrupt(2371,nativeAction));
        std::puts("POST_OUTPUT_SWAP_PREFIX_ACK:11ordered registry actions,16prefix/6ack terminal faults,13late mutations,true scratch0/preserved scratch1,one-shot poison; mock GPU/guest/no retirement PASS");
        std::puts("POST_OUTPUT_SWAP_SCRATCH_PROFILE:4prefix/4ack retained base-mask failures,no invented zero or poison PASS");
        std::puts("NATIVE_SCALER_TAIL_REGISTRY:12preflight controls,2one-shot failures,8late fail-stops,ordered events/RPTR-last PASS; CPU fixture only");
        std::fflush(nullptr);std::_Exit(0);
    }int tailStatus=0;const auto waitedTail=waitpid(tailChild,&tailStatus,0);
    if(waitedTail!=tailChild || !WIFEXITED(tailStatus) || WEXITSTATUS(tailStatus))
        std::fprintf(stderr,"SCALER_TAIL_FIXTURE_EXIT: waited=%d signal=%d raw_status=%d\n",
            waitedTail==tailChild,WIFSIGNALED(tailStatus)?WTERMSIG(tailStatus):0,tailStatus);
    assert(waitedTail==tailChild && WIFEXITED(tailStatus) && WEXITSTATUS(tailStatus)==0);
    std::puts("NATIVE_SCALER_INTERRUPT_REGISTRY: ordered prefix,owned roots,6ack continuity failures,true guest zeros,no RPTR retirement PASS; CPU fixture only");
    assert(get(writeBack)==49 && backend.scalerStorageCalls==3 && backend.wrapperCalls==before &&
        !get(device+11032) && !get(device+17120) && std::memcmp(space.translate(wrapper),irqBytes.data(),irqBytes.size())==0);
    assert(registry.bindGuestThreadToken(worker->layout.threadAddress));
    std::puts("NATIVE_INTERRUPT_SPINLOCK_SCOPE: owned CPU2/BE locks,level2/void ABI,contention/import refusal,5terminal controls PASS; CPU fixture only");
    assert(packetBits==448 && failStops==7 && backend.wrapperCalls==before);
    std::puts("NATIVE_SCALER_STORAGE:448packet/2048ticket bits,unique-copy,7post-fence fail-stops,pinned owner and RPTR-last PASS; CPU fixture, scaler still pending");
}
}
int main() {
    GuestAddressSpace space; assert(space.reserve()); GuestImportRegistry registry;
    GuestImportCall noDisplay;noDisplay.returnValue=123;
    assert(!registry.dispatch("__imp__VdPersistDisplay",noDisplay).handled() && noDisplay.returnValue==123);
    auto write = [&](auto address, auto value) {
        for (unsigned b = 0; b < 4; ++b) space.translate(address)[b] = value >> (24-b*8);
    };
    auto read = [&](auto address) {
        const auto* b = space.translate(address);
        return (std::uint32_t{b[0]}<<24) | (std::uint32_t{b[1]}<<16) | (std::uint32_t{b[2]}<<8) | b[3];
    };
    auto dispatch = [&](const char* name, std::initializer_list<std::uint32_t> args, std::uint32_t stack = 0) {
        GuestImportCall c; c.stackPointer = stack; std::copy(args.begin(),args.end(),c.arguments.begin());
        assert(registry.dispatch(name,c).handled()); return static_cast<std::uint32_t>(c.returnValue);
    };
    assert(!registry.bindExperimentalNativeInitialization());
    assert(!registry.submitExperimentalNativeInitialization(25));
    assert(!registry.submitNativeRuntimeCommandStorageCoherency(97));
    constexpr std::uint32_t Lock = 0x62004600, Thread = 0x31000000, Stack = 0x20000000;
    constexpr auto Worker = "__imp__VdRetrainEDRAMWorker", Retrain = "__imp__VdRetrainEDRAM";
    constexpr auto Status = "__imp__VdIsHSIOTrainingSucceeded";
    assert(!registry.bindNativeEdramTrainingPolicy(Lock));
    assert(!registry.bindEmptyGuestCacheCoherency());
    assert(!registry.bindNativeStateBatches());
    assert(!registry.bindExperimentalOwnedColorBatch());
    assert(!registry.bindNativeNoSystemOverlay());
    assert(!registry.bindExperimentalIdleCachePair());
    assert(!registry.bindExperimentalNativeScalerTiming(0x4800,0x7000));
    assert(!registry.bindExperimentalNativeIdentityScaler());
    assert(registry.initialize(space,0x10000000,0x10000));
    assert(registry.bindVirtualVideoMode720p60());
    assert(!registry.bindExperimentalNativeScalerTiming(0x4800,0x7000));
    auto backend = std::make_shared<Backend>();
    assert(registry.bindGraphicsBackend(backend,0x4000,0x1000,[](auto a){return a == 0x4000;}));
    // Space for a second independent color capture while original scanout
    // allocations remain pinned. This is fixture capacity, not runtime policy.
    assert(registry.bindPhysicalMemoryArena(0xA0000000,0x2000000,0));
    assert(!registry.bindExperimentalNativeInitialization());
    assert(registry.bindNativeInitialCoherencySubmission());
    assert(registry.bindExperimentalNativeInitialization());
    assert(!registry.bindExperimentalNativeInitialization());
    assert(!registry.bindNativeEdramTrainingPolicy(Lock));
    assert(registry.bindEmptyGuestCacheCoherency());
    assert(!registry.bindEmptyGuestCacheCoherency());
    assert(!registry.submitEmptyGuestCacheCoherency(28));
    assert(registry.bindNativeStateBatches());
    assert(!registry.bindNativeStateBatches() && !registry.submitNativeStateBatch(28));
    assert(registry.bindExperimentalIdleCachePair() && !registry.bindExperimentalIdleCachePair());
    assert(registry.bindExperimentalOwnedColorBatch() && !registry.bindExperimentalOwnedColorBatch());
    GuestImportCall systemCall;systemCall.stackPointer=Stack;systemCall.arguments={Stack+208,Stack+116};
    systemCall.returnValue=0xABCDEF0123456789;
    const auto systemGet="__imp__VdGetSystemCommandBuffer";
    assert(!registry.dispatch(systemGet,systemCall).handled());
    assert(registry.bindNativeNoSystemOverlay() && !registry.bindNativeNoSystemOverlay());
    std::function<bool(const GuestImportRegistry::NativeOwnedSwapImage&)> swapTestCallback=[](const auto&){return false;};
    assert(!registry.bindExperimentalNativeSwapCommands({}));
    assert(registry.bindExperimentalNativeSwapCommands([&](const auto& image){return swapTestCallback(image);}));
    assert(!registry.bindExperimentalNativeSwapCommands([](const auto&){return true;}));
    // CPU-only alternate profile; default parent keeps unknown mode words0.
    std::fflush(nullptr);const auto timingChild=fork();assert(timingChild>=0);
    if(!timingChild){alarm(5);
        constexpr unsigned slot=0x4800,storage=0x7000,out=0x8000;
        write(slot,storage);write(storage,500);
        for(unsigned bit=0;bit<32;++bit){write(storage,500^(1u<<bit));
            assert(!registry.bindExperimentalNativeScalerTiming(slot,storage));}write(storage,500);
        for(unsigned address:{0u,slot+1,0x3FFCu,0x5000u,storage})assert(!registry.bindExperimentalNativeScalerTiming(address,storage));
        write(slot,slot+4);write(slot+4,500);assert(!registry.bindExperimentalNativeScalerTiming(slot,slot+4));write(slot,storage);
        assert(registry.bindExperimentalNativeScalerTiming(slot,storage));
        assert(!registry.bindExperimentalNativeScalerTiming(slot,storage));
        for(auto address:{slot,storage}){GuestImportCall fill;fill.arguments={address,4,0};fill.returnValue=0x123456789ULL;
            assert(!registry.dispatch("__imp__RtlFillMemoryUlong",fill).handled() && fill.returnValue==0x123456789ULL);
            assert(read(slot)==storage && read(storage)==500);}
        for(const char* name:{"__imp__XGetVideoMode","__imp__VdQueryVideoMode"}){
            std::memset(space.translate(out-4),0xCC,56);
            GuestImportCall call;call.arguments[0]=out;call.returnValue=0x123456789ULL;
            assert(registry.dispatch(name,call).handled() && call.returnValue==0x123456789ULL);
            assert(read(out)==1280 && read(out+4)==720 && read(out+20)==0x42700000 && read(out+28)==74 && read(out+32)==0 &&
                read(out-4)==0xCCCCCCCC && read(out+48)==0xCCCCCCCC);
            const auto queries=registry.nativeScalerTimingQueries();
            for(auto address:{slot-44,slot,storage-44,storage}){call.arguments[0]=address;
                assert(!registry.dispatch(name,call).handled() && read(slot)==storage && read(storage)==500);}
            call.arguments[0]=out;
            for(unsigned bit=0;bit<32;++bit){write(storage,500^(1u<<bit));
                assert(!registry.dispatch(name,call).handled() && registry.nativeScalerTimingQueries()==queries);}
            write(storage,500);write(slot,storage+4);assert(!registry.dispatch(name,call).handled());write(slot,storage);
            const auto sync=fork();assert(sync>=0);
            if(!sync){dispatch("__imp__RtlInitializeCriticalSectionAndSpinCount",{out,10000});
                assert(!registry.dispatch(name,call).handled());std::_Exit(0);}
            int status=0;assert(waitpid(sync,&status,0)==sync && WIFEXITED(status) && WEXITSTATUS(status)==0);
        }
        assert(registry.nativeScalerTimingQueries()==2);registry.reset();assert(!registry.nativeScalerTimingQueries());
        std::puts("NATIVE_SCALER_TIMING: native-only whole-MHz74/nominal500,96 value bits, alias/canary/void ABI/continuity PASS; CPU only");
        std::fflush(nullptr);std::_Exit(0);
    }
    int timingStatus=0;assert(waitpid(timingChild,&timingStatus,0)==timingChild && WIFEXITED(timingStatus) && WEXITSTATUS(timingStatus)==0);
    GuestImportCall scalerUnbound;
    assert(registry.dispatch("__imp__VdInitializeScalerCommandBuffer",scalerUnbound).state==GuestImportDispatchState::NotRegistered);
    assert(!registry.bindExperimentalNativeIdentityScaler());
    write(0x4800,0x7000);write(0x7000,500);
    assert(registry.bindExperimentalNativeScalerTiming(0x4800,0x7000));
    assert(registry.bindExperimentalNativeIdentityScaler() && !registry.bindExperimentalNativeIdentityScaler());
    assert(!registry.bindNativeInterruptDeviceRoot({}));
    assert(registry.bindNativeInterruptDeviceRoot([&](unsigned expected) {return read(0x4868)==0x7200 && read(0x7200)==expected;}));
    assert(!registry.bindNativeInterruptDeviceRoot([](unsigned) {return true;}));
    assert(!registry.dispatch("__imp__VdInitializeScalerCommandBuffer",scalerUnbound).handled());
    assert(!registry.dispatch(systemGet,systemCall).handled() && !registry.nativeSystemContext());
    dispatch("__imp__RtlInitializeCriticalSectionAndSpinCount",{Lock,10000});
    assert(registry.bindNativeEdramTrainingPolicy(Lock));
    assert(!registry.bindNativeEdramTrainingPolicy(Lock));
    assert(registry.bindGuestThreadToken(Thread));
    assert(registry.bindGuestThreadTlsStorage(Thread,Thread+0x1000,4));
    write(Thread+0x14C,1);
    assert(registry.bindInitialThreadObject(Thread,1));
    auto request = [&](const char* name) {
        GuestImportCall c; c.returnValue = 0xA5A5A5A5; c.stackPointer = Stack;
        if (name == Retrain) c.arguments = {1,Stack+80,4096,Stack+84,Stack+96,2048,0,0};
        return c;
    };
    auto worker = request(Worker);
    assert(!registry.dispatch(Worker,worker).handled());
    auto status = request(Status); assert(!registry.dispatch(Status,status).handled());
    dispatch("__imp__VdInitializeEngines",{0x53020000,0x4000,0,0x4100,0x4200});
    auto allocate = [&](auto size, auto flags = 4u) { return dispatch("__imp__MmAllocatePhysicalMemoryEx",{1,size,flags,0,0xFFFFFFFF,0}); };
    const auto ring = allocate(0x10000u,4u), wb = allocate(0x1000u,4u), cold = allocate(0x1000u,4u);
    const auto ib = allocate(0x1000u,4u), destination = allocate(0x1000u,4u), readOnly = allocate(0x1000u,2u);
    const auto phys = [](auto a) { return a-0xA0000000; };
    dispatch("__imp__VdInitializeRingBuffer",{phys(ring),13});
    dispatch("__imp__VdEnableRingBufferRPtrWriteBack",{phys(wb),7});
    dispatch("__imp__VdSetSystemCommandBufferGpuIdentifierAddress",{wb+8});
    std::vector<std::uint32_t> root(22);
    root[0] = 0xC0114800; root[1] = 0x3FF;
    for (unsigned i = 0; i < 8; ++i) root[i+4] = i*0x80;
    root[12] = 0x10800; root[13] = 7; root[19] = 0xC0013F00; root[20] = phys(cold); root[21] = 11;
    const std::vector<std::uint32_t> coldWords{0xA31,0x01000000,0x10A2F,0x1000,0x20000,0xC0043C00,3,0xA31,0,0x80000000,8};
    for (unsigned i = 0; i < root.size(); ++i) write(ring+i*4,root[i]);
    for (unsigned i = 0; i < coldWords.size(); ++i) write(cold+i*4,coldWords[i]);
    for (unsigned i = 0; i < 19; ++i) write(0x12050+i*4,root[i]);
    dispatch("__imp__KiApcNormalRoutineNop",{0x12050,19},0x12000);
    assert(!registry.submitExperimentalNativeInitialization(25));
    assert(registry.submitNativeInitialCoherency(22));
    const auto vs = program({alloc(2),exec(2,1,1),alloc(1),exec(3)}, {alu(0,0,0,true,true),alu(62,8,8,true,true)});
    const auto ps = program({alloc(),exec()},{alu()});
    std::vector<std::uint32_t> batch{0x2180,0x1000000E,0x2181,0,0x2104,0,0x2200,0,0x2208,4,0x2293,0,0x2280,0x80008,
        0x1DD,phys(destination),0x1DC,0x20001,0x578,0x11223344};
    unsigned stage = 0;
    for (const auto& bytes : {vs,ps}) {
        const auto count = static_cast<std::uint32_t>(bytes.size()/4);
        batch.insert(batch.end(),{0xC0002B00 | ((count+1)<<16),stage++,count});
        for (std::size_t i = 0; i < bytes.size(); i += 4)
            batch.push_back((std::uint32_t{bytes[i]}<<24) | (std::uint32_t{bytes[i+1]}<<16) | (std::uint32_t{bytes[i+2]}<<8) | bytes[i+3]);
    }
    batch.insert(batch.end(),{0xC0003600,0x10081,0xC0025800,3,phys(destination)+6,0x55667788});
    auto publish = [&](const auto& words) {
        write(ring+88,0xC0013F00); write(ring+92,phys(ib)); write(ring+96,static_cast<std::uint32_t>(words.size()));
        for (std::size_t i = 0; i < words.size(); ++i) write(ib+i*4,words[i]);
        std::fill_n(space.translate(destination),0x1000,0xA5);
    };
    auto unchanged = [&] {
        assert(!registry.completedExperimentalNativeInitialization());
        assert(registry.graphicsRingConfiguration()->readWord == 22 && read(wb) == 22);
        for (unsigned i = 0; i < 0x1000; ++i) assert(space.translate(destination)[i] == 0xA5);
    };
    publish(batch);
    for (auto bad : {0u,22u,24u,26u,0x4000u,0xFFFFFFFFu}) { assert(!registry.submitExperimentalNativeInitialization(bad)); unchanged(); }
    for (auto bad : {phys(ring),phys(wb),phys(wb)+8,phys(ib),phys(readOnly),0x80000u,0x1FFFFFFCu}) {
        auto modified = batch; modified[15] = bad;
        publish(modified); assert(!registry.submitExperimentalNativeInitialization(25)); unchanged();
        modified = batch; modified[modified.size()-2] = bad | 2;
        publish(modified); assert(!registry.submitExperimentalNativeInitialization(25)); unchanged();
    }
    for (const auto& extra : {std::vector<std::uint32_t>{0x1234,0,0x1234,1},
                             {0xD02,0x10801,0xD02,0x10800}, {0xC0022100,0x82,0xFFFFFFFF,1}}) {
        auto modified = batch; modified.insert(modified.end(),extra.begin(),extra.end());
        publish(modified); assert(!registry.submitExperimentalNativeInitialization(25)); unchanged();
    }
    assert(backend->calls == 0);
    publish(batch);
    backend->before = [&] { assert(read(wb) == 22 && read(destination) == 0xA5A5A5A5 && read(destination+4) == 0xA5A5A5A5); };
    assert(!registry.submitExperimentalNativeInitialization(25)); unchanged();
    backend->accept = true;
    for (auto* failure : {&backend->throws,&backend->corrupt,&backend->badSize,&backend->badDraws}) {
        *failure = true; assert(!registry.submitExperimentalNativeInitialization(25)); unchanged(); *failure = false;
    }
    backend->active = false; assert(!registry.submitExperimentalNativeInitialization(25)); unchanged(); backend->active = true;
    const auto callsBeforeStale=backend->calls;write(wb,21);
    assert(!registry.submitExperimentalNativeInitialization(25));
    assert(read(wb)==21 && registry.graphicsRingConfiguration()->readWord==22 && backend->calls==callsBeforeStale);
    write(wb,22);unchanged();
    assert(registry.submitExperimentalNativeInitialization(25));
    assert(read(destination) == 0x11223344 && read(destination+4) == 0x55667788 && read(wb) == 25);
    for (unsigned i = 8; i < 0x1000; ++i) assert(space.translate(destination)[i] == 0xA5);
    const auto completed = registry.completedExperimentalNativeInitialization();
    assert(completed && completed->readWord == 25 && completed->draws == 1 && completed->words == 2);
    for (std::size_t i = 0; i < batch.size(); ++i) assert(read(ib+i*4) == batch[i]);
    assert(!registry.submitExperimentalNativeInitialization(25));
    assert(!registry.submitExperimentalNativeInitialization(26));
    worker = request(Worker);
    assert(!registry.dispatch(Worker,worker).handled()); // Neither critical region nor lock owned.
    dispatch("__imp__KeEnterCriticalRegion",{});
    assert(!registry.dispatch(Worker,worker).handled());
    dispatch("__imp__RtlEnterCriticalSection",{Lock});
    assert(!registry.dispatch(Worker,worker).handled()); // Unavailable native backend policy.
    backend->trainingAvailable = true; backend->throws = true;
    assert(!registry.dispatch(Worker,worker).handled()); backend->throws = false;
    worker.arguments[0] = 1; assert(!registry.dispatch(Worker,worker).handled()); worker.arguments[0] = 0;
    assert(registry.nativeEdramTrainingState().workerChecks == 0);
    assert(registry.dispatch(Worker,worker).handled() && worker.returnValue == 0xA5A5A5A5);
    assert(!registry.dispatch(Status,status).handled() && status.returnValue == 0xA5A5A5A5);
    assert(!registry.dispatch(Worker,worker).handled()); // Pending request cannot be overwritten.
    std::memset(space.translate(Stack),0x5A,2200); write(Stack+80,0); write(Stack+84,0);
    const std::vector<std::uint8_t> original(space.translate(Stack),space.translate(Stack)+2200);
    const auto unchangedTraining = [&] {
        assert(std::equal(original.begin(),original.end(),space.translate(Stack)));
        assert(read(wb) == 25 && registry.nativeEdramTrainingState().noCommandQueries == 0);
    };
    for (unsigned i = 0; i < 6; ++i) {
        auto c = request(Retrain); ++c.arguments[i];
        assert(!registry.dispatch(Retrain,c).handled() && c.returnValue == 0xA5A5A5A5); unchangedTraining();
    }
    for (auto sp : {0u,Stack+1,0xFFFFFFF0u,Thread,0x7FC80000u}) {
        auto c = request(Retrain); c.stackPointer = sp; c.arguments[1] = sp+80;
        c.arguments[3] = sp+84; c.arguments[4] = sp+96;
        assert(!registry.dispatch(Retrain,c).handled()); unchangedTraining();
    }
    for (auto address : {Stack+80,Stack+84}) {
        write(address,4); auto c = request(Retrain); assert(!registry.dispatch(Retrain,c).handled());
        assert(read(address) == 4); write(address,0); unchangedTraining();
    }
    backend->active = false; auto c = request(Retrain);
    assert(!registry.dispatch(Retrain,c).handled()); backend->active = true; unchangedTraining();
    backend->trainingAvailable = false;
    assert(!registry.dispatch(Retrain,c).handled()); backend->trainingAvailable = true; unchangedTraining();
    write(Thread+0x14C,99); assert(!registry.dispatch(Retrain,c).handled()); write(Thread+0x14C,1);
    write(Lock+24,Thread+4); assert(!registry.dispatch(Retrain,c).handled()); write(Lock+24,Thread);
    assert(registry.dispatch(Retrain,c).handled() && c.returnValue == 0);
    assert(std::equal(original.begin(),original.end(),space.translate(Stack)));
    assert(registry.nativeEdramTrainingState().workerChecks == 1 && registry.nativeEdramTrainingState().noCommandQueries == 1);
    backend->trainingAvailable = false; assert(!registry.dispatch(Status,status).handled()); backend->trainingAvailable = true;
    backend->throws = true; assert(!registry.dispatch(Status,status).handled()); backend->throws = false;
    backend->active = false; assert(!registry.dispatch(Status,status).handled()); backend->active = true;
    assert(registry.nativeEdramTrainingState().statusQueries == 0 && status.returnValue == 0xA5A5A5A5);
    assert(registry.dispatch(Status,status).handled() && status.returnValue == 1);
    assert(!registry.dispatch(Retrain,c).handled());
    assert(registry.dispatch(Worker,worker).handled());
    dispatch("__imp__RtlLeaveCriticalSection",{Lock}); dispatch("__imp__RtlEnterCriticalSection",{Lock});
    assert(!registry.dispatch(Retrain,c).handled()); // A released lock invalidates preparation.
    assert(registry.dispatch(Worker,worker).handled());
    dispatch("__imp__KeLeaveCriticalRegion",{}); dispatch("__imp__KeEnterCriticalRegion",{});
    assert(!registry.dispatch(Retrain,c).handled()); // Final leave also invalidates preparation.
    dispatch("__imp__RtlLeaveCriticalSection",{Lock}); dispatch("__imp__KeLeaveCriticalRegion",{});
    assert(registry.dispatch(Status,status).handled() && status.returnValue == 1);
    assert(registry.nativeEdramTrainingState().statusQueries == 2);
    const std::vector<std::uint32_t> cacheRoot{0xC0013F00,phys(cold),11};
    const auto publishCache=[&](const auto& r,const auto& ch) {
        for (unsigned i=0;i<r.size();++i) write(ring+(25+i)*4,r[i]);
        for (unsigned i=0;i<ch.size();++i) write(cold+i*4,ch[i]);
    };
    const auto cacheUnchanged=[&] {
        assert(read(wb)==25 && registry.graphicsRingConfiguration()->readWord==25);
        assert(!registry.completedEmptyGuestCacheCoherency());
    };
    publishCache(cacheRoot,coldWords);
    for (auto word : {25u,26u,27u,29u,0xFFFFFFFFu}) {
        assert(!registry.submitEmptyGuestCacheCoherency(word)); cacheUnchanged();
    }
    for (auto pointer : {phys(ring),phys(wb),phys(wb)+8,0x1FFFFFFCu,0x80000u}) {
        auto r=cacheRoot; r[1]=pointer; publishCache(r,coldWords);
        assert(!registry.submitEmptyGuestCacheCoherency(28)); cacheUnchanged();
    }
    for (unsigned i=0;i<coldWords.size();++i) {
        auto bad=coldWords; bad[i]^=1; publishCache(cacheRoot,bad);
        assert(!registry.submitEmptyGuestCacheCoherency(28)); cacheUnchanged();
    }
    assert(backend->cacheCalls==0);
    // Backend runs under the lifecycle mutex: inspect bytes, never reenter registry getters.
    publishCache(cacheRoot,coldWords); backend->cacheBefore=[&] { assert(read(wb)==25); };
    assert(!registry.submitEmptyGuestCacheCoherency(28)); cacheUnchanged();
    backend->cacheAccept=true; backend->throws=true;
    assert(!registry.submitEmptyGuestCacheCoherency(28)); backend->throws=false; cacheUnchanged();
    backend->active=false; assert(!registry.submitEmptyGuestCacheCoherency(28)); backend->active=true; cacheUnchanged();
    assert(registry.submitEmptyGuestCacheCoherency(28));
    assert(read(wb)==28 && registry.graphicsRingConfiguration()->readWord==28);
    const auto coherent=registry.completedEmptyGuestCacheCoherency();
    assert(coherent && coherent->batches==1 && coherent->readWord==28 && coherent->request.byteCount==0x1000);
    assert(backend->lastCache.physicalBase==0x20000 && backend->lastCache.cacheFlags==0x01000000);
    for (unsigned i=0;i<coldWords.size();++i) assert(read(cold+i*4)==coldWords[i]);
    assert(!registry.submitEmptyGuestCacheCoherency(28));
    for (unsigned i=0;i<cacheRoot.size();++i) write(ring+(28+i)*4,cacheRoot[i]);
    backend->cacheBefore=[&] { assert(read(wb)==28); };
    assert(registry.submitEmptyGuestCacheCoherency(31));
    assert(read(wb)==31 && registry.completedEmptyGuestCacheCoherency()->batches==2 && backend->cacheCalls==4);
    const std::vector<std::uint32_t> stateWords{0xC0006000,0x12345678,0x22080,0,0,0x400080,
        0x5C8,0x20000,0xF01,0x200E,0xC0003B00,0x7FFF,0xC0025800,3,phys(destination)+2,0x01020304,
        0xC0025800,3,phys(destination)+6,0xAABBCCDD};
    const auto publishState=[&](const auto& words) {
        write(ring+31*4,0xC0013F00); write(ring+32*4,phys(ib)); write(ring+33*4,static_cast<std::uint32_t>(words.size()));
        for (unsigned i=0;i<words.size();++i) write(ib+i*4,words[i]);
    };
    write(destination-4,0x11111111); write(destination,0xDEADBEEF); write(destination+4,0xDEADBEEF); write(destination+8,0x22222222);
    const auto stateUnchanged=[&] {
        assert(read(wb)==31 && registry.graphicsRingConfiguration()->readWord==31 && !registry.completedNativeStateBatches());
        assert(read(destination)==0xDEADBEEF && read(destination+4)==0xDEADBEEF);
        assert(read(destination-4)==0x11111111 && read(destination+8)==0x22222222);
    };
    publishState(stateWords);
    for (auto pointer : {phys(ring),phys(wb),phys(wb)+8,0x80000u,0x1FFFFFFCu}) {
        write(ring+32*4,pointer); assert(!registry.submitNativeStateBatch(34)); stateUnchanged();
    }
    for (auto address : {phys(ring),phys(wb),phys(wb)+8,phys(ib),phys(readOnly),0x80000u}) {
        auto bad=stateWords; bad[14]=address|2; publishState(bad);
        assert(!registry.submitNativeStateBatch(34)); stateUnchanged();
    }
    publishState(stateWords);
    for (auto word : {31u,32u,33u,35u,0xFFFFFFFFu}) { assert(!registry.submitNativeStateBatch(word)); stateUnchanged(); }
    assert(backend->stateCalls==0);
    backend->stateBefore=[&] { assert(read(wb)==31 && read(destination)==0xDEADBEEF); };
    assert(!registry.submitNativeStateBatch(34)); stateUnchanged();
    backend->stateAccept=true;
    for (auto* flag : {&backend->throws,&backend->corrupt,&backend->badSize,&backend->badDraws}) {
        *flag=true; assert(!registry.submitNativeStateBatch(34)); *flag=false; stateUnchanged();
    }
    backend->active=false; assert(!registry.submitNativeStateBatch(34)); backend->active=true; stateUnchanged();
    const auto stateCallsBeforeStale=backend->stateCalls;write(wb,30);
    assert(!registry.submitNativeStateBatch(34));
    assert(read(wb)==30 && registry.graphicsRingConfiguration()->readWord==31 && backend->stateCalls==stateCallsBeforeStale);
    write(wb,31);stateUnchanged();
    assert(registry.submitNativeStateBatch(34));
    assert(read(wb)==34 && read(destination)==0x01020304 && read(destination+4)==0xAABBCCDD);
    assert(read(destination-4)==0x11111111 && read(destination+8)==0x22222222);
    const auto state=registry.completedNativeStateBatches();
    assert(state && state->batches==1 && state->words==2 && state->readWord==34 && state->stateWrites==8 && state->invalidations==1);
    for (unsigned i=0;i<stateWords.size();++i) assert(read(ib+i*4)==stateWords[i]);
    assert(!registry.submitNativeStateBatch(34));
    // A cache batch can follow state without replaying an old cursor.
    for (unsigned i=0;i<cacheRoot.size();++i) write(ring+(34+i)*4,cacheRoot[i]);
    backend->cacheBefore=[&] { assert(read(wb)==34); };
    assert(registry.submitEmptyGuestCacheCoherency(37));
    assert(read(wb)==37 && registry.completedEmptyGuestCacheCoherency()->batches==3);
    const auto restart=[&](auto pointer,auto exponent) {
        GuestImportCall c; c.arguments={pointer,exponent,0,0,0,0,0,0}; c.returnValue=0x123456789ABCDEF0;
        const auto accepted=registry.dispatch("__imp__VdInitializeRingBuffer",c).handled();
        assert(c.returnValue==0x123456789ABCDEF0); return accepted;
    };
    assert(!restart(phys(ring),13u)); // Identifier still registered.
    dispatch("__imp__VdSetSystemCommandBufferGpuIdentifierAddress",{0});
    for (auto pointer : {phys(ring)+4,phys(ib),0x80000u}) assert(!restart(pointer,13u));
    for (auto exponent : {12u,14u,22u,0xFFFFFFFFu}) assert(!restart(phys(ring),exponent));
    assert(backend->drainCalls==0 && !registry.retiredGraphicsRingReadWord());
    assert(!restart(phys(ring),13u)); // Backend refuses; old generation survives.
    backend->drainAccept=true; backend->throws=true;
    assert(!restart(phys(ring),13u)); backend->throws=false;
    backend->active=false; assert(!restart(phys(ring),13u)); backend->active=true;
    assert(read(wb)==37 && registry.graphicsRingConfiguration()->readWord==37 && registry.graphicsMeInitVerified());
    assert(!registry.retiredGraphicsRingReadWord());
    assert(restart(phys(ring),13u));
    assert(read(wb)==37 && registry.graphicsRingConfiguration()->readWord==0 && !registry.graphicsMeInitVerified());
    assert(registry.retiredGraphicsRingReadWord()==37 && backend->drainCalls==3);
    assert(registry.completedNativeStateBatches()->readWord==34 && registry.completedEmptyGuestCacheCoherency()->readWord==37);
    assert(!restart(phys(ring),13u));
    assert(!registry.submitNativeInitialCoherency(22) && !registry.submitExperimentalNativeInitialization(25) &&
        !registry.submitEmptyGuestCacheCoherency(40) && !registry.submitNativeStateBatch(40));
    const auto newWb=allocate(0x1000u,4u); write(newWb,0xCAFEBABE);
    GuestImportCall badWb; badWb.arguments={phys(readOnly),7,0,0,0,0,0,0};
    assert(!registry.dispatch("__imp__VdEnableRingBufferRPtrWriteBack",badWb).handled());
    assert(registry.graphicsRingConfiguration()->writeBack->virtualAddress==wb && read(wb)==37);
    dispatch("__imp__VdEnableRingBufferRPtrWriteBack",{phys(newWb),7});
    assert(read(newWb)==0xCAFEBABE && read(wb)==37 && registry.graphicsRingConfiguration()->readWord==0);
    badWb.arguments[0]=phys(newWb); assert(!registry.dispatch("__imp__VdEnableRingBufferRPtrWriteBack",badWb).handled());
    dispatch("__imp__VdSetSystemCommandBufferGpuIdentifierAddress",{newWb+8});
    for (unsigned i=0;i<root.size();++i) write(ring+i*4,root[i]);
    for (unsigned i=0;i<coldWords.size();++i) write(cold+i*4,coldWords[i]);
    assert(!registry.submitNativeInitialCoherency(22)); // New ME_INIT verification required.
    dispatch("__imp__KiApcNormalRoutineNop",{0x12050,19},0x12000);
    for (auto pointer : {phys(ring),phys(newWb),phys(newWb)+8,0x80000u,0x1FFFFFFCu}) {
        write(ring+20*4,pointer); assert(!registry.submitNativeInitialCoherency(22));
    }
    write(ring+20*4,phys(cold));
    write(cold,0xA30); assert(!registry.submitNativeInitialCoherency(22)); write(cold,0xA31);
    assert(!registry.submitNativeInitialCoherency(21) && !registry.submitNativeInitialCoherency(23) && backend->warmCalls==0);
    assert(!registry.submitNativeInitialCoherency(22));
    backend->throws=true; assert(!registry.submitNativeInitialCoherency(22)); backend->throws=false;
    assert(read(newWb)==0xCAFEBABE && registry.graphicsRingConfiguration()->readWord==0 && !registry.completedWarmRingCoherency());
    backend->warmAccept=true; assert(registry.submitNativeInitialCoherency(22));
    assert(read(newWb)==22 && registry.graphicsRingConfiguration()->readWord==22 && backend->warmCalls==3);
    assert(registry.completedWarmRingCoherency()->physicalBase==0x20000 && registry.retiredGraphicsRingReadWord()==37);
    assert(registry.completedExperimentalNativeInitialization()->readWord==25 && registry.completedNativeStateBatches()->readWord==34);
    assert(!registry.submitNativeInitialCoherency(22));
    write(ring+22*4,0xC0001000); assert(!registry.submitNativeStateBatch(25));
    for (unsigned i=0;i<stateWords.size();++i) write(ib+i*4,stateWords[i]);
    write(ring+22*4,0xC0013F00); write(ring+23*4,phys(ib)); write(ring+24*4,static_cast<std::uint32_t>(stateWords.size()));
    backend->stateBefore=[&] { assert(read(newWb)==22); };
    backend->corrupt=true; assert(!registry.submitNativeStateBatch(25)); backend->corrupt=false;
    assert(!registry.completedWarmStateBatches() && read(newWb)==22);
    assert(registry.submitNativeStateBatch(25));
    assert(read(newWb)==25 && registry.completedWarmStateBatches()->batches==1 && registry.completedWarmStateBatches()->words==2);
    assert(registry.completedWarmStateBatches()->readWord==25 && registry.completedNativeStateBatches()->readWord==34);
    assert(registry.completedNativeStateBatches()->batches==1 && registry.retiredGraphicsRingReadWord()==37);
    // Fresh cache transactions in generation1 never reuse generation0's read37.
    for (unsigned i=0;i<cacheRoot.size();++i) write(ring+(25+i)*4,cacheRoot[i]);
    for (unsigned i=0;i<coldWords.size();++i) write(cold+i*4,coldWords[i]);
    const auto warmCacheUnchanged=[&] {
        assert(read(newWb)==25 && registry.graphicsRingConfiguration()->readWord==25);
        assert(!registry.completedWarmEmptyCacheCoherency());
        assert(read(wb)==37 && registry.completedEmptyGuestCacheCoherency()->batches==3);
    };
    const auto callsBefore=backend->cacheCalls;
    for (auto pointer : {phys(ring),phys(newWb),phys(newWb)+8,0x80000u,0x1FFFFFFCu}) {
        write(ring+26*4,pointer); assert(!registry.submitEmptyGuestCacheCoherency(28)); warmCacheUnchanged();
    }
    write(ring+26*4,phys(cold));
    for (unsigned i=0;i<coldWords.size();++i) {
        write(cold+i*4,coldWords[i]^1); assert(!registry.submitEmptyGuestCacheCoherency(28));
        write(cold+i*4,coldWords[i]); warmCacheUnchanged();
    }
    for (auto cursor : {25u,26u,27u,29u,37u,40u,UINT32_MAX}) {
        assert(!registry.submitEmptyGuestCacheCoherency(cursor)); warmCacheUnchanged();
    }
    assert(backend->cacheCalls==callsBefore);
    backend->cacheBefore=[&] { assert(read(newWb)==25 && read(wb)==37); };
    backend->cacheAccept=false; assert(!registry.submitEmptyGuestCacheCoherency(28)); warmCacheUnchanged();
    backend->cacheAccept=true; backend->throws=true;
    assert(!registry.submitEmptyGuestCacheCoherency(28)); backend->throws=false; warmCacheUnchanged();
    backend->active=false; assert(!registry.submitEmptyGuestCacheCoherency(28)); backend->active=true;
    warmCacheUnchanged(); assert(registry.submitEmptyGuestCacheCoherency(28));
    assert(read(newWb)==28 && registry.completedWarmEmptyCacheCoherency()->batches==1);
    assert(!registry.submitEmptyGuestCacheCoherency(28) && backend->cacheCalls==callsBefore+3);
    assert(registry.completedWarmStateBatches()->readWord==25 && registry.completedEmptyGuestCacheCoherency()->readWord==37);
    // Resume state after cache, then cache again: both use only the live cursor.
    write(ring+28*4,0xC0013F00); write(ring+29*4,phys(ib)); write(ring+30*4,static_cast<std::uint32_t>(stateWords.size()));
    backend->stateBefore=[&] { assert(read(newWb)==28); };
    assert(registry.submitNativeStateBatch(31));
    assert(registry.completedWarmStateBatches()->batches==2 && registry.completedWarmStateBatches()->readWord==31);
    for (unsigned i=0;i<cacheRoot.size();++i) write(ring+(31+i)*4,cacheRoot[i]);
    backend->cacheBefore=[&] { assert(read(newWb)==31); };
    assert(registry.submitEmptyGuestCacheCoherency(34));
    assert(read(newWb)==34 && registry.completedWarmEmptyCacheCoherency()->batches==2);
    assert(registry.completedWarmEmptyCacheCoherency()->readWord==34 && registry.retiredGraphicsRingReadWord()==37);
    const std::array<std::uint32_t,6> pair{0x5C8,0x20000,0xE00,1,0xE40,1};
    write(ring+34*4,0xC0013F00);write(ring+35*4,phys(ib));write(ring+36*4,6);
    for(unsigned i=0;i<6;++i)write(ib+i*4,pair[i]);
    backend->stateBefore=[&]{assert(read(newWb)==34);};
    const auto pairUnchanged=[&]{assert(read(newWb)==34 && registry.graphicsRingConfiguration()->readWord==34 &&
        registry.experimentalIdleCachePairs()==0 && registry.completedWarmStateBatches()->batches==2);};
    for(unsigned i=0;i<6;++i) {
        write(ib+i*4,pair[i]^1);assert(!registry.submitNativeStateBatch(37));pairUnchanged();write(ib+i*4,pair[i]);
    }
    assert(backend->pairCalls==0);
    for(auto* flag:{&backend->throws,&backend->badSize,&backend->badDraws}){
        *flag=true;assert(!registry.submitNativeStateBatch(37));*flag=false;pairUnchanged();
    }
    backend->stateAccept=false;assert(!registry.submitNativeStateBatch(37));backend->stateAccept=true;pairUnchanged();
    backend->active=false;assert(!registry.submitNativeStateBatch(37));backend->active=true;pairUnchanged();
    assert(registry.submitNativeStateBatch(37) && read(newWb)==37 && registry.experimentalIdleCachePairs()==1);
    assert(!registry.submitNativeStateBatch(37) && backend->pairCalls==5);
    assert(registry.completedWarmStateBatches()->batches==3 && registry.completedWarmStateBatches()->words==4);
    // The larger draw child uses a separate READ-ONLY inspection path. Do not
    // expand submitNativeStateBatch's 256-word state-only admission limit.
    const auto colorChild=allocate(0x4000u,4u),colorSource=allocate(0x200000u,4u),colorCopy=allocate(0x400000u,4u);
    const auto readOnlyCopy=allocate(0x400000u,2u);
    NativeColorBatchFixture color(phys(colorSource),phys(colorCopy),phys(destination),phys(destination)+4);
    for(unsigned i=0;i<color.words.size();++i)write(colorChild+i*4,color.words[i]);
    for(unsigned i=0;i<8;++i)std::memcpy(space.translate(color.addresses[i]+0xA0000000),color.raw[i].data(),color.raw[i].size());
    std::memset(space.translate(colorCopy),0xCD,0x400000);
    write(destination,0xA5A5A5A5);write(destination+4,0xB6B6B6B6);
    write(ring+37*4,0xC0013F00);write(ring+38*4,phys(colorChild));write(ring+39*4,color.words.size());
    assert(!registry.submitNativeStateBatch(40));
    unsigned visits=0,inspectionRejections=0;
    const auto inspect=[&](const NativeColorBatchCapture& capture){++visits;
        assert(capture.commands().commands.words()==color.words && capture.memory().inputCount==8 &&
            capture.controls().previousWord==37 && capture.controls().nextWord==40);
        PackedNativeColorChainInputs packed;assert(capture.color().pack(packed) && packed.depth==color.raw[0]);return true;};
    const auto colorUnchanged=[&]{assert(read(newWb)==37 && registry.graphicsRingConfiguration()->readWord==37 &&
        registry.completedWarmStateBatches()->batches==3 && registry.experimentalIdleCachePairs()==1 &&
        read(destination)==0xA5A5A5A5 && read(destination+4)==0xB6B6B6B6 &&
        std::all_of(space.translate(colorCopy),space.translate(colorCopy)+0x400000,[](auto b){return b==0xCD;}));};
    assert(registry.inspectStoppedNativeColorBatch(*color.nativeProgram,{},40,inspect) && visits==1);colorUnchanged();
    assert(!registry.inspectStoppedPostOutputColorBatch(*color.nativeProgram,{},40,inspect) && visits==1);colorUnchanged();
    const auto rejectInspection=[&](unsigned cursor=40,DrawBinState bins=DrawBinState{}){const auto prior=visits;
        assert(!registry.inspectStoppedNativeColorBatch(*color.nativeProgram,bins,cursor,inspect) && visits==prior);
        ++inspectionRejections;colorUnchanged();};
    for(unsigned cursor:{37u,38u,39u,41u,UINT32_MAX})rejectInspection(cursor);
    rejectInspection(40,DrawBinState{{},16});
    for(unsigned i=0;i<3;++i){const auto original=read(ring+(37+i)*4);write(ring+(37+i)*4,i==2?4097:original^1);
        rejectInspection();write(ring+(37+i)*4,original);}
    write(newWb,38);const auto prior=visits;
    assert(!registry.inspectStoppedNativeColorBatch(*color.nativeProgram,{},40,inspect) && visits==prior);++inspectionRejections;write(newWb,37);
    backend->active=false;rejectInspection();backend->active=true;
    for(unsigned i=0;i<3;++i){auto* p=space.translate(color.addresses[i]+0xA0000000);*p^=1;rejectInspection();*p^=1;}
    write(colorChild+color.copyAddressWord*4,phys(readOnlyCopy));rejectInspection();
    write(colorChild+color.copyAddressWord*4,phys(colorCopy));
    write(ring+38*4,phys(colorChild)+0x4000-4);rejectInspection();write(ring+38*4,phys(colorChild));
    assert(!registry.inspectStoppedNativeColorBatch(*color.nativeProgram,{},40,{}));++inspectionRejections;colorUnchanged();
    assert(!registry.inspectStoppedNativeColorBatch(*color.nativeProgram,{},40,[&](const auto&){++visits;return false;}));++inspectionRejections;colorUnchanged();
    assert(!registry.inspectStoppedNativeColorBatch(*color.nativeProgram,{},40,[&](const auto&)->bool{++visits;throw std::runtime_error("inspector failure");}));
    ++inspectionRejections;colorUnchanged();
    assert(registry.inspectStoppedNativeColorBatch(*color.nativeProgram,{},40,inspect));colorUnchanged();
    const auto rejectColor=[&](unsigned cursor=40){const auto calls=backend->colorCalls;
        assert(!registry.submitExperimentalOwnedColorBatch(*color.nativeProgram,{},cursor));colorUnchanged();
        assert(!registry.completedNativeOwnedColorBatch() && !backend->colorCommitted);return backend->colorCalls-calls;};
    assert(!rejectColor(39));assert(rejectColor()==1); // Backend refusal, no publication.
    for(unsigned fault:{1u,2u}){backend->colorFault=fault;assert(rejectColor()==1);}backend->colorFault=0;
    backend->colorAccept=true;
    for(unsigned fault=3;fault<=7;++fault){backend->colorFault=fault;assert(rejectColor()==1);}backend->colorFault=0;
    backend->colorBefore=[&]{space.translate(colorSource)[0]^=1;};
    assert(rejectColor()==1);space.translate(colorSource)[0]^=1;backend->colorBefore=[]{};
    auto ownedColor=registry.snapshotStoppedNativeColorBatch(*color.nativeProgram,{},40);
    assert(ownedColor && ownedColor->commands().commands.words()==color.words);colorUnchanged();
    assert(!registry.snapshotStoppedNativeColorBatch(*color.nativeProgram,{},41));colorUnchanged();
    assert(!registry.runStoppedNativeColorDiagnostic(*color.nativeProgram,{},41,inspect));colorUnchanged();
    assert(!registry.runStoppedNativeColorDiagnostic(*color.nativeProgram,{},40,{}));colorUnchanged();
    assert(!registry.runStoppedNativeColorDiagnostic(*color.nativeProgram,{},40,[](const auto&){return false;}));colorUnchanged();
    assert(!registry.runStoppedNativeColorDiagnostic(*color.nativeProgram,{},40,[](const auto&)->bool{throw 1;}));colorUnchanged();
    // Synthetic callback only: no inert native fixture modules run on a GPU.
    // A registry free cannot proceed while the new backend scope is active.
    std::promise<void> backendAttempting,backendFreed;
    auto backendAttempted=backendAttempting.get_future(),backendFinished=backendFreed.get_future();
    std::thread backendFreer;
    assert(registry.runStoppedNativeColorDiagnostic(*color.nativeProgram,{},40,[&](const auto& capture){
        assert(inspect(capture));
        backendFreer=std::thread([&]{backendAttempting.set_value();dispatch("__imp__MmFreePhysicalMemory",{1,readOnlyCopy});backendFreed.set_value();});
        assert(backendAttempted.wait_for(std::chrono::seconds(1))==std::future_status::ready);
        assert(backendFinished.wait_for(std::chrono::milliseconds(25))==std::future_status::timeout);return true;
    }));
    backendFreer.join();assert(backendFinished.wait_for(std::chrono::seconds(1))==std::future_status::ready);colorUnchanged();
    std::puts("STOPPED_NATIVE_COLOR_BACKEND_SCOPE: allocation_free_blocked=1 four_rejection_controls=1 guest_publication=0 PASS");
    // The lifecycle mutex is retained through inspection. An external free is
    // allowed only after the callback leaves; this test runs no guest writers.
    std::promise<void> attempting,freed;auto attempted=attempting.get_future(),finished=freed.get_future();std::thread freer;
    assert(registry.inspectStoppedNativeColorBatch(*color.nativeProgram,{},40,[&](const auto& capture){
        assert(inspect(capture));freer=std::thread([&]{attempting.set_value();dispatch("__imp__MmFreePhysicalMemory",{1,colorCopy});freed.set_value();});
        assert(attempted.wait_for(std::chrono::seconds(1))==std::future_status::ready);
        assert(finished.wait_for(std::chrono::milliseconds(25))==std::future_status::timeout);return true;
    }));
    freer.join();assert(finished.wait_for(std::chrono::seconds(1))==std::future_status::ready);
    const auto beforeFreed=visits;assert(!registry.inspectStoppedNativeColorBatch(*color.nativeProgram,{},40,inspect) && visits==beforeFreed);
    ++inspectionRejections;assert(read(newWb)==37 && registry.graphicsRingConfiguration()->readWord==37);
    assert(!registry.snapshotStoppedNativeColorBatch(*color.nativeProgram,{},40));
    // The transferred bytes survive freed output and source allocations; that
    // independence is emphatically NOT permission to publish through old owners.
    dispatch("__imp__MmFreePhysicalMemory",{1,colorSource});
    PackedNativeColorChainInputs detached;assert(ownedColor->color().pack(detached) && detached.depth==color.raw[0]);
    assert(ownedColor->commands().commands.words()==color.words && read(newWb)==37);
    std::puts("STOPPED_NATIVE_COLOR_TRANSFER: owned_bytes_survive_freed_allocations=1 escaped_guest_pointers=0 future_lease=0 PASS");
    std::printf("STOPPED_NATIVE_COLOR_INSPECTION: %u rejections; allocation lock retained, read-only capture, no GPU or retirement PASS\n",inspectionRejections);
    // Fresh synthetic owners after the old inspection fixture was freed. No
    // stale snapshot/lease is used for the successful publication transaction.
    const auto liveSource=allocate(0x200000u,4u),liveCopy=allocate(0x400000u,4u),spare=allocate(0x1000u,4u);
    NativeColorBatchFixture liveColor(phys(liveSource),phys(liveCopy),phys(destination),phys(destination)+4);
    for(unsigned i=0;i<liveColor.words.size();++i)write(colorChild+i*4,liveColor.words[i]);
    for(unsigned i=0;i<8;++i)std::memcpy(space.translate(liveColor.addresses[i]+0xA0000000),liveColor.raw[i].data(),liveColor.raw[i].size());
    std::memset(space.translate(liveCopy),0xCD,0x400000);
    write(ring+37*4,0xC0013F00);write(ring+38*4,phys(colorChild));write(ring+39*4,liveColor.words.size());
    // A defective backend must not turn published RAM into a resumable false
    // result. Bounded child processes use MAP_PRIVATE guest memory; no GPU or
    // live worker exists at fork and every earlier fixture thread has joined.
    // The parent must keep all original outputs/state regardless of child work.
    std::fflush(nullptr);
    for(unsigned fault:{8u,9u}){
        backend->colorFault=fault;
        const auto childProcess=fork();assert(childProcess>=0);
        if(!childProcess){alarm(5);
            const bool returned=registry.submitExperimentalOwnedColorBatch(*liveColor.nativeProgram,{},40);
            (void)returned;std::_Exit(99); // Contract violation must never return.
        }
        int status=0;assert(waitpid(childProcess,&status,0)==childProcess);
        assert(WIFEXITED(status) && WEXITSTATUS(status)==4);
        assert(read(newWb)==37 && registry.graphicsRingConfiguration()->readWord==37 && !registry.completedNativeOwnedColorBatch() &&
            read(destination)==0xA5A5A5A5 && read(destination+4)==0xB6B6B6B6 && !backend->colorCommitted);
        assert(std::all_of(space.translate(liveCopy),space.translate(liveCopy)+0x400000,[](auto b){return b==0xCD;}));
    }
    backend->colorFault=0;
    std::puts("NATIVE_OWNED_COLOR_CONTRACT: two post-publication backend violations fail-stop4, parent state untouched PASS");
    std::promise<void> colorAttempting,colorFreed;auto colorAttempted=colorAttempting.get_future(),colorFinished=colorFreed.get_future();
    std::thread colorFreer;
    backend->colorBefore=[&]{assert(read(newWb)==37 && read(destination)==0xA5A5A5A5);
        colorFreer=std::thread([&]{colorAttempting.set_value();dispatch("__imp__MmFreePhysicalMemory",{1,spare});colorFreed.set_value();});
        assert(colorAttempted.wait_for(std::chrono::seconds(1))==std::future_status::ready);
        assert(colorFinished.wait_for(std::chrono::milliseconds(25))==std::future_status::timeout);
    };
    assert(registry.submitExperimentalOwnedColorBatch(*liveColor.nativeProgram,{},40));
    colorFreer.join();assert(colorFinished.wait_for(std::chrono::seconds(1))==std::future_status::ready);
    const auto colorDone=registry.completedNativeOwnedColorBatch();
    assert(colorDone && colorDone->previousWord==37 && colorDone->readWord==40 && colorDone->draws==4 && colorDone->eventWords==2 &&
        colorDone->copyBytes==3686400 && colorDone->edramBytes==3686400 && colorDone->nativeFences==4 && colorDone->policy==1);
    assert(read(newWb)==40 && registry.graphicsRingConfiguration()->readWord==40 && read(destination)==11 && read(destination+4)==22);
    assert(backend->colorCommitted && !registry.submitExperimentalOwnedColorBatch(*liveColor.nativeProgram,{},40));
    assert(!registry.submitNativeStateBatch(43) && !registry.submitEmptyGuestCacheCoherency(43));
    assert(registry.completedWarmStateBatches()->readWord==37); // Historical state-only record not relabeled.
    std::puts("NATIVE_OWNED_COLOR_REGISTRY: 10 rejection controls, real lifecycle scope, synthetic CPU publication only PASS");
    std::array<std::uint8_t,480> originalStack{};originalStack.fill(0xCD);
    std::copy(originalStack.begin(),originalStack.end(),space.translate(Stack));
    auto rejectSystem=[&](GuestImportCall call){
        assert(!registry.dispatch(systemGet,call).handled() && !registry.nativeSystemContext());
        assert(call.returnValue==systemCall.returnValue);
        assert(std::equal(originalStack.begin(),originalStack.end(),space.translate(Stack)));
        assert(read(newWb)==40 && registry.graphicsRingConfiguration()->readWord==40);
    };
    for(auto offset:{-4,1,4,92,148}){auto bad=systemCall;bad.arguments[0]+=offset;rejectSystem(bad);}
    for(auto offset:{-4,1,4,92}){auto bad=systemCall;bad.arguments[1]+=offset;rejectSystem(bad);}
    for(auto sp:{0u,Stack+1,0xFFFFFFF0u,0x7FC80000u,0x10000000u,ring}){
        auto bad=systemCall;bad.stackPointer=sp;bad.arguments={std::uint32_t(sp+208),std::uint32_t(sp+116)};rejectSystem(bad);
    }
    write(newWb,39);assert(!registry.dispatch(systemGet,systemCall).handled() && !registry.nativeSystemContext());write(newWb,40);
    backend->active=false;rejectSystem(systemCall);backend->active=true;
    const auto savedId=read(*registry.graphicsSystemIdentifierAddress());
    assert(registry.dispatch(systemGet,systemCall).handled() && systemCall.returnValue==0xABCDEF0123456789);
    const auto systemContext=registry.nativeSystemContext();
    assert(systemContext && systemContext->stackPointer==Stack && systemContext->buffer==Stack+208 &&
        systemContext->identifierOutput==Stack+116 && systemContext->readWord==40 && systemContext->policy==1);
    for(unsigned i=0;i<originalStack.size();++i)
        assert(space.translate(Stack)[i]==((i>=116 && i<120)||(i>=208 && i<356)?0:0xCD));
    assert(read(newWb)==40 && registry.graphicsRingConfiguration()->readWord==40 &&
        read(*registry.graphicsSystemIdentifierAddress())==savedId);
    assert(!registry.dispatch(systemGet,systemCall).handled());
    assert(registry.dispatch("__imp__VdSwap",systemCall).state==GuestImportDispatchState::Rejected);
    std::puts("NATIVE_NO_SYSTEM_OVERLAY: bounded empty context, 17 rejection controls, no GPU identifier/retirement/swap PASS");
    GuestImportCall swapCall;swapCall.stackPointer=Stack;
    swapCall.arguments={colorChild+0x3000,Stack+128,*registry.graphicsSystemIdentifierAddress(),Stack+208,0,Stack+108,Stack+112,Stack+104};
    write(Stack+84,Stack+96);write(Stack+92,Stack+100);write(Stack+96,1280);write(Stack+100,720);
    write(Stack+104,0);write(Stack+108,liveCopy);write(Stack+112,6);
    for(unsigned i=0;i<6;++i)write(Stack+128+i*4,0x12340000+i);
    const auto inspectedSwap=registry.inspectStoppedNativeSwapRequest(swapCall);assert(inspectedSwap);
    assert(inspectedSwap->commandVirtual==colorChild+0x3000 && inspectedSwap->commandPhysical==phys(colorChild)+0x3000 &&
        inspectedSwap->allocationBase==colorChild && inspectedSwap->allocationBytes==0x4000 && inspectedSwap->protection==4 &&
        inspectedSwap->frontbufferVirtual==liveCopy && inspectedSwap->format==6 && inspectedSwap->colorSpace==0 &&
        inspectedSwap->width==1280 && inspectedSwap->height==720 && inspectedSwap->identifierWord==savedId);
    for(unsigned i=0;i<6;++i)assert(inspectedSwap->fetch[i]==0x12340000+i);
    for(unsigned arg=0;arg<8;++arg){auto bad=swapCall;bad.arguments[arg]^=1;assert(!registry.inspectStoppedNativeSwapRequest(bad));}
    auto badSwap=swapCall;badSwap.stackPointer+=16;assert(!registry.inspectStoppedNativeSwapRequest(badSwap));
    for(auto address:{0u,ring,newWb,*registry.graphicsSystemIdentifierAddress(),readOnly,colorChild+0x3F04}){
        auto bad=swapCall;bad.arguments[0]=address;assert(!registry.inspectStoppedNativeSwapRequest(bad));
    }
    for(auto address:{Stack+84,Stack+92,Stack+116,Stack+208,newWb}){
        auto saved=read(address);write(address,saved^1);assert(!registry.inspectStoppedNativeSwapRequest(swapCall));write(address,saved);
    }
    assert(registry.inspectStoppedNativeSwapRequest(swapCall) && registry.nativeSystemContext()->readWord==40 &&
        read(newWb)==40 && read(*registry.graphicsSystemIdentifierAddress())==savedId);
    std::puts("STOPPED_NATIVE_SWAP_REQUEST: 20 rejection controls, owned metadata only, no command writes or presentation PASS");
    unsigned imageVisits=0,imageRejections=0;
    const auto checkImage=[&](const GuestImportRegistry::NativeOwnedSwapImage& image){
        ++imageVisits;
        assert(image.physicalAddress==phys(liveCopy) && image.request.frontbufferVirtual==liveCopy &&
            image.owner.physicalBase==phys(liveCopy) && image.owner.virtualBase==liveCopy &&
            image.owner.byteCount==0x400000 && image.owner.protection==4 && image.owner.writable &&
            image.rgba.size()==NativeSwapImagePlan::PixelBytes);
        for(unsigned i=0;i<image.rgba.size();++i)assert(image.rgba[i]==(i%4==3?255:0));
        return true;
    };
    const auto rejectImage=[&](const GuestImportCall& call){auto prior=imageVisits;
        assert(!registry.visitNativeOwnedSwapImage(call,checkImage) && imageVisits==prior);++imageRejections;
        assert(read(newWb)==40 && read(*registry.graphicsSystemIdentifierAddress())==savedId);
    };
    rejectImage(swapCall); // Raw fetch metadata was deliberately NOT a layout.
    const std::array<std::uint32_t,6> imageFetch{0x8A000002,liveCopy|6,0x0059E4FF,0x1414,0,0x200};
    for(unsigned i=0;i<6;++i)write(Stack+128+i*4,imageFetch[i]);
    assert(registry.visitNativeOwnedSwapImage(swapCall,checkImage) && imageVisits==1);
    assert(!registry.visitNativeOwnedSwapImage(swapCall,{}));
    assert(!registry.visitNativeOwnedSwapImage(swapCall,[](const auto&){return false;}));
    assert(!registry.visitNativeOwnedSwapImage(swapCall,[](const auto&)->bool{throw 1;}));
    for(unsigned field=0;field<6;++field)for(unsigned bit=0;bit<32;++bit){
        write(Stack+128+field*4,imageFetch[field]^(1u<<bit));rejectImage(swapCall);
        write(Stack+128+field*4,imageFetch[field]);
    }
    for(auto offset:{96u,100u,104u,108u,112u}){auto original=read(Stack+offset);
        write(Stack+offset,original^1);rejectImage(swapCall);write(Stack+offset,original);
    }
    backend->active=false;rejectImage(swapCall);backend->active=true;
    auto aliasSwap=swapCall;aliasSwap.arguments[0]=liveCopy;rejectImage(aliasSwap);
    // Same bytes/layout/capacity in a DIFFERENT allocation do not establish
    // provenance from the live publication. Neither numeric count nor pixels
    // alone is used as the copy identity.
    const auto unrelated=allocate(0x400000u,4u);
    std::memcpy(space.translate(unrelated),space.translate(liveCopy),0x400000);
    write(Stack+108,unrelated);write(Stack+132,unrelated|6);rejectImage(swapCall);
    write(Stack+108,liveCopy);write(Stack+132,liveCopy|6);
    for(unsigned y:{0u,31u,32u,359u,719u})for(unsigned x:{0u,31u,32u,639u,1279u})for(unsigned lane=0;lane<4;++lane){
        unsigned offset=0;assert(colorCopyOffset(1280,720,x,y,offset));space.translate(liveCopy)[offset+lane]^=1;
        rejectImage(swapCall);space.translate(liveCopy)[offset+lane]^=1;
    }
    std::vector<std::uint8_t> active(NativeSwapImagePlan::Capacity/4);
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x){unsigned offset=0;
        assert(colorCopyOffset(1280,720,x,y,offset));active[offset/4]=1;}
    unsigned hole=0;while(active[hole])++hole;
    space.translate(liveCopy)[hole*4]^=1;
    assert(registry.visitNativeOwnedSwapImage(swapCall,checkImage));
    space.translate(liveCopy)[hole*4]^=1;
    const auto commandBefore=std::vector<std::uint8_t>(space.translate(colorChild+0x3000),space.translate(colorChild+0x3100));
    // Registry-only CPU transaction fixture, NOT native GPU execution. Fork
    // while all earlier threads are joined, so the parent retains its original
    // copy/command/stack and the D-228 lifetime controls still run afterward.
    std::fflush(nullptr);
    const auto swapChild=fork();assert(swapChild>=0);
    if(!swapChild){alarm(50); // Encloses storage/wrapper/scaler/color fixtures, including UBSan.
        const auto commandVirtual=colorChild+0x3000,commandPhysical=phys(commandVirtual);
        unsigned calls=0;
        swapTestCallback=[&](const auto& image){++calls;assert(checkImage(image));return false;};
        assert(!registry.nativeSwapCommandStatus().issued && !registry.submitExperimentalNativeSwapCommand(43));
        auto invalid=swapCall;invalid.arguments[0]+=1;
        assert(!registry.issueExperimentalNativeSwapCommand(invalid));
        assert(registry.dispatch("__imp__VdSwap",invalid).state==GuestImportDispatchState::Rejected);
        assert(std::equal(commandBefore.begin(),commandBefore.end(),space.translate(commandVirtual)));
        const auto syncAlias=fork();assert(syncAlias>=0);
        if(!syncAlias){alarm(5);dispatch("__imp__RtlInitializeCriticalSectionAndSpinCount",{commandVirtual+128,10000});
            const auto before=std::vector<std::uint8_t>(space.translate(commandVirtual),space.translate(commandVirtual)+256);
            assert(!registry.issueExperimentalNativeSwapCommand(swapCall) && !registry.nativeSwapCommandStatus().issued &&
                std::equal(before.begin(),before.end(),space.translate(commandVirtual)));std::_Exit(0);}
        int syncStatus=0;assert(waitpid(syncAlias,&syncStatus,0)==syncAlias && WIFEXITED(syncStatus) && WEXITSTATUS(syncStatus)==0);
        swapCall.returnValue=0x123456789ULL;
        const auto originalArguments=swapCall.arguments;
        assert(registry.dispatch("__imp__VdSwap",swapCall).handled() && !calls);
        assert(swapCall.returnValue==0x123456789ULL && swapCall.arguments==originalArguments);
        auto status=registry.nativeSwapCommandStatus();
        assert(status.issued && status.pending && !status.presentQueued && status.previousWord==40 && status.readWord==40);
        for(auto address:{ring,newWb,*registry.graphicsSystemIdentifierAddress(),colorChild,commandVirtual,liveCopy}){
            const auto original=read(address);GuestImportCall fill;fill.arguments={address,4,0};fill.returnValue=0x123456789ULL;
            assert(!registry.dispatch("__imp__RtlFillMemoryUlong",fill).handled() && fill.returnValue==0x123456789ULL);
            assert(read(address)==original && !calls && registry.nativeSwapCommandStatus().pending);}
        std::puts("ULONG_FILL_GRAPHICS: pinned ring/writeback/identifier/command/image remain unchanged PASS");
        GuestImportCall mode;mode.arguments[0]=0x40000000;mode.returnValue=0x123456789ULL;
        constexpr auto setMode="__imp__VdSetDisplayMode",getInfo="__imp__VdGetCurrentDisplayInformation";
        assert(!registry.dispatch(setMode,mode).handled() && !registry.nativeDisplayModeStatus().selected);
        backend->displayAccept=true;
        for(unsigned bit=0;bit<32;++bit){auto bad=mode;bad.arguments[0]^=1U<<bit;
            assert(!registry.dispatch(setMode,bad).handled() && !registry.nativeDisplayModeStatus().selected);}
        backend->active=false;assert(!registry.dispatch(setMode,mode).handled());backend->active=true;
        write(newWb,41);assert(!registry.dispatch(setMode,mode).handled());write(newWb,40);
        assert(registry.dispatch(setMode,mode).handled() && mode.returnValue==0x123456789ULL);
        assert(registry.nativeDisplayModeStatus().selected && registry.nativeDisplayModeStatus().flags==0x40000000);
        assert(!registry.dispatch(setMode,mode).handled());
        constexpr auto nested=Stack-240;std::memset(space.translate(nested),0xCD,240);
        GuestImportCall info;info.stackPointer=nested;info.arguments[0]=nested+128;info.returnValue=0x123456789ULL;
        const auto infoAlias=fork();assert(infoAlias>=0);
        if(!infoAlias){alarm(5);dispatch("__imp__RtlInitializeCriticalSectionAndSpinCount",{nested+128,10000});
            const auto before=std::vector<std::uint8_t>(space.translate(nested),space.translate(nested)+240);
            assert(!registry.dispatch(getInfo,info).handled() && !registry.nativeDisplayModeStatus().queries &&
                std::equal(before.begin(),before.end(),space.translate(nested)));std::_Exit(0);}
        int infoAliasStatus=0;assert(waitpid(infoAlias,&infoAliasStatus,0)==infoAlias && WIFEXITED(infoAliasStatus) && WEXITSTATUS(infoAliasStatus)==0);
        const auto untouched=[&]{for(unsigned i=0;i<240;++i)assert(space.translate(nested)[i]==0xCD);};
        for(unsigned bit=0;bit<32;++bit){auto bad=info;bad.arguments[0]^=1U<<bit;
            assert(!registry.dispatch(getInfo,bad).handled());untouched();}
        for(unsigned bit=0;bit<32;++bit){auto bad=info;bad.stackPointer^=1U<<bit;
            assert(!registry.dispatch(getInfo,bad).handled());untouched();}
        backend->displayQuery=false;assert(!registry.dispatch(getInfo,info).handled());untouched();backend->displayQuery=true;
        for(unsigned i=0;i<4;++i){backend->displayConfiguration.dimensions[i]^=1;
            assert(!registry.dispatch(getInfo,info).handled());untouched();backend->displayConfiguration.dimensions[i]^=1;}
        backend->displayConfiguration.scaler[7]=0x7f800000;
        assert(!registry.dispatch(getInfo,info).handled());untouched();backend->displayConfiguration.scaler[7]=0;
        std::array<std::uint8_t,88> expectedInfo{};
        assert(makeNativeDisplayInformation(backend->displayConfiguration,expectedInfo));
        assert(registry.dispatch(getInfo,info).handled() && info.returnValue==0x123456789ULL);
        assert(registry.nativeDisplayModeStatus().queries==1 && read(newWb)==40 && !calls);
        for(unsigned i=0;i<240;++i)assert(space.translate(nested)[i]==(i>=128 && i<216?expectedInfo[i-128]:0xCD));
        assert(read(nested+200)==0x050002d0 && read(nested+204)==0x42700000 && read(nested+212)==0x00000500);
        assert(!registry.dispatch(getInfo,info).handled());
        std::puts("NATIVE_DISPLAY_MODE: exact flags, 96 bit controls, bounded88byte readback, backend refusal, no presentation/retirement PASS");
        assert(status.commandPhysical==commandPhysical && read(newWb)==40 && read(*registry.graphicsSystemIdentifierAddress())==savedId);
        assert(!registry.issueExperimentalNativeSwapCommand(swapCall));
        const auto issued=NativeSwapCommand::make({1,commandPhysical,phys(liveCopy),40});assert(issued);
        assert(issued->matches(commandPhysical,space.translate(commandVirtual),256));
        // Separate CPU-only transaction: the real stream has a command-storage
        // fence before the swap root. It must not rewrite the issued ticket or
        // publish a frame, and the original isolated-root controls below remain.
        std::fflush(nullptr);const auto storageChild=fork();assert(storageChild>=0);
        if(!storageChild){alarm(45);
            const auto childAddress=colorChild+0x3E00;
            const std::array<std::uint32_t,11> words{0x00000A31,0x01000000,0x00010A2F,
                0x4000,phys(colorChild),0xC0043C00,3,0xA31,0,0x80000000,8};
            write(ring+40*4,0xC0013F00);write(ring+41*4,phys(childAddress));write(ring+42*4,11);
            for(unsigned i=0;i<words.size();++i)write(childAddress+i*4,words[i]);
            const auto unchanged=[&]{const auto s=registry.nativeSwapCommandStatus();
                assert(s.pending && !s.presentQueued && s.previousWord==40 && s.readWord==40 && s.issuedReadWord==40 &&
                    read(newWb)==40 && !registry.completedNativeCommandStorageCoherency() && !calls &&
                    issued->matches(commandPhysical,space.translate(commandVirtual),256));};
            const auto reject=[&](unsigned cursor=43){const auto before=backend->commandCalls;
                assert(!registry.submitNativeCommandStorageCoherency(cursor) && backend->commandCalls==before);unchanged();};
            for(unsigned cursor:{0u,40u,41u,42u,44u,UINT32_MAX})reject(cursor);
            for(unsigned field:{0u,2u})for(unsigned bit=0;bit<32;++bit){const auto at=ring+(40+field)*4,saved=read(at);
                write(at,saved^(1u<<bit));reject();write(at,saved);}
            for(unsigned address:{phys(childAddress)+1,phys(commandVirtual),phys(liveCopy),0x20000000u}){
                write(ring+41*4,address);reject();write(ring+41*4,phys(childAddress));}
            for(unsigned field=0;field<words.size();++field)for(unsigned bit=0;bit<32;++bit){
                write(childAddress+field*4,words[field]^(1u<<bit));reject();write(childAddress+field*4,words[field]);}
            space.translate(commandVirtual)[8]^=1;
            assert(!registry.submitNativeCommandStorageCoherency(43) && !backend->commandCalls);
            space.translate(commandVirtual)[8]^=1;unchanged();
            space.translate(liveCopy)[0]^=1;reject();space.translate(liveCopy)[0]^=1;
            write(newWb,41);assert(!registry.submitNativeCommandStorageCoherency(43) && !backend->commandCalls);
            write(newWb,40);backend->active=false;reject();backend->active=true;
            assert(!registry.submitNativeCommandStorageCoherency(43) && backend->commandCalls==1);unchanged();
            backend->commandThrow=true;
            assert(!registry.submitNativeCommandStorageCoherency(43) && backend->commandCalls==2);unchanged();
            backend->commandThrow=false;backend->commandAccept=true;
            for(unsigned at:{ring+40*4,childAddress,commandVirtual,liveCopy,newWb}){
                const auto violation=fork();assert(violation>=0);
                if(!violation){alarm(5);backend->commandBefore=[&]{write(at,read(at)^1);};
                    (void)registry.submitNativeCommandStorageCoherency(43);std::_Exit(99);}
                int result=0;assert(waitpid(violation,&result,0)==violation && WIFEXITED(result) && WEXITSTATUS(result)==4);
                unchanged();
            }
            assert(registry.submitNativeCommandStorageCoherency(43) && backend->commandCalls==3);
            const auto completion=registry.completedNativeCommandStorageCoherency();assert(completion);
            assert(completion->previousWord==40 && completion->readWord==43 && completion->request.physicalBase==phys(colorChild) &&
                completion->request.byteCount==0x4000 && completion->request.cacheFlags==0x01000000);
            auto progress=registry.nativeSwapCommandStatus();
            assert(progress.pending && !progress.presentQueued && progress.previousWord==43 && progress.readWord==43 &&
                progress.issuedReadWord==40 && read(newWb)==43 && !calls &&
                issued->matches(commandPhysical,space.translate(commandVirtual),256));
            for(auto target:{colorChild,liveCopy}){GuestImportCall free;free.arguments={1,target};
                assert(!registry.dispatch("__imp__MmFreePhysicalMemory",free).handled());}
            assert(!registry.submitNativeCommandStorageCoherency(46) && backend->commandCalls==3);
            std::fflush(nullptr);const auto wrapperChild=fork();assert(wrapperChild>=0);
            if(!wrapperChild){alarm(40);
                const auto wrapper=commandVirtual-40;
                const std::array<std::uint32_t,4> targets{newWb+64,newWb+68,newWb+16,newWb+20};
                const std::array<std::uint32_t,10> prefix{0x5C8,0x20000,0xE00,1,0xE40,1,0xC0025800,0x80000003,phys(targets[0]),0xDEADBEEF};
                const std::array<std::uint32_t,16> tail{0xC0025800,0x80000003,phys(targets[1]),0xDEADBEEF,
                    0xC0003B00,0x7FFF,0xC0001000,0x77777777,0xC0025800,3,phys(targets[2])|2,0x12345678,
                    0xC0025800,3,phys(targets[3])|2,0xABCDEF01};
                for(unsigned i=0;i<10;++i)write(wrapper+i*4,prefix[i]);
                for(unsigned i=0;i<16;++i)write(wrapper+(74+i)*4,tail[i]);
                write(ring+43*4,0xC0013F00);write(ring+44*4,phys(wrapper));write(ring+45*4,90);
                for(auto address:targets)write(address,0xCCCCCCCC);
                const auto pairCount=registry.experimentalIdleCachePairs();
                const auto untouched=[&]{const auto s=registry.nativeSwapCommandStatus();
                    assert(s.pending && !s.presentQueued && s.readWord==43 && s.issuedReadWord==40 && read(newWb)==43 &&
                        !registry.completedNativeWrappedSwap() && registry.experimentalIdleCachePairs()==pairCount &&
                        read(*registry.graphicsSystemIdentifierAddress())==savedId);
                    for(auto address:targets)assert(read(address)==0xCCCCCCCC);
                };
                const auto reject=[&](unsigned cursor=46){const auto before=backend->wrapperCalls;
                    assert(!registry.submitNativeWrappedSwapCommand(cursor) && backend->wrapperCalls==before);untouched();};
                for(unsigned cursor:{0u,43u,44u,45u,47u,UINT32_MAX})reject(cursor);
                for(unsigned field=0;field<3;++field)for(unsigned bit=0;bit<32;++bit){const auto at=ring+(43+field)*4,saved=read(at);
                    write(at,saved^(1u<<bit));reject();write(at,saved);}
                // Immutable non-ticket words. Ticket bit integrity has2048
                // dedicated controls; dynamic destinations/literals below.
                unsigned immutableBits=0;
                for(unsigned field=0;field<90;++field){
                    if((field>=10 && field<74) || field==8 || field==76 || field==84 || field==85 || field==88 || field==89)continue;
                    const auto at=wrapper+field*4,saved=read(at);
                    for(unsigned bit=0;bit<32;++bit){write(at,saved^(1u<<bit));reject();write(at,saved);++immutableBits;}
                }
                for(unsigned field:{8u,76u,84u,88u}){const auto at=wrapper+field*4,saved=read(at),endian=field<80?0u:2u;
                    for(auto address:{ring,commandVirtual,liveCopy,newWb,newWb+8,readOnly,unrelated,newWb+4096}){
                        write(at,phys(address)|endian);reject();write(at,saved);}
                }
                for(unsigned i=0;i<4;++i){const auto sync=fork();assert(sync>=0);
                    if(!sync){alarm(5);dispatch("__imp__RtlInitializeCriticalSectionAndSpinCount",{targets[i],10000});
                        const auto before=backend->wrapperCalls;
                        assert(!registry.submitNativeWrappedSwapCommand(46) && backend->wrapperCalls==before);std::_Exit(0);}
                    int status=0;assert(waitpid(sync,&status,0)==sync && WIFEXITED(status) && WEXITSTATUS(status)==0);
                }
                for(auto target:{colorChild,liveCopy,newWb}){GuestImportCall free;free.arguments={1,target};
                    assert(!registry.dispatch("__imp__MmFreePhysicalMemory",free).handled());}
                for(unsigned fault:{0u,1u,2u}){backend->wrapperFault=fault;
                    assert(!registry.submitNativeWrappedSwapCommand(46) && !calls);untouched();}
                backend->wrapperFault=0;backend->wrapperAccept=true;
                assert(!registry.submitNativeWrappedSwapCommand(46) && calls==1);untouched();
                swapTestCallback=[&](const auto& image)->bool{++calls;assert(checkImage(image));throw std::runtime_error("pre-accept");};
                assert(!registry.submitNativeWrappedSwapCommand(46) && calls==2);untouched();
                swapTestCallback=[&](const auto& image){++calls;assert(checkImage(image));
                    // All guest events and RPTR remain old during callback.
                    assert(read(newWb)==43);for(auto at:targets)assert(read(at)==0xCCCCCCCC);return true;};
                unsigned failStops=0;
                const auto checkExit=[&](unsigned fault,unsigned at){const auto violation=fork();assert(violation>=0);
                    if(!violation){alarm(5);backend->wrapperFault=fault;
                        if(at)backend->wrapperAfter=[&]{write(at,read(at)^1);};
                        (void)registry.submitNativeWrappedSwapCommand(46);std::_Exit(99);}
                    int status=0;assert(waitpid(violation,&status,0)==violation && WIFEXITED(status) && WEXITSTATUS(status)==4);
                    untouched();++failStops;
                };
                for(unsigned fault=3;fault<=9;++fault)checkExit(fault,0);
                for(unsigned at:{ring+43*4,wrapper,wrapper+85*4,commandVirtual,liveCopy,newWb,targets[0],targets[3]})checkExit(0,at);
                assert(registry.submitNativeWrappedSwapCommand(46) && calls==3);
                const auto done=registry.completedNativeWrappedSwap();assert(done && done->previousWord==43 && done->readWord==46 &&
                    done->childPhysical==phys(wrapper) && done->gpu.nativeFences==4 && read(newWb)==46);
                assert(read(targets[0])==0xF8FFFFFF && read(targets[1])==0x10000000 &&
                    read(targets[2])==0x12345678 && read(targets[3])==0xABCDEF01 &&
                    registry.experimentalIdleCachePairs()==pairCount+1 && read(*registry.graphicsSystemIdentifierAddress())==savedId);
                assert(!registry.submitNativeWrappedSwapCommand(49) && registry.nativeSwapCommandStatus().presentQueued);
                testNativeScalerProducer(registry,space,*backend,ring,newWb,colorChild,liveCopy,swapTestCallback);
                GuestImportCall freeCommand;freeCommand.arguments={1,colorChild};
                assert(!registry.dispatch("__imp__MmFreePhysicalMemory",freeCommand).handled());
                dispatch("__imp__MmFreePhysicalMemory",{1,liveCopy});
                registry.reset();assert(!registry.completedNativeWrappedSwap());
                assert(!registry.completedNativeScalerStorageCoherency());
                assert(!registry.nativeScalerCommandStatus().issued && !registry.bindExperimentalNativeIdentityScaler());
                std::printf("NATIVE_WRAPPED_SWAP: immutable_bits=%u root_bits=96 post_accept_fail_stops=%u pinned_control_events=4 deferred_publication=1 PASS; synthetic CPU only\n",immutableBits,failStops);
                std::fflush(nullptr);std::_Exit(0);
            }
            int wrapperStatus=0;assert(waitpid(wrapperChild,&wrapperStatus,0)==wrapperChild && WIFEXITED(wrapperStatus) && WEXITSTATUS(wrapperStatus)==0);
            // Opaque prefix/tail remain read-only diagnostic bytes. This does
            // not authorize the execution path to ignore their unknown meaning.
            const auto wrapper=commandVirtual-40;
            write(ring+43*4,0xC0013F00);write(ring+44*4,phys(wrapper));write(ring+45*4,80);
            const auto inspected=registry.inspectStoppedNativePendingSubmission(46);assert(inspected);
            assert(inspected->rootPhysical==phys(ring)+43*4 && inspected->childPhysical==phys(wrapper) &&
                inspected->childWords==80 && inspected->commandOffsetWords==10 && inspected->readWord==43 && inspected->writeWord==46);
            assert(!registry.submitExperimentalNativeSwapCommand(46) && !calls && read(newWb)==43);
            for(unsigned cursor:{0u,43u,44u,45u,47u,UINT32_MAX})assert(!registry.inspectStoppedNativePendingSubmission(cursor));
            for(unsigned bit=0;bit<32;++bit){const auto at=ring+43*4,saved=read(at);
                write(at,saved^(1u<<bit));assert(!registry.inspectStoppedNativePendingSubmission(46));write(at,saved);}
            for(unsigned address:{phys(wrapper)+1,phys(commandVirtual)+4,phys(liveCopy),0x20000000u}){
                write(ring+44*4,address);assert(!registry.inspectStoppedNativePendingSubmission(46));}
            write(ring+44*4,phys(wrapper));
            for(unsigned words:{0u,63u,64u,73u,1025u,UINT32_MAX}){
                write(ring+45*4,words);assert(!registry.inspectStoppedNativePendingSubmission(46));}
            write(ring+45*4,80);
            for(unsigned at:{commandVirtual,liveCopy,newWb}){const auto saved=read(at);write(at,saved^1);
                assert(!registry.inspectStoppedNativePendingSubmission(46));write(at,saved);}
            assert(read(newWb)==43 && !calls && backend->commandCalls==3 && registry.nativeSwapCommandStatus().pending);
            std::puts("NATIVE_PENDING_SUBMISSION_INSPECTION: bounded wrapper,32 root bit controls, immutable ticket, no execution or retirement PASS");
            write(ring+43*4,0xC0013F00);write(ring+44*4,commandPhysical);write(ring+45*4,64);
            swapTestCallback=[&](const auto& image){++calls;assert(checkImage(image));return true;};
            assert(!registry.submitExperimentalNativeSwapCommand(43));
            assert(registry.submitExperimentalNativeSwapCommand(46) && calls==1);
            progress=registry.nativeSwapCommandStatus();
            assert(!progress.pending && progress.presentQueued && progress.previousWord==43 && progress.readWord==46 &&
                progress.issuedReadWord==40 && read(newWb)==46 && read(*registry.graphicsSystemIdentifierAddress())==savedId);
            dispatch("__imp__MmFreePhysicalMemory",{1,colorChild});dispatch("__imp__MmFreePhysicalMemory",{1,liveCopy});
            registry.reset();assert(!registry.completedNativeCommandStorageCoherency());
            std::puts("NATIVE_COMMAND_STORAGE_COHERENCY: 416 packet bit controls, 5 post-fence fail-stops, immutable ticket, fence-only retirement then separate swap PASS; synthetic CPU only");
            std::fflush(nullptr);std::_Exit(0);
        }
        int storageStatus=0;const auto waitedStorage=waitpid(storageChild,&storageStatus,0);
        if(waitedStorage!=storageChild || !WIFEXITED(storageStatus) || WEXITSTATUS(storageStatus))
            std::fprintf(stderr,"STORAGE_FIXTURE_EXIT: waited=%d signal=%d raw_status=%d\n",waitedStorage==storageChild,
                WIFSIGNALED(storageStatus)?WTERMSIG(storageStatus):0,storageStatus);
        assert(waitedStorage==storageChild && WIFEXITED(storageStatus) && WEXITSTATUS(storageStatus)==0);
        for(auto target:{colorChild,liveCopy}){GuestImportCall free;free.arguments={1,target};
            assert(!registry.dispatch("__imp__MmFreePhysicalMemory",free).handled());}
        const auto cancelled=fork();assert(cancelled>=0);
        if(!cancelled){alarm(5);registry.reset();assert(!registry.nativeSwapCommandStatus().issued && !registry.nativeSwapCommandStatus().pending &&
            !registry.submitExperimentalNativeSwapCommand(43));std::_Exit(0);}
        int cancelStatus=0;assert(waitpid(cancelled,&cancelStatus,0)==cancelled && WIFEXITED(cancelStatus) && WEXITSTATUS(cancelStatus)==0);
        write(ring+40*4,0xC0013F00);write(ring+41*4,commandPhysical);write(ring+42*4,64);
        const auto rejectPending=[&](unsigned cursor=43){auto prior=calls;
            assert(!registry.submitExperimentalNativeSwapCommand(cursor) && calls==prior && read(newWb)==40 &&
                registry.nativeSwapCommandStatus().pending && !registry.nativeSwapCommandStatus().presentQueued);
        };
        for(unsigned cursor:{0u,40u,41u,42u,44u,UINT32_MAX})rejectPending(cursor);
        for(unsigned field=0;field<3;++field)for(unsigned bit=0;bit<32;++bit){
            const auto address=ring+(40+field)*4,saved=read(address);write(address,saved^(1u<<bit));rejectPending();write(address,saved);
        }
        for(unsigned byte=0;byte<256;++byte)for(unsigned bit=0;bit<8;++bit){
            space.translate(commandVirtual)[byte]^=1u<<bit;rejectPending();space.translate(commandVirtual)[byte]^=1u<<bit;
        }
        for(unsigned lane=0;lane<4;++lane){space.translate(liveCopy)[lane]^=1;rejectPending();space.translate(liveCopy)[lane]^=1;}
        write(newWb,39);assert(!registry.submitExperimentalNativeSwapCommand(43) && !calls);write(newWb,40);
        backend->active=false;rejectPending();backend->active=true;
        const auto originalIdentifier=*registry.graphicsSystemIdentifierAddress();
        dispatch("__imp__VdSetSystemCommandBufferGpuIdentifierAddress",{0});rejectPending();
        dispatch("__imp__VdSetSystemCommandBufferGpuIdentifierAddress",{originalIdentifier});
        // The caller's original stack no longer supplies borrowed request
        // fields after issuance. Consuming uses the immutable owned descriptor.
        std::memset(space.translate(Stack),0xEA,480);
        assert(!registry.submitExperimentalNativeSwapCommand(43) && calls==1 && read(newWb)==40);
        swapTestCallback=[&](const auto&)->bool{++calls;throw std::runtime_error("pre-accept refusal");};
        assert(!registry.submitExperimentalNativeSwapCommand(43) && calls==2 && read(newWb)==40);
        // A callback that claims acceptance and then changed protected input
        // must fail-stop4. This child has private synthetic memory and no GPU.
        std::fflush(nullptr);const auto violation=fork();assert(violation>=0);
        if(!violation){alarm(5);swapTestCallback=[&](const auto&){write(ring+40*4,0);return true;};
            (void)registry.submitExperimentalNativeSwapCommand(43);std::_Exit(99);}
        int violationStatus=0;assert(waitpid(violation,&violationStatus,0)==violation && WIFEXITED(violationStatus) && WEXITSTATUS(violationStatus)==4);
        assert(read(newWb)==40 && registry.nativeSwapCommandStatus().pending);
        std::promise<void> submitAttempting,submitFreed;auto submitAttempted=submitAttempting.get_future(),submitFinished=submitFreed.get_future();
        std::thread submitFreer;
        swapTestCallback=[&](const auto& image){++calls;assert(checkImage(image));
            submitFreer=std::thread([&]{submitAttempting.set_value();dispatch("__imp__MmFreePhysicalMemory",{1,colorChild});submitFreed.set_value();});
            assert(submitAttempted.wait_for(std::chrono::seconds(1))==std::future_status::ready);
            assert(submitFinished.wait_for(std::chrono::milliseconds(25))==std::future_status::timeout);return true;
        };
        assert(registry.submitExperimentalNativeSwapCommand(43));submitFreer.join();
        assert(submitFinished.wait_for(std::chrono::seconds(1))==std::future_status::ready && calls==3);
        status=registry.nativeSwapCommandStatus();
        assert(status.issued && !status.pending && status.presentQueued && status.readWord==43 &&
            read(newWb)==43 && registry.graphicsRingConfiguration()->readWord==43 &&
            read(*registry.graphicsSystemIdentifierAddress())==savedId);
        assert(!registry.submitExperimentalNativeSwapCommand(43) && !registry.issueExperimentalNativeSwapCommand(swapCall));
        dispatch("__imp__MmFreePhysicalMemory",{1,liveCopy});
        assert(registry.dispatch("__imp__VdSwap",swapCall).state==GuestImportDispatchState::Rejected);
        registry.reset();assert(!registry.nativeSwapCommandStatus().issued && !registry.submitExperimentalNativeSwapCommand(43));
        assert(!registry.nativeDisplayModeStatus().selected && !registry.nativeDisplayModeStatus().queries);
        std::puts("NATIVE_SWAP_REGISTRY_TRANSACTION: isolated root IB, 2048 command/96 root bit controls, pinning, rejection, fail-stop and RPTR-last PASS; synthetic CPU only");
        std::fflush(nullptr);std::_Exit(0);
    }
    int swapStatus=0;assert(waitpid(swapChild,&swapStatus,0)==swapChild && WIFEXITED(swapStatus) && WEXITSTATUS(swapStatus)==0);
    assert(!registry.nativeSwapCommandStatus().issued && read(newWb)==40 &&
        std::equal(commandBefore.begin(),commandBefore.end(),space.translate(colorChild+0x3000)));
    std::vector<std::uint8_t> retained;
    std::promise<void> imageAttempting,imageFreed;auto imageAttempted=imageAttempting.get_future(),imageFinished=imageFreed.get_future();
    std::thread imageFreer;
    assert(registry.visitNativeOwnedSwapImage(swapCall,[&](const auto& image){
        assert(checkImage(image));retained=image.rgba;
        imageFreer=std::thread([&]{imageAttempting.set_value();dispatch("__imp__MmFreePhysicalMemory",{1,liveCopy});imageFreed.set_value();});
        assert(imageAttempted.wait_for(std::chrono::seconds(1))==std::future_status::ready);
        assert(imageFinished.wait_for(std::chrono::milliseconds(25))==std::future_status::timeout);return true;
    }));
    imageFreer.join();assert(imageFinished.wait_for(std::chrono::seconds(1))==std::future_status::ready);
    rejectImage(swapCall);
    const auto reused=allocate(0x400000u,4u);assert(reused==liveCopy);
    std::memcpy(space.translate(reused),space.translate(unrelated),0x400000);
    rejectImage(swapCall); // Identical address, size, protection AND bytes, different lifetime.
    assert(retained.size()==NativeSwapImagePlan::PixelBytes && retained[3]==255);
    assert(std::equal(commandBefore.begin(),commandBefore.end(),space.translate(colorChild+0x3000)) &&
        read(newWb)==40 && read(*registry.graphicsSystemIdentifierAddress())==savedId &&
        registry.completedNativeOwnedColorBatch()->readWord==40);
    assert(registry.dispatch("__imp__VdSwap",swapCall).state==GuestImportDispatchState::Rejected);
    std::printf("NATIVE_OWNED_SWAP_IMAGE: %u rejections, active-byte/owner continuity, lock-held callback, same-address reuse rejected; no swap return PASS\n",imageRejections);
    registry.reset(); assert(!registry.completedExperimentalNativeInitialization());
    assert(!registry.visitNativeOwnedSwapImage(swapCall,checkImage));
    assert(!registry.inspectStoppedNativeSwapRequest(swapCall));
    assert(!registry.nativeSystemContext() && !registry.bindNativeNoSystemOverlay());
    assert(!registry.completedNativeOwnedColorBatch() && !registry.bindExperimentalOwnedColorBatch());
    assert(!registry.experimentalIdleCachePairs() && !registry.bindExperimentalIdleCachePair());
    assert(!registry.completedWarmEmptyCacheCoherency());
    assert(!registry.completedWarmRingCoherency());
    assert(!registry.completedWarmStateBatches());
    assert(!registry.retiredGraphicsRingReadWord());
    assert(!registry.completedNativeStateBatches() && !registry.submitNativeStateBatch(40));
    assert(!registry.completedEmptyGuestCacheCoherency() && !registry.submitEmptyGuestCacheCoherency(34));
    assert(!registry.dispatch(Status,status).handled() && registry.nativeEdramTrainingState().statusQueries == 0);
    assert(registry.nativeEdramTrainingState().workerChecks == 0 && registry.nativeEdramTrainingState().noCommandQueries == 0);
    assert(!registry.submitExperimentalNativeInitialization(25));
}
