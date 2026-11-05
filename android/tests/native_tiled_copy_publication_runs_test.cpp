// matchesPublishedOwnedMemoryRuns == matchesPublishedOwnedMemory for a real 1280x720 tiled copy: the published bytes,
// every single-byte change at the run boundaries (both edges of each padding run) and at 20000 random positions,
// random multi-byte changes, and the rejected targets. -DRUNS_MUTANT_RUN_START (an off-by-one at every run start) must fail.
#include "native_tiled_copy_publication.h"
#include <cassert>
#include <cstdio>
#include <random>
using namespace recomp::gpu;
int main() {
    ColorCopyPlan plan;
    assert(planColorCopy({0x14000500,0xC02D0,1280,720,false},0x100000,0x1F2D8000,0x2D00500,0x1000300,plan));
    std::vector<std::uint8_t> readback(plan.capacity+32,0x5A),mask(plan.capacity/4),destination(plan.capacity+32);
    for (unsigned y=0;y<720;++y) for (unsigned x=0;x<1280;++x) {
        std::uint32_t offset=0;assert(colorCopyOffset(1280,720,x,y,offset));mask[offset/4]=1;
        for (unsigned c=0;c<4;++c) readback[16+offset+c]=static_cast<std::uint8_t>((x*7+y*13+c)%256);
    }
    auto captured=NativeTiledCopyPublication::capture(plan,readback.data(),readback.size());assert(captured);
    for (std::size_t i=0;i<destination.size();++i) destination[i]=static_cast<std::uint8_t>(i%251);
    assert(captured->publishToOwnedMemory(plan.destinationBase,destination.data()+16,plan.capacity));
    unsigned long long checks=0,disagree=0,rejected=0;
    const auto check=[&]{
        const bool a=captured->matchesPublishedOwnedMemory(plan.destinationBase,destination.data()+16,plan.capacity);
#if defined(RUNS_MUTANT_RUN_START)
        const bool b=[&]{ // the run algorithm with an off-by-one: the first active word after each padding word is skipped
            std::size_t run=0;
            const auto differs=[&](std::size_t f,std::size_t t){return t>f && std::memcmp(destination.data()+16+f*4,readback.data()+16+f*4,(t-f)*4);};
            for(std::size_t w=0;w<mask.size();++w)if(!mask[w]){if(differs(run,w))return false;run=w+2;}
            return !differs(run,mask.size());}();
#else
        const bool b=captured->matchesPublishedOwnedMemoryRuns(plan.destinationBase,destination.data()+16,plan.capacity);
#endif
        ++checks;disagree+=a!=b;rejected+=!a;
    };
    check(); // the published copy
    std::vector<std::size_t> positions;
    std::size_t paddingRuns=0;
    for (std::size_t w=0;w<mask.size();) { // every run of padding words: the words on both of its edges (and one step in)
        if (mask[w]) {++w;continue;}
        std::size_t e=w;while(e<mask.size() && !mask[e])++e;++paddingRuns;
        for (const long at:{long(w)-2,long(w)-1,long(w),long(w)+1,long(e)-2,long(e)-1,long(e),long(e)+1})
            if (at>=0 && std::size_t(at)<mask.size()) for (unsigned lane:{0u,3u}) positions.push_back(std::size_t(at)*4+lane);
        w=e;
    }
    for (unsigned lane=0;lane<4;++lane){positions.push_back(lane);positions.push_back(plan.capacity-4+lane);}
    std::mt19937_64 rng(232);
    for (unsigned i=0;i<20000;++i) positions.push_back(rng()%plan.capacity);
    for (const auto at:positions){destination[16+at]^=0x40;check();destination[16+at]^=0x40;}
    for (unsigned i=0;i<2000;++i) { // several changes at once, some inside padding only
        std::vector<std::size_t> changed;for(unsigned k=0,n=1+rng()%4;k<n;++k)changed.push_back(rng()%plan.capacity);
        for(auto at:changed)destination[16+at]^=0x11;
        check();
        for(auto it=changed.rbegin();it!=changed.rend();++it)destination[16+*it]^=0x11;
    }
    check();
    assert(!captured->matchesPublishedOwnedMemoryRuns(plan.destinationBase+4,destination.data()+16,plan.capacity));
    assert(!captured->matchesPublishedOwnedMemoryRuns(plan.destinationBase,destination.data()+16,plan.capacity-4));
    assert(!captured->matchesPublishedOwnedMemoryRuns(plan.destinationBase,nullptr,plan.capacity));
    std::printf("publication runs: checks=%llu rejected=%llu disagreements=%llu positions=%zu padding_runs=%zu\n",checks,rejected,disagree,positions.size(),paddingRuns);
    return disagree?1:0;
}
