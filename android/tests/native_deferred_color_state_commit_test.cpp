#include "native_deferred_color_state_commit.h"
#include "native_deferred_color_result.h"
#include "native_deferred_color_scratch_fixture.h"
#include <cstdio>
using namespace recomp::gpu;
using E=NativeColorEdramSnapshot;
using P=NativeColorChainProfile<true,true,true>;
// CPU synthetic pixels and modeled counters ONLY, never actual GPU evidence.
static NativeDeferredColorExecutionResult synthetic(const NativeDeferredColorCapture& capture,bool red,unsigned destinationOffset=0){
    std::vector<std::uint8_t> half(P::OutputBytes,0x5A),edram(P::BackingBytes,0x5A),tiled(P::CopyBytes,0x5A);
    const std::array<std::uint32_t,4> color=red?std::array<std::uint32_t,4>{0x3F800000,0,0,0x3F800000}:
        std::array<std::uint32_t,4>{0,0,0,0};
    unsigned packed=0;assert(packColor7e3(color,packed));const auto copied=copyColorWord(packed,true);
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x){
        const auto pixel=y*1280+x;unsigned at=0;assert(colorCopyOffset(1280,720,x,y,at));
        for(unsigned c=0;c<4;++c){const unsigned bits=red && (c==0 || c==3)?0x3C00:0;
            half[16+pixel*8+c*2]=bits&255;half[17+pixel*8+c*2]=bits>>8;
            edram[16+E::Begin+pixel*4+c]=packed>>(c*8);tiled[16+at+c]=copied>>(c*8);}
    }
    NativeDeferredColorExecutionResult out;out.submission=capture.submission();
    out.edram=E::capture(half.data(),half.size(),edram.data(),edram.size(),tiled.data(),tiled.size());
    auto copyPlan=capture.copy().copy;copyPlan.destinationBase+=destinationOffset;
    out.copy=NativeTiledCopyPublication::capture(copyPlan,tiled.data(),tiled.size());assert(out.edram && out.copy);
    out.nativeFences=4;out.leadingSetupFences=1;out.auditedPrefixTimeline.emplace();
    assert(out.auditedPrefixTimeline->finishAuditedLeadingFence());
    for(unsigned i=0;i<4;++i)assert(out.auditedPrefixTimeline->begin(i) && out.auditedPrefixTimeline->finishAuditedStage(i));
    return out;
}
static auto stateWords(const NativeGraphicsState& s){
    return std::array<std::uint64_t,7>{s.binMask,s.binSelect,s.knownBinWords,s.invalidationEpoch,
        s.experimentalIdleCachePairEpoch,s.batches,s.experimentalColorBatches};
}
int main(){
    static_assert(!std::is_convertible_v<NativeDeferredColorDiagnosticResult,NativeDeferredColorExecutionResult>);
    static_assert(!std::is_copy_constructible_v<NativeDeferredColorExecutionResult>);
    test::NativeDeferredColorScratchFixture f;
    auto previous=synthetic(*f.capture,false),completed=synthetic(*f.capture,true);
    auto retained=std::move(previous.edram);const auto oldBytes=retained->knownBytes(),newBytes=completed.edram->knownBytes();
    const auto* oldOwner=retained->knownBytes().data();assert(oldBytes!=newBytes);
    NativeRegisterJournal live{{0x3000,{0xAB00,0xFF00}},{0xA31,{0,0xFFFFFFFF}},{0xF01,{0x200E,0xFFFFFFFF}}};
    NativeGraphicsState state;state.batches=19;state.invalidationEpoch=73;state.experimentalIdleCachePairEpoch=91;state.experimentalColorBatches=2;
    completed.pendingRegisters.emplace();completed.pendingState.emplace();
    assert(prepareNativeDeferredColorState(*f.capture,live,state,*completed.pendingRegisters,*completed.pendingState));
    const auto original=live,prepared=*completed.pendingRegisters;const auto oldState=state,newState=*completed.pendingState;
    struct Scope {test::NativeDeferredColorScratchFixture& fixture;NativeRegisterJournal& live;const NativeRegisterJournal& original;
        NativeGraphicsState& state;const NativeGraphicsState& old;std::optional<E>& retained;const std::uint8_t* oldOwner;
        unsigned calls=0;bool accept=false;};
    Scope scope{f,live,original,state,oldState,retained,oldOwner};
    NativeDeferredColorPublisher publisher{&scope,[](void* context,const NativeDeferredColorExecutionResult& out) noexcept{
        auto& s=*static_cast<Scope*>(context);++s.calls;
        assert(s.live==s.original && stateWords(s.state)==stateWords(s.old) && s.retained->knownBytes().data()==s.oldOwner);
        return s.accept && s.fixture.guard->publishCopy(*s.fixture.capture,s.fixture.memory,*out.copy);
    }};
    const auto before=f.storage;unsigned refusals=0;
    const auto run=[&](const auto& p){return publishAndCommitNativeDeferredColorState(*f.capture,completed,p,live,state,retained);};
    const auto unchanged=[&]{assert(f.storage==before && live==original && stateWords(state)==stateWords(oldState) &&
        retained->knownBytes()==oldBytes && retained->knownBytes().data()==oldOwner && completed.edram->knownBytes()==newBytes &&
        !completed.committed && !f.guard->published());};
    const auto reject=[&](const auto& p){assert(!run(p));unchanged();++refusals;};
    reject(NativeDeferredColorPublisher{});reject(NativeDeferredColorPublisher{&scope,nullptr});
    reject(NativeDeferredColorPublisher{nullptr,publisher.publishCopy});assert(!scope.calls);
    reject(publisher);assert(scope.calls==1);scope.accept=true;
    for(unsigned value:{0u,3u,5u,UINT32_MAX}){completed.nativeFences=value;reject(publisher);}completed.nativeFences=4;
    for(unsigned value:{0u,2u,UINT32_MAX}){completed.leadingSetupFences=value;reject(publisher);}completed.leadingSetupFences=1;
    auto timeline=completed.auditedPrefixTimeline;
    completed.auditedPrefixTimeline.reset();reject(publisher);completed.auditedPrefixTimeline.emplace();reject(publisher);
    assert(completed.auditedPrefixTimeline->finishAuditedLeadingFence());reject(publisher);completed.auditedPrefixTimeline=timeline;
    auto submission=std::move(completed.submission);completed.submission.reset();reject(publisher);completed.submission=std::move(submission);
    auto otherBytes=f.storage.at(f.plan->controls().child.address);
    otherBytes[16+f.capture->submission().swapOffsetWords()*4-1]^=1;
    const auto other=NativeDeferredSwapSubmission::parse(f.capture->submission().swap().command(),f.plan->controls().child.address,
        otherBytes.data()+16,f.plan->controls().child.bytes);assert(other);
    completed.submission=*other;reject(publisher);completed.submission=f.capture->submission();
    auto copy=std::move(completed.copy);completed.copy.reset();reject(publisher);completed.copy=std::move(copy);
    auto wrong=synthetic(*f.capture,true,0x400000);copy=std::move(completed.copy);
    completed.copy=std::move(wrong.copy);reject(publisher);completed.copy=std::move(copy);
    auto registers=std::move(completed.pendingRegisters);completed.pendingRegisters.reset();reject(publisher);completed.pendingRegisters=std::move(registers);
    auto pending=completed.pendingState;completed.pendingState.reset();reject(publisher);completed.pendingState=pending;
    for(unsigned field=0;field<7;++field){auto bad=newState;
        if(field==0)++bad.binMask;if(field==1)++bad.binSelect;if(field==2)++bad.knownBinWords;
        if(field==3)++bad.invalidationEpoch;if(field==4)++bad.experimentalIdleCachePairEpoch;
        if(field==5)++bad.batches;if(field==6)++bad.experimentalColorBatches;
        completed.pendingState=bad;reject(publisher);
    }completed.pendingState=pending;
    completed.pendingRegisters->at(0x3000).first^=1;reject(publisher);*completed.pendingRegisters=prepared;
    assert(scope.calls==1);
    auto old=std::move(retained);retained.reset();
    assert(!run(publisher) && scope.calls==1 && f.storage==before && live==original && stateWords(state)==stateWords(oldState));
    retained=std::move(old);++refusals;unchanged();
    auto movedOld=std::move(*retained);
    assert(!run(publisher) && scope.calls==1 && f.storage==before && live==original && stateWords(state)==stateWords(oldState));
    *retained=std::move(movedOld);++refusals;unchanged();
    auto newEdram=std::move(completed.edram);completed.edram.reset();
    assert(!run(publisher) && scope.calls==1 && f.storage==before && live==original && retained->knownBytes()==oldBytes);
    completed.edram=std::move(newEdram);++refusals;unchanged();
    assert(!publishAndCommitNativeDeferredColorState(*f.capture,completed,publisher,*completed.pendingRegisters,state,retained));++refusals;
    assert(!publishAndCommitNativeDeferredColorState(*f.capture,completed,publisher,live,*completed.pendingState,retained));++refusals;
    assert(!publishAndCommitNativeDeferredColorState(*f.capture,completed,publisher,live,state,completed.edram));++refusals;unchanged();
    for(auto* byte:{f.memory.inputs[0].bytes(),f.memory.writes[0].bytes(),f.memory.writes[5].bytes(),f.memory.protectedRanges[4].bytes()}){
        *byte^=1;const auto changed=f.storage;const auto calls=scope.calls;
        assert(!run(publisher) && scope.calls==calls+1 && f.storage==changed && live==original &&
            stateWords(state)==stateWords(oldState) && retained->knownBytes()==oldBytes && !completed.committed);
        *byte^=1;++refusals;unchanged();
    }
    assert(run(publisher) && completed.committed && f.guard->published());
    assert(live==prepared && *completed.pendingRegisters==original && stateWords(state)==stateWords(newState) &&
        retained->knownBytes()==newBytes && completed.edram->knownBytes()==oldBytes && completed.edram->knownBytes().data()==oldOwner);
    auto expected=before;const auto& image=f.memory.writes[0];
    assert(completed.copy->publishToOwnedMemory(image.range.address,expected.at(image.range.address).data()+16,image.range.bytes));
    assert(f.storage==expected && state.experimentalColorBatches==3 && state.invalidationEpoch==75 && state.batches==20 &&
        state.experimentalIdleCachePairEpoch==91 && f.memory.protectedRanges[2].bytes()[3]==76);
    for(unsigned address:{0u,E::Begin-4,E::Begin+E::Bytes,10485756u}){unsigned sentinel=99;
        assert(!retained->readWord(address,sentinel) && sentinel==99);}
    const auto after=f.storage;const auto calls=scope.calls;
    assert(!run(publisher) && scope.calls==calls && f.storage==after && retained->knownBytes()==newBytes);
    std::printf("DEFERRED_COLOR_STATE_COMMIT: %u failure-atomic controls; copy before no-throw register/EDRAM commit; old storage retained; replay refused; tail/IRQ/RPTR unchanged; CPU scratch only PASS\n",refusals);
}
