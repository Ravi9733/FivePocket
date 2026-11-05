#include "vulkan_engine.h"
#include "../../android/tests/native_post_output_swap_fixture.h"
#include "../../android/tests/native_display_gamma_fixture.h"
#include "../../android/tests/native_display_output_fixture.h"
#include "../../android/tests/native_color_batch_fixture.h"
#include "native_deferred_color_capture.h"
#include "native_deferred_color_result.h"
#include "process_limits.h"
#include "xenos_native_initialization.h"
#include "xenos_native_state_batch.h"
#include "native_display_information.h"
#include "../../android/tests/native_identity_scaler_fixture.h"
#include "../../android/tests/native_scaler_wrapper_fixture.h"
#include "../../android/tests/xenos_maintenance_point_fixture.h"
#include <cstdio>

int main() {
    if (!recomp::gpu::initializeProbeProcess()) return 2;
    recomp::gpu::VulkanEngine engine;
    using namespace recomp::gpu;
    // Authored packet/table fixture for entry rejection ONLY. This engine has
    // not requested logical geometry, so neither uninitialized nor ready state
    // may read guest owners, fence a scene, or publish its culling records.
    std::vector<std::uint8_t> extentCommands,extentTable(24);
    for(unsigned w:{0x80000000u,0xC0006000u,UINT32_MAX,0xC0032201u,0u,0x30004u,0x5000u,
            0x40000003u,0xC0006000u,0x80000000u,0xC0015A01u,0x1Au,0x2000009u})
        for(unsigned b=0;b<4;++b)extentCommands.push_back(w>>(24-b*8));
    const auto extentWord=[&](unsigned at,unsigned value){for(unsigned b=0;b<4;++b)extentTable[at+b]=value>>(24-b*8);};
    extentWord(0,Gta5TileRecordingDiagnostic::originalLink(0xA2000014));
    extentWord(4,0xA1000000);extentWord(20,0xC0000000);
    const std::array<std::array<unsigned,4>,1> extentTiles{{{0,0,1280,720}}};
    auto extentPlan=Gta5TileExtentPublication::prepare({0xA1000000,0x1000000,extentCommands},
        {0xA2000000,0x2000000,extentTable},extentTiles);
    if(!extentPlan)return 1;
    test::NativeColorBatchFixture colorFixture(0x800000,0x2000000,0x7000000,0x7000004,true,true);
    std::vector<std::uint8_t> colorBytes;
    for(auto word:colorFixture.words)for(unsigned b=0;b<4;++b)colorBytes.push_back(word>>(24-b*8));
    Pm4Snapshot colorChild;Pm4Error colorError;
    if(!decodePm4(colorBytes.data(),colorBytes.size(),{},colorChild,colorError))return 1;
    NativeColorMemoryControls colorControls{{0x1000,4096},{0x3000,static_cast<unsigned>(colorBytes.size())},{0x2000,4},{0x2004,4},64,67,
        {{0xC0013F00,0x3000,static_cast<unsigned>(colorFixture.words.size())}}};
    auto colorCapture=NativeColorBatchCapture::create(colorChild,{},*colorFixture.nativeProgram,colorControls,
        [&](auto address,auto size)->std::optional<std::vector<std::uint8_t>>{
            for(unsigned i=0;i<8;++i)if(address==colorFixture.addresses[i] && size==colorFixture.raw[i].size())return colorFixture.raw[i];
            return std::nullopt;},NativeColorBatchPhase::PostOutput);
    if(!colorCapture)return 1;
    auto deferredWords=colorFixture.words;deferredWords.resize(deferredWords.size()-14);
    const auto deferredCommand=NativeSwapCommand::make({3,0x3000+unsigned(deferredWords.size())*4+40,0x2000000,73});
    if(!deferredCommand)return 1;
    std::vector<std::uint8_t> deferredBytes;
    for(auto word:deferredWords)for(unsigned b=0;b<4;++b)deferredBytes.push_back(word>>(24-b*8));
    const auto deferredTail=test::postOutputSwapBytes(*deferredCommand);deferredBytes.insert(deferredBytes.end(),deferredTail.begin(),deferredTail.end());
    const auto deferredFrame=NativeDeferredSwapSubmission::parse(*deferredCommand,0x3000,deferredBytes.data(),deferredBytes.size());
    if(!deferredFrame)return 1;
    const auto deferredCapture=NativeDeferredColorCapture::create(*deferredFrame,{},*colorFixture.nativeProgram,
        [&](auto address,auto size)->std::optional<std::vector<std::uint8_t>>{
            for(unsigned i=0;i<8;++i)if(address==colorFixture.addresses[i] && size==colorFixture.raw[i].size())return colorFixture.raw[i];
            return std::nullopt;});
    if(!deferredCapture)return 1;
    static_assert(!std::is_convertible_v<NativeDeferredColorDiagnosticResult,NativeColorChainExecutionResult>);
    const auto scalerRecord=test::identityScalerRecord();
    const auto scalerCandidate=NativeIdentityScalerCandidate::inspect(test::identityScalerInputs(),scalerRecord.data(),scalerRecord.size());
    if(!scalerCandidate)return 1;
    const auto scalerPlan=NativeIdentityScalerPlan::make(*scalerCandidate);if(!scalerPlan)return 1;
    const auto scalerCommand=NativeScalerCommand::make({1,43,0x64000000},*scalerPlan);if(!scalerCommand)return 1;
    const auto scalerBytes=test::scalerWrapperBytes(*scalerCommand,0x20000,0x82004000,0x208);
    const auto scalerWrapper=NativeScalerWrapper::parse(*scalerCommand,0x10000,0x10114,scalerBytes.data(),scalerBytes.size());
    if(!scalerWrapper)return 1;
    unsigned interruptActions=0;
    const auto rejectInterrupt=[&] {
        const std::array<std::uint8_t,8> binBytes{0xC0,0,0x62,0,0x80,0,0,3};
        const auto binCommand=NativeTileBinCommand::parse(binBytes);if(!binCommand)return false;
        const auto binBefore=engine.stateBatchSnapshot();
        if(engine.executeLogicalBinCommand(*binCommand))return false;
        const auto binAfter=engine.stateBatchSnapshot();
        if(binAfter.binMask!=binBefore.binMask || binAfter.binSelect!=binBefore.binSelect ||
           binAfter.knownBinWords!=binBefore.knownBinWords)return false;
        unsigned extentCalls=0;
        if(engine.publishTileExtentBounds(*extentPlan,[&](auto,auto)->std::optional<std::vector<std::uint8_t>> {
            ++extentCalls;return std::nullopt;
        },[&](auto,const auto&,const auto&){++extentCalls;return true;}) || extentCalls ||
            extentPlan->phase()!=Gta5TileExtentPublication::Phase::Prepared)return false;
        NativeDeferredSwapGpuResult deferredSwap;deferredSwap.nativeFences=99;
        deferredSwap.interruptIdleFences=98;deferredSwap.eventBytes.fill(0xA5);unsigned deferredSwapCalls=0;
        if(engine.executeNativeDeferredSwapWrapper(*deferredFrame,[&]{++deferredSwapCalls;return true;},
            [&](const auto&){++deferredSwapCalls;return true;},deferredSwap) || deferredSwapCalls ||
            deferredSwap.nativeFences!=99 || deferredSwap.interruptIdleFences!=98 ||
            !std::all_of(deferredSwap.eventBytes.begin(),deferredSwap.eventBytes.end(),[](auto b){return b==0xA5;}))return false;
        unsigned deferredPublications=0;
        const NativeDeferredColorPublisher deferredPublisher{&deferredPublications,[](void* p,const auto&) noexcept{
            ++*static_cast<unsigned*>(p);return false;}};
        if(engine.executeExperimentalDeferredColorPrefix(*colorFixture.nativeProgram,*deferredCapture,{}) ||
            engine.executeExperimentalDeferredColorPrefix(*colorFixture.nativeProgram,*deferredCapture,deferredPublisher) || deferredPublications)return false;
        NativeDeferredColorDiagnosticResult deferredResult;deferredResult.nativeFences=99;deferredResult.leadingSetupFences=98;
        if(engine.executeStoppedDeferredColorDiagnostic(*colorFixture.nativeProgram,*deferredCapture,deferredResult) ||
            deferredResult.nativeFences!=99 || deferredResult.leadingSetupFences!=98 || deferredResult.copy || deferredResult.edram ||
            deferredResult.auditedPrefixTimeline || deferredResult.pendingRegisters || deferredResult.pendingState)return false;
        const auto command=NativeSwapCommand::make({2,0x40028,0x100000,67});if(!command)return false;
        const auto bytes=test::postOutputSwapBytes(*command);
        const auto plan=NativePostOutputSwapWrapper::parse(*command,0x40000,bytes.data(),bytes.size());if(!plan)return false;
        NativePostOutputSwapGpuResult swap;swap.nativeFences=99;unsigned swapCalls=0;
        if(engine.executeNativePostOutputSwapWrapper(*plan,[&]{++swapCalls;return true;},[&](const auto&){++swapCalls;return true;},swap) ||
            swapCalls || swap.nativeFences!=99)return false;
        unsigned colorPublications=0;
        NativeColorPublisher colorPublisher{&colorPublications,[](void* p,const auto&) noexcept{++*static_cast<unsigned*>(p);return true;}};
        if(engine.executeExperimentalPostOutputColorBatch(*colorFixture.nativeProgram,*colorCapture,colorPublisher) || colorPublications ||
            engine.executeExperimentalPostOutputColorBatch(*colorFixture.nativeProgram,*colorCapture,{}))return false;
        NativeColorChainExecutionResult colorResult;colorResult.nativeFences=99;colorResult.leadingSetupFences=98;colorResult.eventBytes.fill(0xA5);
        if(engine.executeStoppedPostOutputColorDiagnostic(*colorFixture.nativeProgram,*colorCapture,colorResult) ||
            colorResult.nativeFences!=99 || colorResult.leadingSetupFences!=98 || colorResult.edram || colorResult.copy ||
            !std::all_of(colorResult.eventBytes.begin(),colorResult.eventBytes.end(),[](auto b){return b==0xA5;}))return false;
        const NativeScalerInterruptSequence::Action action=[&](const auto&){++interruptActions;return true;};
        NativeScalerTailResult result;std::vector<std::uint8_t> output{1,2,3};
        NativeDisplayGammaRamp::Packed packed{};
        const auto gammaBytes=test::gammaWrapperBytes(packed);
        const auto gamma=NativeDisplayGammaWrapper::parse(0x10000,gammaBytes.data(),gammaBytes.size(),0x100000);
        NativeDisplayGammaResult gammaResult;gammaResult.nativeFences=99;unsigned gammaCalls=0;
        const auto outputBytes=test::displayOutputBytes();
        const auto outputPlan=NativeDisplayOutputWrapper::parse(0x10000,outputBytes.data(),outputBytes.size());
        NativeDisplayOutputResult outputResult;outputResult.nativeFences=99;
        if(!outputPlan || engine.executeExperimentalNativeDisplayOutput(*outputPlan,[&]{++gammaCalls;return true;},outputResult) ||
            gammaCalls || outputResult.nativeFences!=99)return false;
        if(!gamma || engine.executeNativeDisplayGamma(*gamma,[&]{++gammaCalls;return true;},gammaResult) || gammaCalls || gammaResult.nativeFences!=99)return false;
        if(engine.completeNativePostScalerStorageCoherency({0x7900000,0x700000,0x01000000}))return false;
        if(engine.completeNativePostDisplayStorageCoherency({0x7900000,0x700000,0x01000000}))return false;
        if(engine.completeNativePostOutputStorageCoherency({0x7900000,0x700000,0x01000000}))return false;
        if(engine.completeNativePostOutputSwapStorageCoherency({0x7900000,0x700000,0x01000000}))return false;
        if(engine.completeNativeDeferredSwapStorageCoherency({0x7900000,0x700000,0x01000000}))return false;
        if(engine.completeNativeScalerTail(result) || engine.nativeScalerConfigured() ||
            engine.mapNativeScanout(nullptr,0,1280,720,output) || output!=std::vector<std::uint8_t>({1,2,3}))return false;
        return !engine.beginNativeScalerInterrupt(*scalerWrapper,action) &&
            !engine.acknowledgeNativeScalerInterrupt(action) && !interruptActions;
    };
    if(!rejectInterrupt())return 1;
    if(engine.bindNativeSwapCompletionClock({}) || !engine.bindNativeSwapCompletionClock([]{return std::optional<std::uint64_t>{1000};}) ||
        engine.bindNativeSwapCompletionClock([]{return std::optional<std::uint64_t>{2000};}))return 1;
    const auto swapCommand=NativeSwapCommand::make({1,0x2028,0x100000,37});if(!swapCommand)return 1;
    std::array<std::uint8_t,360> swapBytes{};
    const auto putSwap=[&](unsigned i,unsigned v){for(unsigned b=0;b<4;++b)swapBytes[i*4+b]=v>>(24-b*8);};
    const std::array<unsigned,10> swapPrefix{0x5C8,0x20000,0xE00,1,0xE40,1,0xC0025800,0x80000003,0x8000,0xDEADBEEF};
    const std::array<unsigned,16> swapTail{0xC0025800,0x80000003,0x8004,0xDEADBEEF,0xC0003B00,0x7FFF,
        0xC0001000,0x77777777,0xC0025800,3,0x9002,0x12345678,0xC0025800,3,0xA002,0xABCDEF01};
    for(unsigned i=0;i<swapPrefix.size();++i)putSwap(i,swapPrefix[i]);
    for(unsigned i=0;i<swapTail.size();++i)putSwap(i+74,swapTail[i]);
    std::copy(swapCommand->bytes().begin(),swapCommand->bytes().end(),swapBytes.begin()+40);
    const auto swapPlan=NativeSwapWrapper::parse(*swapCommand,0x2000,swapBytes.data(),swapBytes.size());if(!swapPlan)return 1;
    unsigned swapCallbacks=0;NativeSwapGpuResult swapResult;swapResult.nativeFences=99;
    const auto rejectSwap=[&]{return !engine.executeNativeSwapWrapper(*swapPlan,[&]{++swapCallbacks;return true;},swapResult) &&
        swapResult.nativeFences==99 && !swapCallbacks;};
    if(!rejectSwap())return 1;
    recomp::bootstrap::NativeDisplayConfiguration selectedDisplay{};
    if(engine.selectNativeDisplayMode(0x40000000) || engine.querySelectedDisplayConfiguration(selectedDisplay))return 1;
    std::vector<std::uint32_t> warmRoot(22); warmRoot[0]=0xC0114800; warmRoot[1]=0x3FF;
    for (unsigned i=0;i<8;++i) warmRoot[4+i]=i*0x80;
    warmRoot[12]=0x10800; warmRoot[13]=7; warmRoot[19]=0xC0013F00; warmRoot[20]=0x1000; warmRoot[21]=11;
    const std::vector<std::uint32_t> warmChild{0xA31,0x01000000,0x10A2F,0x1000,0x2000,0xC0043C00,3,0xA31,0,0x80000000,8};
    if (engine.completeWarmRingInitialization(warmRoot,warmChild) || engine.warmRingInitialized()) return 1;
    if (engine.drainForRingReconfiguration()) return 1;
    const std::vector<std::uint32_t> stateWords{0xC0006000,0x12345678,0xC0006100,0x9ABCDEF0,
        0xC0006200,0x11223344,0xC0006300,0x55667788,0x22080,0,0,0x00400080,
        0x5C8,0x20000,0xF01,0x200E,0xC0003B00,0x7FFF,
        0xC0025800,3,0x10002,0x01020304,0xC0025800,3,0x10006,0xAABBCCDD};
    recomp::gpu::NativeInitializationReadback stateOutput; stateOutput.byteCount=99;
    if (engine.completeNativeStateBatch(stateWords,stateOutput) || stateOutput.byteCount!=99) return 1;
    using Training = recomp::bootstrap::GuestGraphicsBackend::EdramTrainingPolicy;
    if (engine.queryEdramTrainingPolicy() != Training::Unavailable) return 1;
    const recomp::bootstrap::NativeInitialCoherency initialCoherency{0, 0x1000, 0x01000000};
    if (engine.completeNativeCommandStorageCoherency(initialCoherency) || engine.completeNativeScalerStorageCoherency(initialCoherency)) return 1;
    if (engine.completeEmptyGuestCacheCoherency(initialCoherency)) return 1;
    if (engine.completeInitialEmptyCacheCoherency(initialCoherency) || engine.initialEmptyCacheCoherencyCompleted()) return 1;
    std::uint32_t gammaType = 0xA5A5A5A5, gammaPower = 0xA5A5A5A5;
    if (engine.queryDisplayGamma(gammaType, gammaPower) || gammaType != 0xA5A5A5A5 || gammaPower != 0xA5A5A5A5) return 1;
    recomp::bootstrap::NativeDisplayConfiguration display{{1280, 720, 1280, 720},
        {0, 0, 1280, 720, 1280, 720, 0, 0, 0, 0, 0, 0, 0, 0}};
    if (engine.configureDisplay(display) || engine.displayConfiguration()) return 1;
    if (engine.ready() || !engine.initialize() || !engine.ready() || engine.initialize()) return 1;
    if(!rejectInterrupt())return 1;
    if(!rejectSwap() || engine.bindNativeSwapCompletionClock([]{return std::optional<std::uint64_t>{1000};}))return 1;
    if (engine.queryDisplayGamma(gammaType, gammaPower)) return 1;
    if (engine.completeInitialEmptyCacheCoherency(initialCoherency)) return 1;
    if (!engine.configureDisplay(display) || !engine.displayConfiguration() ||
        engine.displayConfiguration()->dimensions != display.dimensions) return 1;
    if (!engine.queryDisplayGamma(gammaType, gammaPower) || gammaType != 1 || gammaPower != 0x3F800000) return 1;
    auto invalid = display; invalid.dimensions[2] = 0;
    if (engine.configureDisplay(invalid) || engine.displayConfiguration()->dimensions != display.dimensions) return 1;
    invalid = display; invalid.scaler[7] = 0x7FC00000;
    if (engine.configureDisplay(invalid) || !engine.configureDisplay(display)) return 1;
    for(unsigned bit=0;bit<32;++bit)
        if(engine.selectNativeDisplayMode(0x40000000^(1U<<bit)) || engine.querySelectedDisplayConfiguration(selectedDisplay))return 1;
    if(!engine.selectNativeDisplayMode(0x40000000) || engine.selectNativeDisplayMode(0x40000000) ||
        !engine.querySelectedDisplayConfiguration(selectedDisplay) || selectedDisplay.dimensions!=display.dimensions ||
        selectedDisplay.scaler!=display.scaler)return 1;
    std::array<std::uint8_t,88> displayBytes{};
    if(!recomp::bootstrap::makeNativeDisplayInformation(selectedDisplay,displayBytes) ||
        displayBytes[72]!=5 || displayBytes[73] || displayBytes[74]!=2 || displayBytes[75]!=0xd0 ||
        displayBytes[86]!=5 || displayBytes[87])return 1;
    if(!engine.configureDisplay(display) || engine.querySelectedDisplayConfiguration(selectedDisplay) ||
        !engine.selectNativeDisplayMode(0x40000000) || !engine.querySelectedDisplayConfiguration(selectedDisplay))return 1;
    std::puts("NATIVE_FIXED_DISPLAY_MODE: capability-backed selection/readback,32 flag bits rejected,reconfiguration invalidation PASS; no physical display change");
    if(!rejectInterrupt())return 1;
    std::puts("NATIVE_SCALER_INTERRUPT_ADMISSION: before/after engine init/display selection reject without scratch actions PASS");
    if(!rejectSwap())return 1;
    if (engine.completeNativeCommandStorageCoherency(initialCoherency) || engine.completeNativeScalerStorageCoherency(initialCoherency)) return 1;
    auto invalidCoherency = initialCoherency; invalidCoherency.cacheFlags = 0;
    if (engine.completeInitialEmptyCacheCoherency(invalidCoherency) || engine.initialEmptyCacheCoherencyCompleted()) return 1;
    if (!engine.completeInitialEmptyCacheCoherency(initialCoherency) || !engine.initialEmptyCacheCoherencyCompleted() ||
        engine.completeInitialEmptyCacheCoherency(initialCoherency)) return 1;
    std::puts("NATIVE_INITIAL_COHERENCY_V1 fenced=1 resident_guest_resources=0 range_uploaded=0");
    std::puts("NATIVE_DISPLAY_V1 capability_checked=1 dimensions=1280,720,1280,720 images_created=0 scaler_executed=0");
    std::puts("VULKAN_ENGINE_V1 initialized=1 queue_verified_words=1024 guest_commands=0 draws=0");
    recomp::gpu::StoppedConstantInitialization plan;
    recomp::gpu::ConstantShaderPair pair; pair.position = {0,0,0,1}; pair.parameter = {0,0,0,0};
    for (unsigned i = 0; i < 24; ++i) plan.draws.push_back({0,i*2,pair});
    using Phase = recomp::gpu::NativeWordPhase;
    const std::array<recomp::gpu::NativeWordRequest,3> words{{
        {0,0x11223344,Phase::BeforeGraphics},{4,0x55667788,Phase::AfterGraphics},
        {0,0x99AABBCC,Phase::AfterGraphics}}};
    if (!recomp::gpu::planNativeWordUpdates(words.data(),words.size(),8,plan.memory.updates)) return 1;
    recomp::gpu::NativeInitializationReadback output; output.bytes.fill(0xA5); output.byteCount = 77; output.draws = 88;
    auto broken = plan; broken.memory.updates.words[1].byteOffset = 256;
    if (engine.completeExperimentalConstantInitialization(broken,output) || output.byteCount != 77 ||
        output.draws != 88 || output.bytes[0] != 0xA5) return 1;
    broken = plan; broken.memory.updates.words[1].phase = Phase::BeforeGraphics;
    broken.memory.updates.words[1].byteOffset = 0;
    if (engine.completeExperimentalConstantInitialization(broken,output) || output.byteCount != 77) return 1;
    if (engine.queryEdramTrainingPolicy() != Training::Unavailable) return 1;
    if (engine.completeEmptyGuestCacheCoherency(initialCoherency)) return 1;
    if (!engine.completeExperimentalConstantInitialization(plan,output) || output.byteCount != 8 || output.draws != 24 ||
        engine.completeExperimentalConstantInitialization(plan,output)) return 1;
    const std::array<std::uint8_t,8> expected{0x99,0xAA,0xBB,0xCC,0x55,0x66,0x77,0x88};
    if (!std::equal(expected.begin(),expected.end(),output.bytes.begin())) return 1;
    std::puts("NATIVE_CONSTANT_INITIALIZATION_V1 fenced=1 draws=24 audited_bytes=8 guest_memory_written=0 one_shot=1");
    if (engine.queryEdramTrainingPolicy() != Training::DriverManagedNoXenosLink ||
        engine.queryEdramTrainingPolicy() != Training::DriverManagedNoXenosLink) return 1;
    std::puts("NATIVE_EDRAM_POLICY_V1 driver_managed=1 xenos_link=0 hardware_training=0 edram_rendered=0");
    if (engine.completeEmptyGuestCacheCoherency(invalidCoherency) ||
        !engine.completeEmptyGuestCacheCoherency(initialCoherency) ||
        !engine.completeEmptyGuestCacheCoherency(initialCoherency)) return 1;
    std::puts("NATIVE_EMPTY_CACHE_COHERENCY_V1 batches=2 individually_fenced=1 resident_guest_resources=0 range_uploaded=0");
    if (engine.drainForRingReconfiguration()) return 1;
    auto badState=stateWords; badState[13]=0x20001;
    if (engine.completeNativeStateBatch(badState,stateOutput) || stateOutput.byteCount!=99 || engine.stateBatchSnapshot().batches) return 1;
    if (!engine.completeNativeStateBatch(stateWords,stateOutput) || stateOutput.byteCount!=8 || stateOutput.draws) return 1;
    const std::array<std::uint8_t,8> stateExpected{1,2,3,4,0xAA,0xBB,0xCC,0xDD};
    if (!std::equal(stateExpected.begin(),stateExpected.end(),stateOutput.bytes.begin())) return 1;
    const auto state=engine.stateBatchSnapshot();
    if (state.binMask!=0x9ABCDEF012345678ULL || state.binSelect!=0x5566778811223344ULL ||
        state.knownBinWords!=15 || state.batches!=1 || state.invalidationEpoch!=1 ||
        engine.retainedRegister(0x2082)!=0x00400080 || engine.retainedRegister(0x21F9)!=3) return 1;
    if (!engine.completeNativeStateBatch({0xC0006000,7},stateOutput) || stateOutput.byteCount!=4 ||
        engine.stateBatchSnapshot().binMask!=0x9ABCDEF000000007ULL || engine.stateBatchSnapshot().batches!=2 ||
        engine.stateBatchSnapshot().invalidationEpoch!=1 || engine.retainedRegister(0x2082)!=0x00400080 ||
        !engine.completeEmptyGuestCacheCoherency(initialCoherency)) return 1;
    if (engine.completeNativeStateBatch(badState,stateOutput) || engine.stateBatchSnapshot().batches!=2) return 1;
    std::puts("NATIVE_STATE_BATCH_V1 batches=2 event_bytes_audited=8 state_retained=1 bin_word_merge=1 draws=0");
    if (!engine.drainForRingReconfiguration() || engine.stateBatchSnapshot().batches!=2 ||
        engine.retainedRegister(0x2082)!=0x00400080) return 1;
    std::puts("NATIVE_RING_DRAIN_V1 queue_idle=1 state_preserved=1 guest_memory_written=0");
    if (engine.drainForRingReconfiguration() || engine.warmRingInitialized()) return 1;
    if (engine.completeEmptyGuestCacheCoherency(initialCoherency) ||
        engine.completeNativeStateBatch(stateWords,stateOutput)) return 1;
    auto badWarm=warmRoot; badWarm[1]^=1;
    if (engine.completeWarmRingInitialization(badWarm,warmChild) || engine.warmRingInitialized()) return 1;
    if (!engine.completeWarmRingInitialization(warmRoot,warmChild) || !engine.warmRingInitialized() ||
        engine.completeWarmRingInitialization(warmRoot,warmChild) || engine.drainForRingReconfiguration() ||
        engine.stateBatchSnapshot().invalidationEpoch!=2 || engine.stateBatchSnapshot().binMask!=0x9ABCDEF000000007ULL ||
        engine.retainedRegister(0x2082)!=0x00400080) return 1;
    std::puts("NATIVE_WARM_RING_V1 generation=1 new_fence_waited=1 state_preserved=1 resource_upload=0");
    if(!rejectInterrupt())return 1;
    std::puts("NATIVE_EXTENT_ENGINE_ADMISSION_PASS states=4 logical_feature_missing_rejected=1 owner_callbacks=0 plan_unconsumed=1 positive_scene_connection=0");
    std::puts("NATIVE_BIN_ENGINE_ADMISSION_PASS states=4 logical_feature_missing_rejected=1 bin_state_unchanged=1");
    std::puts("NATIVE_POST_OUTPUT_COLOR_GPU_ADMISSION: pre-init,initialized,display-only,warm-no-output reject; diagnostic output unchanged PASS");
    std::puts("NATIVE_DEFERRED_COLOR_GPU_ADMISSION: pre-init,initialized,display-only,warm-no-output reject; diagnostic output unchanged PASS");
    std::puts("NATIVE_DEFERRED_COLOR_LIVE_ADMISSION: pre-init,initialized,display-only,warm-no-output reject; publisher calls=0 PASS");
    std::puts("NATIVE_POST_OUTPUT_SWAP_GPU_ADMISSION: pre-init,initialized,display-only,warm-no-output reject; callbacks/receipt unchanged PASS");
    std::puts("NATIVE_DEFERRED_SWAP_GPU_ADMISSION: pre-init,initialized,display-only,warm-no-output reject; callbacks/receipt unchanged PASS");
    if (engine.completeNativeCommandStorageCoherency(initialCoherency) || engine.completeNativeScalerStorageCoherency(initialCoherency)) return 1;
    std::puts("NATIVE_SCALER_STORAGE_ADMISSION: pre-init, no-color, warm-no-swap rejected PASS; positive path needs completed owned swap");
    std::puts("NATIVE_COMMAND_STORAGE_ADMISSION: pre-init, no-color, warm-no-color rejected PASS; positive path requires owned-color execution");
    const std::vector<std::uint32_t> setup{0xD02,0x10800,0x30A02,0xC0100000,0x07F00000,0xC0000000,0x00100000,
        0x1DD,0x10000,0x1DC,0x20037,0xD04,0,0x5C8,0x20000,
        0xC0025800,3,0x10002,0x01020304,0xC0025800,3,0x10006,0xAABBCCDD};
    if (!engine.completeNativeStateBatch(setup,stateOutput) || stateOutput.byteCount!=8 ||
        !std::equal(stateExpected.begin(),stateExpected.end(),stateOutput.bytes.begin()) ||
        engine.retainedRegister(0x1DC)!=0x20037 || engine.retainedRegister(0x1DD)!=0x10000 ||
        engine.stateBatchSnapshot().batches!=3) return 1;
    std::puts("NATIVE_WARM_STATE_V1 scratch_config_retained=1 events_audited=2 scratch_writebacks=0 draws=0");
    unsigned scratchBase=0xAAAA,scratchMask=0xBBBB;
    if(!engine.queryNativeScratchWriteback(scratchBase,scratchMask) || scratchBase!=0x10000 || scratchMask!=0x20037)return 1;
    std::puts("NATIVE_SCRATCH_QUERY: fully_known_base_mask retained; no scratch/IRQ writes PASS");
    if (!engine.completeEmptyGuestCacheCoherency(initialCoherency) ||
        !engine.completeEmptyGuestCacheCoherency(initialCoherency) ||
        engine.stateBatchSnapshot().batches!=3 || engine.stateBatchSnapshot().invalidationEpoch!=2 ||
        engine.retainedRegister(0x1DC)!=0x20037 || engine.retainedRegister(0x2082)!=0x00400080) return 1;
    std::puts("NATIVE_WARM_CACHE_V1 fences_waited=2 state_preserved=1 resource_upload=0 frame_presented=0");
    const std::vector<std::uint32_t> idlePair{0x5C8,0x20000,0xE00,1,0xE40,1};
    if(engine.completeNativeStateBatch(idlePair,stateOutput) || engine.stateBatchSnapshot().experimentalIdleCachePairEpoch) return 1;
    for(unsigned i=0;i<6;++i)for(unsigned bit=0;bit<32;++bit){
        auto changed=idlePair;changed[i]^=1U<<bit;stateOutput.byteCount=99;
        if(engine.completeExperimentalIdleCachePair(changed,stateOutput) || stateOutput.byteCount!=99 ||
            engine.stateBatchSnapshot().experimentalIdleCachePairEpoch || engine.stateBatchSnapshot().batches!=3) return 1;
    }
    for(unsigned count=1;count<=2;++count){
        if(!engine.completeExperimentalIdleCachePair(idlePair,stateOutput) || stateOutput.byteCount!=4 || stateOutput.draws ||
            !std::all_of(stateOutput.bytes.begin(),stateOutput.bytes.begin()+4,[](auto b){return b==0x5A;}) ||
            engine.stateBatchSnapshot().experimentalIdleCachePairEpoch!=count || engine.stateBatchSnapshot().batches!=3+count ||
            engine.stateBatchSnapshot().invalidationEpoch!=2 || engine.retainedRegister(0xE00) || engine.retainedRegister(0xE40) ||
            engine.retainedRegister(0x2082)!=0x00400080) return 1;
    }
    if(engine.completeExperimentalIdleCachePair(stateWords,stateOutput) || engine.completeNativeStateBatch(idlePair,stateOutput))return 1;
    std::puts("EXPERIMENTAL_IDLE_CACHE_PAIR_V1 individually_fenced=2 mutation_rejections=192 empty_residency=1 hardware_semantics_verified=0");
    {
        using namespace test::maintenance;
        auto words=batch();words.resize(words.size()-8);append(words,{0xC0003B00,0x100});append(words,event());
        const auto root=snapshot(words);const auto graph=flat(root);
        const auto before=engine.stateBatchSnapshot();const auto oldProgram=engine.retainedMaintenancePrograms();
        unsigned visits=0;
        if(engine.executeExperimentalMaintenancePoints(root,graph,{}))return 1;
        if(engine.executeExperimentalMaintenancePoints(root,graph,[&](const auto& readback){
            ++visits;return readback.draws!=96;}) || visits!=1)return 1;
        if(engine.stateBatchSnapshot().batches!=before.batches || engine.retainedMaintenancePrograms()!=oldProgram)return 1;
        if(engine.executeExperimentalMaintenancePoints({},graph,[&](const auto&){++visits;return true;}) || visits!=1)return 1;
        for(unsigned count=1;count<=2;++count) {
            if(!engine.executeExperimentalMaintenancePoints(root,graph,[&](const auto& readback){
                ++visits;return readback.draws==96 && readback.byteCount==4 && readback.bytes[3]==7;}) || visits!=count+1)return 1;
            const auto after=engine.stateBatchSnapshot();
            if(after.batches!=before.batches+count || after.invalidationEpoch!=before.invalidationEpoch+count ||
               after.experimentalIdleCachePairEpoch!=before.experimentalIdleCachePairEpoch+count ||
               after.binMask!=before.binMask || engine.retainedRegister(0x1DC)!=0x20037 ||
               engine.retainedRegister(0x2180)!=0x1000000E || engine.retainedRegister(0x21F9)!=3 ||
               engine.retainedMaintenancePrograms()[0].empty() || engine.retainedMaintenancePrograms()[1].empty())return 1;
        }
        std::puts("NATIVE_MAINTENANCE_COMMIT_PASS published_batches=2 points=192 publisher_rejection_preserves_state=1 raw_shader_retained=1 fresh_epochs=1");
    }
}
