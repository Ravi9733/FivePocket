#include "native_deferred_color_pending_state.h"
#include "native_color_batch_fixture.h"
#include "native_post_output_swap_fixture.h"
#include <array>
#include <cstdio>
using namespace recomp::gpu;
static auto stateWords(const NativeGraphicsState& s){
    return std::array<std::uint64_t,7>{s.binMask,s.binSelect,s.knownBinWords,s.invalidationEpoch,
        s.experimentalIdleCachePairEpoch,s.batches,s.experimentalColorBatches};
}
int main(){
    test::NativeColorBatchFixture fixture(0x800000,0x2000000,0x7000000,0x7000004,true,true);
    auto words=fixture.words;words.resize(words.size()-14);
    words.insert(words.begin(),{0x2010,0x87654313,0xC0006000,7,0xC0006300,11});
    words.insert(words.end(),{0xC0022F00,0xC00000,0x7FE,2,0x2007,0,0x2318,0,0x231B,0});
    const std::vector<std::uint8_t> constants{1,2,3,4,0xAB,0xCD,0xEF,0};
    const DrawBinState bins{{{0x12345678,0x9ABCDEF0,0x11223344,0x55667788}},15};
    const auto create=[&](){
        const auto command=NativeSwapCommand::make({3,0x3000+unsigned(words.size())*4+40,0x2000000,73});assert(command);
        std::vector<std::uint8_t> bytes;
        for(auto w:words)for(unsigned i=0;i<4;++i)bytes.push_back(w>>(24-i*8));
        const auto tail=test::postOutputSwapBytes(*command);bytes.insert(bytes.end(),tail.begin(),tail.end());
        const auto submitted=NativeDeferredSwapSubmission::parse(*command,0x3000,bytes.data(),bytes.size());assert(submitted);
        return NativeDeferredColorCapture::create(*submitted,bins,*fixture.nativeProgram,
            [&](auto address,auto size)->std::optional<std::vector<std::uint8_t>>{
                if(address==0xC00000 && size==constants.size())return constants;
                for(unsigned i=0;i<8;++i)if(address==fixture.addresses[i] && size==fixture.raw[i].size())return fixture.raw[i];
                return std::nullopt;
            });
    };
    auto capture=create();assert(capture && capture->initialBins().words==bins.words && capture->initialBins().knownWords==15);
    const NativeGraphicsState prior{0x9ABCDEF012345678ULL,0x5566778811223344ULL,73,91,15,19,2};
    const NativeRegisterJournal journal{{0x2010,{0x87654321,0xFFFFFFFF}},{0x3000,{0xAB00,0xFF00}},
        {0x5C8,{0x20000,0xFFFFFFFF}},{0xF01,{0x200E,0xFFFFFFFF}},{0xA31,{0,0xFFFFFFFF}},
        {0xA2F,{0x1000,0xFFFFFFFF}},{0xA30,{0x2000,0xFFFFFFFF}},{0x21F9,{3,0xFFFFFFFF}},{0xE00,{1,0xFFFFFFFF}}};
    NativeRegisterJournal pending;NativeGraphicsState state;
    assert(prepareNativeDeferredColorState(*capture,journal,prior,pending,state));
    assert(state.binMask==0x9ABCDEF000000007ULL && state.binSelect==0xB11223344ULL && state.knownBinWords==15 &&
        state.invalidationEpoch==75 && state.experimentalIdleCachePairEpoch==91 && state.batches==20 && state.experimentalColorBatches==3);
    assert(pending.at(0x2010)==std::make_pair(0x87654313u,0xFFFFFFFFu));
    assert(pending.at(0x3000)==journal.at(0x3000) && pending.at(0xE00)==journal.at(0xE00));
    assert(pending.at(0x47FE)==std::make_pair(0x01020304u,0xFFFFFFFFu));
    assert(pending.at(0x47FF)==std::make_pair(0xABCDEF00u,0xFFFFFFFFu));
    for(unsigned index:{0x2007u,0x2318u,0x231Bu})assert(pending.at(index)==std::make_pair(0u,0xFFFFFFFFu));
    for(unsigned index:{0x5C8u,0xF01u,0xA2Fu,0xA30u,0xA31u,0x21F9u})assert(!pending.count(index));
    unsigned rejected=0,cases=0;
    const auto reject=[&](const auto& bad,const auto& badJournal){
        auto result=state;auto registers=pending;
        assert(!prepareNativeDeferredColorState(*capture,badJournal,bad,registers,result));
        assert(stateWords(result)==stateWords(state) && registers==pending);++rejected;
    };
    for(unsigned bit=0;bit<64;++bit){
        auto bad=prior;bad.binMask^=std::uint64_t{1}<<bit;reject(bad,journal);
        bad=prior;bad.binSelect^=std::uint64_t{1}<<bit;reject(bad,journal);
    }
    for(unsigned bit=0;bit<32;++bit){auto bad=prior;bad.knownBinWords^=1u<<bit;reject(bad,journal);}
    for(unsigned n=0;n<2;++n){auto bad=prior;bad.invalidationEpoch=UINT64_MAX-n;reject(bad,journal);}
    for(unsigned count:{0u,1u,UINT32_MAX}){auto bad=prior;bad.batches=count;reject(bad,journal);}
    for(unsigned count:{0u,1u,3u,UINT32_MAX}){auto bad=prior;bad.experimentalColorBatches=count;reject(bad,journal);}
    for(unsigned bit=0;bit<32;++bit){auto bad=journal;bad[0x3000]={1u<<bit,~(1u<<bit)};reject(prior,bad);}
    for(unsigned index:{0x8000u,UINT32_MAX}){auto bad=journal;bad[index]={0,0};reject(prior,bad);}
    for(unsigned known=0;known<256;++known)for(unsigned value=0;value<256;++value)if(!(value&~known)){
        auto source=journal;source[0x2010]={value,known};source[0x3000]={value,known};
        NativeRegisterJournal result;NativeGraphicsState candidate;
        assert(prepareNativeDeferredColorState(*capture,source,prior,result,candidate));
        assert(result.at(0x2010)==std::make_pair(0x87654313u,0xFFFFFFFFu) && result.at(0x3000)==std::make_pair(value,known));
        assert(stateWords(candidate)==stateWords(state));++cases;
    }
    auto edge=prior;edge.invalidationEpoch=UINT64_MAX-2;edge.batches=UINT32_MAX-1;edge.experimentalIdleCachePairEpoch=UINT64_MAX;
    auto aliasJournal=journal;const auto originalEdge=edge;
    assert(prepareNativeDeferredColorState(*capture,aliasJournal,edge,aliasJournal,edge));
    assert(edge.invalidationEpoch==UINT64_MAX && edge.batches==UINT32_MAX && edge.experimentalColorBatches==3 &&
        edge.experimentalIdleCachePairEpoch==originalEdge.experimentalIdleCachePairEpoch && aliasJournal==pending);
    auto moved=std::move(*capture);reject(prior,journal);
    assert(prepareNativeDeferredColorState(moved,journal,prior,aliasJournal,edge) && aliasJournal==pending && stateWords(edge)==stateWords(state));
    std::printf("DEFERRED_COLOR_PENDING_STATE: %u atomic refusals,%u prior-knowledge cases; two invalidations, six unknown registers; no live commit PASS\n",rejected,cases);
}
