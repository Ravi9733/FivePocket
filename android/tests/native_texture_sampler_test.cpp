#include "native_texture_sampler.h"
#include <cassert>

int main() {
    using namespace recomp::gpu;
    unsigned accepted=0;
    for(unsigned u=0;u<3;++u)for(unsigned v=0;v<3;++v)
    for(unsigned mag=0;mag<2;++mag)for(unsigned min=0;min<2;++min)for(unsigned mip=0;mip<2;++mip) {
        const std::array<std::uint32_t,6> f{0x81000002u|(u<<10)|(v<<13),0x10000014,
            127|(127u<<13),0xD10u|(mag<<19)|(min<<21)|(mip<<23),2u<<6,0x20000A10};
        const auto p=planNativeTextureSampler2D(f);assert(p);
        assert(p->clamp[0]==u && p->clamp[1]==v && p->magFilter==mag &&
            p->minFilter==min && p->mipFilter==mip && p->maxLevel==2 && p->triClamp==2);
        for(unsigned word:{3u,4u,5u}) {
            auto bad=f;bad[word]|=word==3?1u<<25:word==4?1u<<12:1u<<5;
            assert(!planNativeTextureSampler2D(bad));
        }
        auto bad=f;bad[0]=(bad[0]&~(7u<<10))|(3u<<10);assert(!planNativeTextureSampler2D(bad));
        bad=f;bad[3]=(bad[3]&~(3u<<23))|(2u<<23);assert(!planNativeTextureSampler2D(bad));
        bad=f;bad[4]=3u<<6;assert(!planNativeTextureSampler2D(bad)); // Real packed tail.
        ++accepted;
    }
    assert(accepted==72);
    for(unsigned filter=0;filter<2;++filter)for(unsigned gamma=0;gamma<2;++gamma) {
        const std::array<std::uint32_t,6> f{0x81024802u|(gamma?0xFCu:0),0x10000086,
            127|(127u<<13)|(5u<<26),0xC14u|(filter<<19)|(filter<<21)|(1u<<23),3,0x610};
        const auto p=planNativeTextureSamplerCubeBase(f);assert(p && p->maxLevel==0 && p->minFilter==filter);
        assert(!planNativeTextureSampler2D(f));
        for(auto change:std::array<std::array<unsigned,2>,10>{{{0,1u<<10},{0,1u<<16},{1,1024},{2,1},
                {3,1u<<21},{3,1u<<25},{4,1u<<2},{4,1u<<12},{5,1u<<5},{5,1u<<11}}}) {
            auto bad=f;bad[change[0]]^=change[1];assert(!planNativeTextureSamplerCubeBase(bad));
        }
    }
}
