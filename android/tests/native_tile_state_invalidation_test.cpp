#include "native_tile_state_invalidation.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace recomp::gpu;
int main() {
    constexpr std::array<std::uint8_t,8> bytes{0xC0,0,0x3B,0,0,0,1,0};unsigned checks=0;
    const auto fresh=[&]{auto p=NativeTileStateInvalidation::parse(bytes);assert(p);return *p;};
    for(unsigned n=0;n<8;++n){assert(!NativeTileStateInvalidation::parse(std::span(bytes).first(n)));++checks;}
    auto longer=std::vector<std::uint8_t>(bytes.begin(),bytes.end());longer.push_back(0);
    assert(!NativeTileStateInvalidation::parse(longer));++checks;
    for(unsigned i=0;i<8;++i)for(unsigned bit=0;bit<8;++bit) {
        auto bad=bytes;bad[i]^=1u<<bit;assert(!NativeTileStateInvalidation::parse(bad));++checks;
    }
    NativeGraphicsState s;s.binMask=3;s.binSelect=0xC;s.knownBinWords=7;s.batches=11;
    s.experimentalColorBatches=13;s.experimentalIdleCachePairEpoch=17;s.invalidationEpoch=19;
    const std::array<unsigned,3> rawRegisters{0x1234,0x5678,0x9ABC},rawProgram{1,2,3};
    const std::vector<unsigned char> image{0xAA,0x55};std::vector<unsigned> derived{8,9};
    auto op=fresh();unsigned phase=0;
    assert(op.execute(s,[&]{assert(phase==0 && derived.size()==2);phase=1;return true;},
        [&]{assert(phase==1 && s.invalidationEpoch==19);derived.clear();phase=2;return true;}));
    assert(phase==2 && derived.empty() && s.invalidationEpoch==20 && s.binMask==3 && s.binSelect==0xC &&
        s.knownBinWords==7 && s.batches==11 && s.experimentalColorBatches==13 && s.experimentalIdleCachePairEpoch==17);
    assert(rawRegisters[0]==0x1234 && rawProgram[2]==3 && image[0]==0xAA);++checks;
    assert(!op.execute(s,[]{assert(false);return true;},[]{assert(false);return true;}) && s.invalidationEpoch==20);++checks;
    for(unsigned mode=0;mode<5;++mode) {
        auto test=fresh();NativeGraphicsState state;state.invalidationEpoch=mode==4?UINT64_MAX:7;
        unsigned fences=0,evictions=0;const auto before=state.invalidationEpoch;
        NativeTileStateInvalidation::Action fence=[&]{++fences;return mode!=2;},evict=[&]{++evictions;return mode!=3;};
        if(mode==0)fence={};if(mode==1)evict={};
        assert(!test.execute(state,fence,evict) && state.invalidationEpoch==before);
        assert(fences==unsigned(mode==2 || mode==3) && evictions==unsigned(mode==3));++checks;
    }
    for(unsigned site=0;site<2;++site) {
        auto test=fresh();NativeGraphicsState state;unsigned evictions=0;
        const auto reenter=[&]{assert(!test.execute(state,[]{return true;},[]{return true;}));return true;};
        assert(!test.execute(state,[&]{return site==0?reenter():true;},[&]{++evictions;return reenter();}));
        assert(!state.invalidationEpoch && evictions==site && test.phase()==NativeTileStateInvalidation::Phase::Failed);++checks;
    }
    for(unsigned site=0;site<2;++site) {
        auto test=fresh();NativeGraphicsState state;bool caught=false;
        try{test.execute(state,[&]{if(site==0)throw std::runtime_error("fence");return true;},
            [&]()->bool{throw std::runtime_error("evict");});}catch(const std::runtime_error&){caught=true;}
        assert(caught && !state.invalidationEpoch && test.phase()==NativeTileStateInvalidation::Phase::Failed);++checks;
    }
    auto stale=fresh();NativeGraphicsState changed;
    assert(!stale.execute(changed,[&]{++changed.invalidationEpoch;return true;},[]{assert(false);return true;}));
    assert(changed.invalidationEpoch==1);++checks;
    std::printf("NATIVE_TILE_STATE_INVALIDATION_PASS checks=%u canonical_mask=1 fence_before_eviction=1 epoch_after_eviction=1 failures_latched=1 no_other_state_effects=1 gpu_execution=0\n",checks);
}
