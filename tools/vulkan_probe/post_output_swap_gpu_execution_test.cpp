#include "native_post_output_swap_gpu_execution.h"
#include "native_post_output_swap_interrupt_sequence.h"
#include "native_post_output_swap_native_actions.h"
#include "native_post_output_swap_fixture.h"
#include "process_limits.h"
#include <cassert>
#include <stdexcept>
#include <string_view>
using namespace recomp::gpu;
#ifndef RECOMP_SURFACE_TICKET
#define RECOMP_SURFACE_TICKET 2
#endif
using Wrapper=NativeSurfaceSwapWrapper<RECOMP_SURFACE_TICKET>;
using Sequence=NativeSurfaceSwapInterruptSequence<RECOMP_SURFACE_TICKET>;
using Actions=NativeSurfaceSwapNativeActions<RECOMP_SURFACE_TICKET>;
int main(int argc,char** argv){
    if(!initializeProbeProcess() || argc>2)return 2;
    const auto mode=argc==2?std::string_view(argv[1]):std::string_view{};
    constexpr std::array modes{
        "--post-accept-clock-fault","--post-accept-clock-throw","--post-accept-backwards-clock","--post-accept-wrap-clock",
        "--interrupt-refusal","--interrupt-throw","--missing-idle","--duplicate-idle","--missing-ack","--late-interrupt-throw"};
    if(!mode.empty() && std::find(modes.begin(),modes.end(),mode)==modes.end())return 2;
    ProbeDevice context;if(!context.inspect() || !context.create({}))return 1;
    const auto command=NativeSwapCommand::make({RECOMP_SURFACE_TICKET,0x2028,0x100000,67});assert(command);
    const auto bytes=test::postOutputSwapBytes(*command);
    const auto plan=Wrapper::parse(*command,0x2000,bytes.data(),bytes.size());assert(plan);
    unsigned samples=0,presents=0,interrupts=0,actions=0;
    NativeSurfaceSwapGpuResult<RECOMP_SURFACE_TICKET> result;result.nativeFences=99;
    const auto untouched=[&]{assert(result.nativeFences==99 && !result.interruptAcknowledged && !result.interruptIdleFences);
        assert(!result.beforeCompletionNs && !result.afterCompletionNs);for(auto byte:result.eventBytes)assert(!byte);};
    NativeCompletionClock clock=[&]()->std::optional<std::uint64_t>{
        if(!samples++)return 8'589'934'590ULL;
        assert(presents==1 && interrupts==0);
        if(mode=="--post-accept-clock-throw")throw std::runtime_error("synthetic clock exception");
        if(mode=="--post-accept-clock-fault")return std::nullopt;
        if(mode=="--post-accept-backwards-clock")return 8'589'934'589ULL;
        if(mode=="--post-accept-wrap-clock")return 8'589'934'590ULL+8'589'934'592ULL;
        return 8'589'934'600ULL;
    };
    Sequence sequence(*plan);
    Actions::Journal journal{{0x579,{0x11223344,0xFFFFFFFF}}};
    NativeGraphicsState state;Actions nativeActions(*plan,journal,state);
    std::array<std::uint32_t,8> scratch{};scratch[1]=0x11223344;
    NativePostOutputSwapInterruptDispatch interrupt=[&](const std::function<bool()>& waitIdle){
        ++interrupts;assert(samples==2 && presents==1);untouched();
        if(mode=="--interrupt-refusal")return false;
        if(mode=="--interrupt-throw")throw std::runtime_error("synthetic IRQ exception");
        if(mode=="--missing-idle")return true;
        const auto action=[&](const Wrapper::Operation& op){
            ++actions;
            using Kind=Wrapper::Kind;
            switch(op.kind){
            case Kind::BinMaskLow:assert(actions==1);break;
            case Kind::WaitIdle:assert(actions==2);break;
            case Kind::ScratchWrite:scratch[op.target]=op.value;break;
            case Kind::WaitScratch:if(scratch[op.target]!=op.value)return false;break;
            case Kind::CallbackAcknowledge:break;
            case Kind::Interrupt:assert(false);return false;
            }
            return nativeActions.apply(op,waitIdle);
        };
        if(!sequence.prefix(plan->scratchPhysical(),0x20037,action))return false;
        assert(actions==8 && scratch[0]==4 && scratch[4]==plan->callbackAddress() && scratch[5]==plan->callbackContext());
        if(mode=="--duplicate-idle"){assert(!waitIdle());return true;} // Ignoring failure still must stop.
        assert(sequence.enterGuest());
        if(mode!="--missing-ack")scratch[0]=0; // EXPLICIT MOCK, no guest callback is executed by this test.
        assert(sequence.guestReturned());
        if(!sequence.acknowledge(action))return false;
        assert(actions==11 && scratch[0]==0 && scratch[1]==0x11223344 && scratch[4]==0x0BADF00D);
        if(mode=="--late-interrupt-throw")throw std::runtime_error("synthetic acknowledged IRQ exception");
        return true;
    };
    if(mode.empty()){
        const auto refusalClock=[]()->std::optional<std::uint64_t>{return std::nullopt;};
        const auto throwClock=[]()->std::optional<std::uint64_t>{throw std::runtime_error("pre-accept clock");};
        const auto earlyPresent=[&]{++presents;return true;};
        assert(!executeNativeSurfaceSwapGpuWrapper(context,*plan,{},earlyPresent,interrupt,result));untouched();
        assert(!executeNativeSurfaceSwapGpuWrapper(context,*plan,clock,{},interrupt,result));untouched();
        assert(!executeNativeSurfaceSwapGpuWrapper(context,*plan,clock,earlyPresent,{},result));untouched();
        assert(!executeNativeSurfaceSwapGpuWrapper(context,*plan,refusalClock,earlyPresent,interrupt,result));untouched();
        assert(!executeNativeSurfaceSwapGpuWrapper(context,*plan,throwClock,earlyPresent,interrupt,result));untouched();
        const auto fixedClock=[]()->std::optional<std::uint64_t>{return 1000;};
        assert(!executeNativeSurfaceSwapGpuWrapper(context,*plan,fixedClock,[]{return false;},interrupt,result));untouched();
        assert(!executeNativeSurfaceSwapGpuWrapper(context,*plan,fixedClock,[]()->bool{throw std::runtime_error("pre-accept present");},interrupt,result));untouched();
        assert(!presents && !samples && !interrupts && !actions);
    }
    // Genuine Vulkan staging writes/fences, but NO window or actual presentation
    // and NO guest execution. Synthetic owner exercises the ordered IRQ seam.
    const auto okay=executeNativeSurfaceSwapGpuWrapper(context,*plan,clock,[&]{
        ++presents;assert(samples==1 && interrupts==0);untouched();return true;
    },interrupt,result);
    if(!mode.empty() || !okay)return 1;
    assert(validNativeSurfaceSwapCompletion(*plan,result) && samples==2 && presents==1 && interrupts==1 && actions==11);
    assert(sequence.phase()==Sequence::Phase::Acknowledged);
    assert(nativeActions.complete() && journal.at(0x579).first==0x11223344 && journal.at(0x57C).first==0x0BADF00D);
    std::printf("%s: six_fences=1 irq_idle_fences=1 guarded_gpu_audit=1 counter_wrap=1 preaccept_refusals=7 ordered_mock_irq_actions=11 actual_presentations=0 guest_execution=0 PASS\n",
        RECOMP_SURFACE_TICKET==2?"NATIVE_POST_OUTPUT_SWAP_GPU_EVENTS":"NATIVE_DEFERRED_SWAP_GPU_EVENTS");
}
