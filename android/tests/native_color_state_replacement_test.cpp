#include "native_color_state_commit.h"
#include "native_color_output_publication.h"
#include "native_color_batch_fixture.h"
#include "../../tools/vulkan_probe/native_color_scratch_memory.h"
#include <cstdio>
using namespace recomp::gpu;
using E=NativeColorEdramSnapshot;
using P=NativeColorChainProfile<true,true,true>;
namespace {
// All CPU synthetic data. Neither fake SPIR-V framing nor modeled stage
// counters constitute an actual GPU completion certificate.
NativeColorChainExecutionResult output(const NativeColorBatchCapture& capture,bool red){
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
    NativeColorChainExecutionResult result;
    result.edram=E::capture(half.data(),half.size(),edram.data(),edram.size(),tiled.data(),tiled.size());
    result.copy=NativeTiledCopyPublication::capture(capture.copy().copy,tiled.data(),tiled.size());assert(result.edram && result.copy);
    result.nativeFences=4;result.leadingSetupFences=1;result.auditedTimeline.emplace();
    for(unsigned i=0;i<4;++i)assert(result.auditedTimeline->begin(i) && result.auditedTimeline->finishAuditedStage(i));
    result.eventBytes.fill(0x5A);for(unsigned i=0;i<2;++i){const auto& w=capture.events().staging.words[i];
        std::memcpy(result.eventBytes.data()+w.byteOffset,w.bigEndianBytes.data(),4);}
    return result;
}
}
int main(){
    test::NativeColorBatchFixture fixture(0x800000,0x2000000,0x7000000,0x7000004,true,true);
    std::vector<std::uint8_t> raw;for(auto word:fixture.words)for(unsigned b=0;b<4;++b)raw.push_back(word>>(24-b*8));
    Pm4Snapshot child;Pm4Error error;assert(decodePm4(raw.data(),raw.size(),{},child,error));
    NativeColorMemoryControls controls{{0x1000,4096},{0x3000,static_cast<unsigned>(raw.size())},{0x2000,4},{0x2004,4},64,67,
        {{0xC0013F00,0x3000,static_cast<unsigned>(fixture.words.size())}}};
    auto capture=NativeColorBatchCapture::create(child,{},*fixture.nativeProgram,controls,
        [&](auto address,auto size)->std::optional<std::vector<std::uint8_t>>{
            for(unsigned i=0;i<8;++i)if(address==fixture.addresses[i] && size==fixture.raw[i].size())return fixture.raw[i];
            return std::nullopt;},NativeColorBatchPhase::PostOutput);
    assert(capture);auto scratch=NativeColorScratchMemory::create(*capture);assert(scratch);const auto& memory=scratch->memory();
    auto previous=output(*capture,false),completed=output(*capture,true);
    auto retained=std::move(previous.edram);const auto oldBytes=retained->knownBytes(),newBytes=completed.edram->knownBytes();
    assert(oldBytes!=newBytes);const auto* oldOwner=retained->knownBytes().data();
    NativeRegisterJournal live{{0x3000,{0xAB00,0xFF00}},{0xA31,{0,0xFFFFFFFF}}},prepared;
    const auto original=live;NativeGraphicsState state,candidate;state.batches=7;state.invalidationEpoch=19;state.experimentalColorBatches=1;
    assert(prepareNativeColorState(*capture,live,state,prepared,candidate));const auto originalPrepared=prepared;
    struct Publication {const NativeColorBatchCapture& capture;const NativeColorResolvedMemory& memory;
        NativeRegisterJournal& live;const NativeRegisterJournal& original;NativeGraphicsState& state;
        std::optional<E>& retained;const std::uint8_t* oldOwner;unsigned calls=0;bool accept=false;};
    Publication scope{*capture,memory,live,original,state,retained,oldOwner};
    NativeColorPublisher publisher{&scope,[](void* context,const NativeColorChainExecutionResult& out) noexcept{
        auto& s=*static_cast<Publication*>(context);++s.calls;
        assert(s.live==s.original && s.state.experimentalColorBatches==1 && s.retained && s.retained->knownBytes().data()==s.oldOwner);
        return s.accept && publishCapturedNativeColorOutputs(s.capture,s.memory,*out.copy,out.eventBytes.data(),out.eventBytes.size());
    }};
    const auto snapshot=[&](){std::array<std::vector<std::uint8_t>,4> result;
        for(unsigned i=0;i<4;++i)result[i].assign(memory.outputs[i].bytes(),memory.outputs[i].bytes()+memory.outputs[i].range.bytes);return result;};
    const auto before=snapshot();unsigned rejections=0;
    const auto reject=[&](NativeColorEdramCommit mode=NativeColorEdramCommit::ReplaceFullKnownRegion){
        assert(!publishAndCommitNativeColorState(*capture,completed,prepared,candidate,publisher,live,state,retained,mode));++rejections;
        assert(snapshot()==before && live==original && prepared==originalPrepared && retained->knownBytes()==oldBytes &&
            retained->knownBytes().data()==oldOwner && completed.edram->knownBytes()==newBytes);
    };
    reject(NativeColorEdramCommit::Initialize);reject(static_cast<NativeColorEdramCommit>(99));assert(!scope.calls);
    for(unsigned count:{0u,256u,UINT32_MAX}){state.experimentalColorBatches=count;reject();}state.experimentalColorBatches=1;
    auto movedOut=std::move(*retained);assert(retained->knownBytes().empty());
    assert(!validNativeColorEdramTransition(*capture,state,retained,NativeColorEdramCommit::ReplaceFullKnownRegion));retained=std::move(movedOut);
    reject();assert(scope.calls==1);scope.accept=true;
    completed.nativeFences=3;reject();completed.nativeFences=4;
    for(unsigned fences:{0u,2u,UINT32_MAX}){completed.leadingSetupFences=fences;reject();}completed.leadingSetupFences=1;
    auto timeline=completed.auditedTimeline;completed.auditedTimeline.emplace();reject();completed.auditedTimeline=timeline;
    ++candidate.experimentalColorBatches;reject();--candidate.experimentalColorBatches;
    completed.eventBytes[23]^=1;reject();completed.eventBytes[23]^=1;
    memory.inputs[0].bytes()[0]^=1;reject();memory.inputs[0].bytes()[0]^=1;
    assert(publishAndCommitNativeColorState(*capture,completed,prepared,candidate,publisher,live,state,retained,NativeColorEdramCommit::ReplaceFullKnownRegion));
    assert(retained->knownBytes()==newBytes && completed.edram->knownBytes()==oldBytes && completed.edram->knownBytes().data()==oldOwner);
    assert(state.experimentalColorBatches==2 && state.batches==8 && state.invalidationEpoch==22 && memory.outputs[3].bytes()[3]==67);
    assert(live==originalPrepared && prepared==original);
    unsigned color=0;assert(packColor7e3({0x3F800000,0,0,0x3F800000},color));
    for(unsigned at=E::Begin;at<E::Begin+E::Bytes;at+=4){unsigned word=0;assert(retained->readWord(at,word) && word==color);}
    for(unsigned at:{0u,E::Begin-4,E::Begin+E::Bytes,10485756u}){unsigned sentinel=0xABCDEF01;
        assert(!retained->readWord(at,sentinel) && sentinel==0xABCDEF01);}
    const auto after=snapshot();const auto calls=scope.calls;
    assert(!publishAndCommitNativeColorState(*capture,completed,prepared,candidate,publisher,live,state,retained,NativeColorEdramCommit::ReplaceFullKnownRegion));
    assert(snapshot()==after && scope.calls==calls && retained->knownBytes()==newBytes);
    std::printf("NATIVE_COLOR_STATE_REPLACEMENT: %u failure-atomic controls,full known-region replacement,old owner preserved,unknown bytes stay unknown,RPTR-last,replay refused,CPU only PASS\n",rejections);
}
