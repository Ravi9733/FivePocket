#include "native_tile_cache_barrier.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace recomp::gpu;
int main() {
    constexpr std::array<std::uint8_t,24> bytes{0,0,5,0xC8,0,2,0,0,0,0,0xE,0,0,0,0,1,0,0,0xE,0x40,0,0,0,1};
    const auto fresh=[&](bool pair=true){auto p=NativeTileCacheBarrier::parse(std::span(bytes).first(pair?24:8));assert(p);return *p;};
    for(unsigned n=0;n<24;++n)if(n!=8)assert(!NativeTileCacheBarrier::parse(std::span(bytes).first(n)));
    auto extra=std::vector<std::uint8_t>(bytes.begin(),bytes.end());extra.push_back(0);assert(!NativeTileCacheBarrier::parse(extra));
    for(unsigned i=0;i<24;++i)for(unsigned bit=0;bit<8;++bit) {
        auto bad=bytes;bad[i]^=1u<<bit;assert(!NativeTileCacheBarrier::parse(bad));
        if(i<8)assert(!NativeTileCacheBarrier::parse(std::span(bad).first(8)));
    }
    NativeTileCacheBarrier::Journal journal{{0x21F9,{8,UINT32_MAX}},{0x5C8,{1,1}}};
    NativeGraphicsState state;state.invalidationEpoch=11;state.experimentalIdleCachePairEpoch=7;state.binSelect=3;
    // Connected control: preceding producer changes source bytes; stale derived
    // data is evicted only AFTER ordering, and the following consumer rebuilds.
    std::vector<unsigned> source{10,20},derived=source;source[0]=99;unsigned calls=0;
    auto pair=fresh();assert(pair.cachePair());
    assert(pair.execute(state,journal,[&]{assert(derived[0]==10);++calls;return true;},
        [&]{assert(calls==1 && state.experimentalIdleCachePairEpoch==7);derived.clear();++calls;return true;}));
    if(derived.empty())derived=source;
    assert(derived[0]==99 && calls==2 && state.experimentalIdleCachePairEpoch==8 && state.invalidationEpoch==11 && state.binSelect==3);
    assert(journal.at(0x21F9).first==8 && journal.at(0x5C8).first==0x20000 && journal.at(0xE00).first==1 && journal.at(0xE40).second==UINT32_MAX);
    const auto stable=journal;
    assert(!pair.execute(state,journal,[]{assert(false);return true;},[]{assert(false);return true;}) && journal==stable);
    auto wait=fresh(false);assert(!wait.cachePair());
    assert(wait.execute(state,journal,[&]{++calls;return true;},[]{assert(false);return true;}));
    assert(calls==3 && state.experimentalIdleCachePairEpoch==8 && journal==stable);
    for(unsigned mode=0;mode<5;++mode) {
        auto op=fresh();NativeGraphicsState s;s.experimentalIdleCachePairEpoch=mode==4?UINT64_MAX:7;
        auto j=stable;const auto old=s.experimentalIdleCachePairEpoch;unsigned fences=0,evictions=0;
        NativeTileCacheBarrier::Action fence=[&]{++fences;return mode!=2;},evict=[&]{++evictions;return mode!=3;};
        if(mode==0)fence={};if(mode==1)evict={};
        assert(!op.execute(s,j,fence,evict) && s.experimentalIdleCachePairEpoch==old && j==stable);
        assert(fences==unsigned(mode==2 || mode==3) && evictions==unsigned(mode==3));
    }
    for(unsigned site=0;site<2;++site)for(unsigned mode=0;mode<4;++mode) {
        auto op=fresh();NativeGraphicsState s;auto j=stable;bool caught=false;unsigned evictions=0;
        const auto disrupt=[&]()->bool {
            if(mode==0)assert(!op.execute(s,j,[]{return true;},[]{return true;}));
            if(mode==1)++s.experimentalIdleCachePairEpoch;
            if(mode==2)j[0x21F9].first=25;
            if(mode==3)throw std::runtime_error("ordering");
            return true;
        };
        try {assert(!op.execute(s,j,[&]{return site==0?disrupt():true;},[&]{++evictions;return disrupt();}));}
        catch(const std::runtime_error&){caught=true;}
        assert(caught==(mode==3) && evictions==site && op.phase()==NativeTileCacheBarrier::Phase::Failed);
        assert(s.experimentalIdleCachePairEpoch==unsigned(mode==1));
    }
    NativeGraphicsState saturated;saturated.experimentalIdleCachePairEpoch=UINT64_MAX;auto j=stable;
    assert(fresh(false).execute(saturated,j,[]{return true;},{}) && saturated.experimentalIdleCachePairEpoch==UINT64_MAX);
    std::puts("NATIVE_TILE_CACHE_BARRIER_PASS exact_packets=1 producer_consumer=1 fence_before_eviction=1 no_failed_receipt=1 reentry_and_stale_state_rejected=1 wait_only_preserves_epoch=1 gpu_execution=0");
    constexpr std::array<std::uint8_t,44> hostBytes{0,0,0xA,0x31,0,1,0,0,0,1,0xA,0x2F,0,0,0,0,0,0,0x10,0,
        0xC0,4,0x3C,0,0,0,0,3,0,0,0xA,0x31,0,0,0,0,0x80,0,0,0,0,0,0,8};
    constexpr std::array<std::uint8_t,8> flushBytes{0xC0,0,0x46,0,0,0,0,6};
    for(auto raw:{std::span<const std::uint8_t>(hostBytes),std::span<const std::uint8_t>(flushBytes)}) {
        assert(NativeTileCacheBarrier::parse(raw));
        for(unsigned n=0;n<raw.size();++n)assert(!NativeTileCacheBarrier::parse(raw.first(n)));
        for(unsigned i=0;i<raw.size();++i)for(unsigned bit=0;bit<8;++bit) {
            auto bad=std::vector<std::uint8_t>(raw.begin(),raw.end());bad[i]^=1u<<bit;
            assert(!NativeTileCacheBarrier::parse(bad));
        }
    }
    for(unsigned mode=0;mode<7;++mode) {
        auto op=NativeTileCacheBarrier::parse(hostBytes);assert(op && op->hostCoherency());
        NativeGraphicsState s;auto regs=stable;unsigned fences=0,evictions=0;bool caught=false;
        const auto fence=[&]()->bool {
            ++fences;assert(regs.at(0xA31).first==0x80010000 && regs.at(0xA2F).first==0 && regs.at(0xA30).first==0x1000);
            if(mode==4)throw std::runtime_error("coherency fence");
            return mode!=1;
        };
        const auto evict=[&]()->bool {
            ++evictions;assert(fences==1 && regs.at(0xA31).first==0x80010000);
            if(mode==3)assert(!op->execute(s,regs,[]{return true;},[]{return true;}));
            if(mode==5)regs[0xA30].first=0x2000;
            if(mode==6)throw std::runtime_error("coherency evict");
            return mode!=2;
        };
        try{assert(op->execute(s,regs,fence,evict)==(mode==0));}catch(const std::runtime_error&){caught=true;}
        assert(caught==(mode==4 || mode==6) && fences==1 && evictions==unsigned(mode!=1 && mode!=4));
        assert(regs.at(0xA31).first==(mode?0x80010000u:0u) && !s.experimentalIdleCachePairEpoch && !s.invalidationEpoch);
        assert(regs.at(0x21F9)==stable.at(0x21F9));
    }
    for(bool success:{false,true}) {
        auto op=NativeTileCacheBarrier::parse(flushBytes);assert(op && op->cacheFlush());
        NativeGraphicsState s;auto regs=stable;unsigned fences=0;
        assert(op->execute(s,regs,[&]{++fences;return success;},[]{assert(false);return true;})==success);
        assert(fences==1 && regs.at(0x21F9).first==(success?6u:8u) && !s.experimentalIdleCachePairEpoch && !s.invalidationEpoch);
    }
    std::puts("NATIVE_TILE_COHERENCY_EVENT_PASS busy_before_fence=1 clear_after_eviction=1 failed_request_stays_dirty=1 stale_and_reentry_rejected=1 flush_orders_without_invalidation=1 gpu_execution=0");
}
