#include "native_color_state_commit.h"
#include "native_color_output_publication.h"
#include "native_color_batch_fixture.h"
#include "../../tools/vulkan_probe/native_color_scratch_memory.h"
#include <cstdio>
using namespace recomp::gpu;
int main(){
    test::NativeColorBatchFixture fixture(0x800000,0x2000000,0x7000000,0x7000004);
    std::vector<std::uint8_t> raw;
    for(auto word:fixture.words)for(unsigned b=0;b<4;++b)raw.push_back(word>>(24-b*8));
    Pm4Snapshot child;Pm4Error error;assert(decodePm4(raw.data(),raw.size(),{},child,error));
    NativeColorMemoryControls controls{{0x1000,4096},{0x3000,static_cast<unsigned>(raw.size())},{0x2000,4},{0x2004,4},
        34,37,{{0xC0013F00,0x3000,static_cast<unsigned>(fixture.words.size())}}};
    auto capture=NativeColorBatchCapture::create(child,{},*fixture.nativeProgram,controls,
        [&](auto address,auto size)->std::optional<std::vector<std::uint8_t>>{
            for(unsigned i=0;i<fixture.raw.size();++i)
                if(address==fixture.addresses[i] && size==fixture.raw[i].size())return fixture.raw[i];
            return std::nullopt;});
    assert(capture);auto scratch=NativeColorScratchMemory::create(*capture);assert(scratch);
    const auto& memory=scratch->memory();
    // CPU unit data only. The modeled timeline/count below is deliberately
    // synthetic and NOT evidence of a GPU fence or executable fixture module.
    using P=NativeColorChainProfile<true,true,true>;using E=NativeColorEdramSnapshot;
    std::vector<std::uint8_t> half(P::OutputBytes,0x5A),edram(P::BackingBytes,0x5A),tiled(P::CopyBytes,0x5A);
    std::fill_n(half.begin()+16,1280*720*8,0);std::fill_n(edram.begin()+16+E::Begin,E::Bytes,0);
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x){unsigned at=0;assert(colorCopyOffset(1280,720,x,y,at));
        std::fill_n(tiled.begin()+16+at,4,0);}
    NativeColorChainExecutionResult result;
    result.edram=E::capture(half.data(),half.size(),edram.data(),edram.size(),tiled.data(),tiled.size());
    result.copy=NativeTiledCopyPublication::capture(capture->copy().copy,tiled.data(),tiled.size());assert(result.edram && result.copy);
    result.auditedTimeline.emplace();for(unsigned stage=0;stage<4;++stage)
        assert(result.auditedTimeline->begin(stage) && result.auditedTimeline->finishAuditedStage(stage));
    result.nativeFences=4;result.eventBytes.fill(0x5A);
    for(const auto& w:capture->events().staging.words)if(w.byteOffset==16 || w.byteOffset==20)
        std::memcpy(result.eventBytes.data()+w.byteOffset,w.bigEndianBytes.data(),4);
    NativeRegisterJournal live{{0x3000,{0xAB00,0xFF00}},{0xA31,{0,0xFFFFFFFF}}},prepared;
    const auto original=live;
    NativeGraphicsState state,candidate;std::optional<E> retained;
    assert(prepareNativeColorState(*capture,live,state,prepared,candidate));
    const auto preparedOriginal=prepared;
    struct Publication {const NativeColorBatchCapture& capture;const NativeColorResolvedMemory& memory;
        NativeRegisterJournal& live;const NativeRegisterJournal& original;NativeGraphicsState& state;
        std::optional<E>& retained;unsigned calls=0;bool accept=false;};
    Publication scope{*capture,memory,live,original,state,retained};
    NativeColorPublisher publisher{&scope,[](void* context,const NativeColorChainExecutionResult& output) noexcept{
        auto& s=*static_cast<Publication*>(context);++s.calls;
        assert(s.live==s.original && !s.state.batches && !s.retained); // No early state commit.
        return s.accept && publishCapturedNativeColorOutputs(s.capture,s.memory,*output.copy,output.eventBytes.data(),output.eventBytes.size());
    }};
    const auto snapshot=[&](){std::array<std::vector<std::uint8_t>,4> bytes;
        for(unsigned i=0;i<4;++i)bytes[i].assign(memory.outputs[i].bytes(),memory.outputs[i].bytes()+memory.outputs[i].range.bytes);return bytes;};
    const auto before=snapshot();unsigned rejects=0;
    const auto run=[&](NativeColorPublisher p){return publishAndCommitNativeColorState(*capture,result,prepared,candidate,p,live,state,retained);};
    const auto reject=[&](NativeColorPublisher p){assert(!run(p));++rejects;
        assert(snapshot()==before && live==original && !state.batches && !retained && result.edram && prepared==preparedOriginal);};
    reject({});reject({&scope,nullptr});reject({nullptr,publisher.publish});assert(!scope.calls);
    reject(publisher);assert(scope.calls==1);scope.accept=true;
    result.nativeFences=3;reject(publisher);result.nativeFences=4;
    result.leadingSetupFences=1;reject(publisher);result.leadingSetupFences=0;
    auto completedTimeline=result.auditedTimeline;result.auditedTimeline.reset();reject(publisher);
    result.auditedTimeline.emplace();reject(publisher);result.auditedTimeline=completedTimeline;
    auto savedCopy=std::move(result.copy);result.copy.reset();reject(publisher);result.copy=std::move(savedCopy);
    for(unsigned field=0;field<7;++field){auto bad=candidate;
        switch(field){case 0:++bad.binMask;break;case 1:++bad.binSelect;break;case 2:++bad.knownBinWords;break;
        case 3:++bad.invalidationEpoch;break;case 4:++bad.batches;break;case 5:++bad.experimentalIdleCachePairEpoch;break;
        case 6:++bad.experimentalColorBatches;break;}
        assert(!publishAndCommitNativeColorState(*capture,result,prepared,bad,publisher,live,state,retained));++rejects;
        assert(snapshot()==before && live==original && !state.batches && !retained && prepared==preparedOriginal);
    }
    prepared[0x3000].first^=1;assert(!run(publisher));prepared=preparedOriginal;++rejects;
    assert(!publishAndCommitNativeColorState(*capture,result,live,candidate,publisher,live,state,retained));++rejects;
    assert(!publishAndCommitNativeColorState(*capture,result,prepared,state,publisher,live,state,retained));++rejects;
    assert(!publishAndCommitNativeColorState(*capture,result,prepared,candidate,publisher,live,state,result.edram));++rejects;
    assert(scope.calls==1);
    result.eventBytes[23]^=1;reject(publisher);result.eventBytes[23]^=1;
    memory.inputs[0].bytes()[0]^=1;reject(publisher);memory.inputs[0].bytes()[0]^=1;
    assert(scope.calls==3 && run(publisher) && scope.calls==4);
    assert(live==preparedOriginal && prepared==original && state.batches==1 && state.experimentalColorBatches==1 && state.invalidationEpoch==3);
    assert(retained && !result.edram && memory.outputs[3].bytes()[3]==37);
    for(unsigned at=E::Begin;at<E::Begin+E::Bytes;at+=4){unsigned word=99;assert(retained->readWord(at,word) && word==0);}
    for(unsigned index:{0x5C8u,0xA2Fu,0xA30u,0xA31u,0x21F9u})assert(!live.count(index));
    const auto after=snapshot();assert(!run(publisher) && snapshot()==after && scope.calls==4);++rejects;
    std::printf("NATIVE_COLOR_STATE_COMMIT: %u failure-atomic controls, publication before nonthrowing state/EDRAM commit, CPU scratch only PASS\n",rejects);
}
