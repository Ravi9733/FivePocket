// Read-only native filtering measurement. Authored texture; no game execution.
#include "mip_sample_probe.h"
#include "process_limits.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
using namespace recomp::gpu;
int main(int argc,char** argv) {
    assert(argc==2 && initializeProbeProcess());std::setvbuf(stdout,nullptr,_IONBF,0);
    std::ifstream f(argv[1],std::ios::binary|std::ios::ate);assert(f);auto length=f.tellg();assert(length>0 && length%4==0 && length<65536);
    f.seekg(0);std::vector<unsigned> code(length/4);assert(f.read(reinterpret_cast<char*>(code.data()),length));
    const auto d=XenosTextureDescription::decode({0x80404802,0x01000020,0x2001,0xA80D10,0,0x200});assert(d);
    const auto plan=planXenosTexture2D(*d);assert(plan && plan->linearBytes==32);
    std::vector<std::uint8_t> linear(32);
    for(unsigned y=0;y<2;++y)for(unsigned x=0;x<2;++x) {
        const std::array<float,4> pixel{float(x),float(y),float(x*y),1};
        for(unsigned c=0;c<4;++c) {
            const _Float16 value=pixel[c];unsigned short bits;std::memcpy(&bits,&value,2);
            const auto at=(y*2+x)*8+c*2;linear[at]=bits&255;linear[at+1]=bits>>8;
        }
    }
    const auto tiled=storeXenosTexture2D(*d,linear,std::vector<std::uint8_t>(plan->sourceBytes,0x5A));assert(tiled);
    ProbeDevice device;assert(device.inspect() && device.create({}));
    VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(device.physical,&props);
    auto image=NativeTextureImage::create(device,*d,*tiled);assert(image && image->supportsLinearFiltering());
    std::vector<std::array<float,4>> queries;
    for(unsigned axis=0;axis<2;++axis)for(unsigned i=0;i<=4096;++i) {
        const float v=.25f+float(i)/8192;queries.push_back(axis?std::array<float,4>{.625f,v,0,0}:std::array<float,4>{v,.625f,0,0});
    }
    NativeTextureSamplerPlan sampler{{2,2},1,1,0,0,0};
    const auto output=sampleMipProbe(device,*image,sampler,code,queries);assert(output && output->size()==queries.size());
    unsigned candidates=0;
    for(unsigned bits=4;bits<=16;++bits)for(unsigned mode=0;mode<3;++mode) {
        const float scale=float(1u<<bits);double worst=0;
        const auto quant=[&](float v) {const float s=v*scale;
            return (mode==0?std::floor(s+.5f):mode==1?std::nearbyint(s):std::floor(s))/scale;};
        for(unsigned i=0;i<queries.size();++i) {
            const float x=quant(queries[i][0]*2-.5f),y=quant(queries[i][1]*2-.5f);
            const std::array<float,4> expected{x,y,x*y,1};
            for(unsigned c=0;c<4;++c) {
                assert(std::isfinite((*output)[i][c]));
                worst=std::max(worst,std::abs(double((*output)[i][c])-expected[c]));
            }
        }
        if(worst==0){++candidates;std::printf("NATIVE_LINEAR_WEIGHT_MODEL bits=%u rounding=%s exact=1\n",bits,mode==0?"half-up":mode==1?"nearest-even":"floor");}
    }
    assert(image->readback()==std::optional{linear});
    std::printf("NATIVE_LINEAR_PRECISION_MEASUREMENT_PASS queries=%zu reported_subtexel_bits=%u exact_models=%u authored_rgba16f=1 xbox_conformance=0\n",queries.size(),props.limits.subTexelPrecisionBits,candidates);
    assert(candidates>0);
}
