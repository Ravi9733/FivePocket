#include "xenos_texture_2d.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    unsigned cases=0;
    for(unsigned endian=0;endian<4;++endian)for(bool tiled:{false,true})
    for(unsigned width:{1u,7u,32u,33u,127u,640u}) {
        const unsigned height=width==640?360:35,pitch=(width+63)&~31u;
        const auto d=XenosTextureDescription::decode({2u|((pitch/32)<<22)|(tiled?0x80000000u:0),
            0x10000u|32u|(endian<<6),(width-1)|((height-1)<<13),0x688,0,0x200});
        assert(d);const auto p=planXenosTexture2D(*d);assert(p && p->bytesPerTexel==8);
        std::vector<std::uint8_t> linear(p->linearBytes),before(p->sourceBytes,0xA5);
        for(unsigned i=0;i<linear.size();++i)linear[i]=(i*37+i/8*11+i/256)%128;
        const auto stored=storeXenosTexture2D(*d,linear,before);assert(stored);
        const auto decoded=linearizeXenosTexture2D(*d,*stored);assert(decoded);
        if(*decoded!=linear) {
            std::fprintf(stderr,"TEXTURE64_ROUNDTRIP_FAILURE endian=%u tiled=%u width=%u height=%u\n",endian,unsigned(tiled),width,height);
            return 1;
        }
        assert(std::count_if(stored->begin(),stored->end(),[](auto b){return b!=0xA5;})==linear.size());
        assert(before==std::vector<std::uint8_t>(p->sourceBytes,0xA5));++cases;
    }
    std::printf("TEXTURE64_STORAGE_IMPORT_PASS cases=%u all_endians=1 padding_preserved=1 gpu=0\n",cases);
}
