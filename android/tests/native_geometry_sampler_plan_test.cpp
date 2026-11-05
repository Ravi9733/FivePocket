#include "native_geometry_sampler_plan.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    unsigned cases=0;
    const std::array<std::uint32_t,6> base{0x81000002u,0x10000014,127|(127u<<13),0xD10u,2u<<6,0x20000A10};
    for(unsigned aniso=0;aniso<=5;++aniso)for(unsigned walk=0;walk<4;++walk)
    for(unsigned filter=0;filter<8;++filter) {
        auto fetch=base;fetch[3]|=((filter&1)<<19)|(((filter>>1)&1)<<21)|(((filter>>2)&1)<<23)|(aniso<<25);
        fetch[4]|=walk<<10;const auto original=fetch;
        const auto p=planNativeGeometrySampler2D(fetch);assert(fetch==original);
        if(!aniso && walk){assert(!p);continue;}
        assert(p && p->anisotropy==aniso && p->walkFlags==walk && p->maxAnisotropy==(aniso?1u<<(aniso-1):1u));
        assert(p->sampling.magFilter==(aniso?1u:filter&1));
        assert(p->sampling.minFilter==(aniso?1u:(filter>>1)&1));
        assert(p->sampling.mipFilter==(aniso?1u:(filter>>2)&1));
        assert(p->sampling.maxLevel==2 && p->sampling.triClamp==2);
        assert(bool(planNativeTextureSampler2D(fetch))==(!aniso && !walk));
        ++cases;
    }
    for(unsigned word=3;word<=5;++word) {
        const unsigned mask=word==3?0xF0000000u:word==4?0xFFFFF000u:0x1E4u;
        for(unsigned bit=0;bit<32;++bit)if(mask&(1u<<bit)) {
            auto bad=base;bad[word]|=1u<<bit;assert(!planNativeGeometrySampler2D(bad));
        }
    }
    for(unsigned value:{6u,7u}){auto bad=base;bad[3]|=value<<25;assert(!planNativeGeometrySampler2D(bad));}
    for(unsigned shift:{19u,21u,23u})for(unsigned value:{2u,3u}) {
        auto bad=base;bad[3]|=value<<shift;assert(!planNativeGeometrySampler2D(bad));
    }
    auto bad=base;bad[4]=3u<<6;assert(!planNativeGeometrySampler2D(bad)); // Packed tail stays rejected.
    bad=base;bad[0]|=3u<<10;assert(!planNativeGeometrySampler2D(bad));
    std::printf("GEOMETRY_SAMPLER_PLAN_PASS cases=%u original_fetch_preserved=1 legacy_gate_preserved=1\n",cases);
}
