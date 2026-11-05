#include "xenos_texture_2d.h"
#include <cassert>
#include <cstdio>

int main() {
    using namespace recomp::gpu;
    unsigned cases=0;
    for(unsigned format:{2u,6u})for(bool tiled:{false,true})for(unsigned endian=0;endian<4;++endian) {
        const std::array<std::uint32_t,6> fetch{2u|(2u<<22)|(tiled?0x80000000u:0),
            0x10000u|format|(endian<<6),36|(40<<13),0x688,0,0x200};
        const auto desc=XenosTextureDescription::decode(fetch);assert(desc);
        const auto layout=planXenosTexture2D(*desc);assert(layout);
        const auto bpp=layout->bytesPerTexel;
        std::vector<std::uint8_t> original(layout->linearBytes);
        for(unsigned i=0;i<original.size();++i)original[i]=i%61;
        const std::vector<std::uint8_t> padding(layout->sourceBytes,0xA5);
        const auto before=storeXenosTexture2D(*desc,original,padding);assert(before);
        for(const std::array<unsigned,4> r:{std::array<unsigned,4>{5,7,33,39},{36,40,37,41},{0,0,37,41}}) {
            std::vector<std::uint8_t> region((r[2]-r[0])*(r[3]-r[1])*bpp,0xF3);
            const auto stored=storeXenosTexture2DRegion(*desc,r,region,*before);assert(stored);
            auto expected=original;
            for(unsigned y=r[1];y<r[3];++y)for(unsigned x=r[0];x<r[2];++x)for(unsigned c=0;c<bpp;++c)
                expected[(y*37+x)*bpp+c]=0xF3;
            assert(linearizeXenosTexture2D(*desc,*stored)==std::optional{expected});
            assert(stored==storeXenosTexture2D(*desc,expected,padding)); // Outside pixels AND padding.
            unsigned changed=0;for(unsigned i=0;i<stored->size();++i)changed+=(*stored)[i]!=(*before)[i];
            assert(changed==region.size());
            region.pop_back();assert(!storeXenosTexture2DRegion(*desc,r,region,*before));++cases;
        }
        for(const std::array<unsigned,4> r:{std::array<unsigned,4>{0,0,38,41},{0,0,37,42},{1,1,1,2},{3,0,2,1},{0,0,UINT32_MAX,1}})
            assert(!storeXenosTexture2DRegion(*desc,r,{},*before));
        auto shortBefore=*before;shortBefore.pop_back();
        assert(!storeXenosTexture2DRegion(*desc,{0,0,1,1},std::vector<std::uint8_t>(bpp),shortBefore));
    }
    std::printf("XENOS_TEXTURE_REGION_PASS cases=%u formats=2 endian_modes=4 outside_and_padding_preserved=1\n",cases);
}
