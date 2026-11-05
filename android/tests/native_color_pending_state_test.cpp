#include "native_color_pending_state.h"
#include "native_color_batch_fixture.h"
#include <cstdio>
using namespace recomp::gpu;

static bool same(const NativeGraphicsState& a,const NativeGraphicsState& b){
    return a.binMask==b.binMask && a.binSelect==b.binSelect && a.knownBinWords==b.knownBinWords &&
        a.invalidationEpoch==b.invalidationEpoch && a.batches==b.batches &&
        a.experimentalIdleCachePairEpoch==b.experimentalIdleCachePairEpoch && a.experimentalColorBatches==b.experimentalColorBatches;
}
int main(){
    test::NativeColorBatchFixture fixture(0x800000,0x2000000,0x7000000,0x7000004);
    // Initial words differ from the first draw's bins. Verify the retained
    // initial identity, not merely the final/draw predicate, gates preparation.
    fixture.words.insert(fixture.words.begin(),{0xC0006000,7,0xC0006300,11});
    std::vector<std::uint8_t> raw;
    for(auto w:fixture.words)for(unsigned b=0;b<4;++b)raw.push_back(w>>(24-b*8));
    Pm4Snapshot child;Pm4Error error;assert(decodePm4(raw.data(),raw.size(),{},child,error));
    NativeColorMemoryControls controls{{0x1000,4096},{0x3000,static_cast<unsigned>(raw.size())},
        {0x2000,4},{0x2004,4},34,37,{{0xC0013F00,0x3000,static_cast<unsigned>(fixture.words.size())}}};
    DrawBinState bins{{{0x12345678,0x9ABCDEF0,0x11223344,0x55667788}},15};
    auto capture=NativeColorBatchCapture::create(child,bins,*fixture.nativeProgram,controls,
        [&](auto address,auto size)->std::optional<std::vector<std::uint8_t>>{
            for(unsigned i=0;i<fixture.raw.size();++i)
                if(address==fixture.addresses[i] && size==fixture.raw[i].size())return fixture.raw[i];
            return std::nullopt;
        });
    assert(capture && capture->initialBins().words==bins.words);
    NativeGraphicsState prior{0x9ABCDEF012345678ULL,0x5566778811223344ULL,73,91,15,19,0};
    const auto original=prior;
    NativeRegisterJournal journal{{0x3000,{0xAB00,0xFF00}},{0xA31,{0,0xFFFFFFFF}}};
    const auto oldJournal=journal;
    NativeGraphicsState pending;NativeRegisterJournal pendingJournal;
    assert(prepareNativeColorState(*capture,journal,prior,pendingJournal,pending));
    assert(same(prior,original) && journal==oldJournal && pending.binMask==0x9ABCDEF000000007ULL &&
        pending.binSelect==0xB11223344ULL && pending.knownBinWords==15 && pending.invalidationEpoch==76 &&
        pending.batches==20 && pending.experimentalColorBatches==1 && pending.experimentalIdleCachePairEpoch==91);
    assert(pendingJournal.at(0x3000)==journal.at(0x3000));
    for(unsigned index:{0x5C8u,0xA2Fu,0xA30u,0xA31u,0x21F9u})assert(!pendingJournal.count(index));
    unsigned rejects=0;
    const auto reject=[&](const auto& bad,const auto& badJournal){
        auto output=pending;auto outputJournal=pendingJournal;
        assert(!prepareNativeColorState(*capture,badJournal,bad,outputJournal,output));
        assert(same(output,pending) && outputJournal==pendingJournal);++rejects;
    };
    for(unsigned bit=0;bit<64;++bit){
        auto bad=prior;bad.binMask^=std::uint64_t{1}<<bit;reject(bad,journal);
        bad=prior;bad.binSelect^=std::uint64_t{1}<<bit;reject(bad,journal);
    }
    for(unsigned bit=0;bit<32;++bit){auto bad=prior;bad.knownBinWords^=1u<<bit;reject(bad,journal);}
    for(unsigned n=0;n<3;++n){auto bad=prior;bad.invalidationEpoch=UINT64_MAX-n;reject(bad,journal);}
    auto bad=prior;bad.batches=UINT32_MAX;reject(bad,journal);
    bad=prior;bad.experimentalColorBatches=UINT32_MAX;reject(bad,journal);
    auto badJournal=journal;badJournal[0x3000]={1,0};reject(prior,badJournal);
    // Exact arithmetic boundary succeeds, without overflow or allocating after
    // output transfer. Prior/output aliasing is supported and remains atomic.
    bad=prior;bad.invalidationEpoch=UINT64_MAX-3;bad.batches=UINT32_MAX-1;bad.experimentalColorBatches=UINT32_MAX-1;
    NativeGraphicsState edge;NativeRegisterJournal edgeJournal;
    assert(prepareNativeColorState(*capture,journal,bad,edgeJournal,edge));
    assert(edge.invalidationEpoch==UINT64_MAX && edge.batches==UINT32_MAX && edge.experimentalColorBatches==UINT32_MAX);
    assert(prepareNativeColorState(*capture,journal,prior,journal,prior));
    assert(same(prior,pending) && journal==pendingJournal);
    auto moved=std::move(*capture);
    reject(original,oldJournal);
    assert(prepareNativeColorState(moved,oldJournal,original,journal,prior));
    std::printf("NATIVE_COLOR_PENDING_STATE: %u atomic rejections, initial-bin identity, counter edges, no live commit PASS\n",rejects);
}
