#include "native_texture_image.h"
#include "process_limits.h"
#include "mip_sample_probe.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

using namespace recomp::gpu;
namespace {
std::vector<std::uint32_t> sampleCode;
std::optional<std::vector<std::uint8_t>> read(const std::string& path,unsigned limit) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file)return std::nullopt;
    const auto size=file.tellg();if(size<0 || static_cast<std::uint64_t>(size)>limit)return std::nullopt;
    std::vector<std::uint8_t> result(static_cast<std::size_t>(size));file.seekg(0);
    if(size && !file.read(reinterpret_cast<char*>(result.data()),size))return std::nullopt;
    return result;
}

bool sampling(ProbeDevice& device,const NativeTextureImage& image,
        const std::vector<std::vector<std::uint8_t>>& levels) {
    if(sampleCode.empty())return true;
    const auto p=planNativeTextureSampler2D(image.description().fetch);if(!p)return false;
    const auto& d=image.description();std::vector<std::array<float,4>> queries;
    for(unsigned level=0;level<levels.size();++level) {
        const auto width=std::max(d.width>>level,1u),height=std::max(d.height>>level,1u);
        // Full capture-sized textures, bounded lattice for larger inputs.
        for(unsigned y=0;y<height;y+=std::max(height/128,1u))for(unsigned x=0;x<width;x+=std::max(width/128,1u))
            queries.push_back({(x+.5f)/width,(y+.5f)/height,float(level),0});
    }
    const auto centerQueries=queries.size();
    for(float lod:{-2.f,0.25f,0.5f,0.75f,1.25f,1.5f,1.75f,4.f})
    for(float u:{-.25f,0.f,.125f,.5f,.875f,1.f,1.25f})
    for(float v:{-.25f,0.f,.125f,.5f,.875f,1.f,1.25f})queries.push_back({u,v,lod,0});
    const auto result=sampleMipProbe(device,image,*p,sampleCode,queries);if(!result)return false;
    const auto address=[](int at,int size,unsigned mode) {
        if(mode==2)return std::clamp(at,0,size-1);
        const auto period=mode==1?2*size:size;at%=period;if(at<0)at+=period;
        return mode==1 && at>=size?2*size-1-at:at;
    };
    const auto sampleLevel=[&](float u,float v,unsigned level,bool linear,unsigned component) {
        const auto width=std::max(d.width>>level,1u),height=std::max(d.height>>level,1u);
        const auto selected=d.swizzle[component];
        if(selected>=4)return double(selected-4);
        const auto channel=d.format==2?0:selected,bpp=d.format==2?1u:4u;
        const auto value=[&](int x,int y) {
            const auto at=(address(y,int(height),p->clamp[1])*width+address(x,int(width),p->clamp[0]))*bpp+channel;
            return double(levels[level][at])/255.;
        };
        const double x=double(u)*width,y=double(v)*height;
        if(!linear)return value(int(std::floor(x)),int(std::floor(y)));
        const auto left=int(std::floor(x-.5)),top=int(std::floor(y-.5));
        const auto fx=x-.5-left,fy=y-.5-top;
        return (value(left,top)*(1-fx)+value(left+1,top)*fx)*(1-fy)+
            (value(left,top+1)*(1-fx)+value(left+1,top+1)*fx)*fy;
    };
    double maximum=0;unsigned outside=0,centerOutside=0,filteredOutsideBudget=0;
    for(unsigned i=0;i<queries.size();++i) {
        const auto& q=queries[i];const double lod=std::clamp(double(q[2]),0.,double(p->maxLevel));
        const auto low=unsigned(std::floor(p->mipFilter?lod:lod+.5));
        const auto high=std::min(low+1,p->maxLevel);const auto fraction=p->mipFilter?lod-low:0.;
        const auto linear=q[2]<=0?p->magFilter:p->minFilter;
        for(unsigned c=0;c<4;++c) {
            const auto expected=sampleLevel(q[0],q[1],low,linear,c)*(1-fraction)+
                sampleLevel(q[0],q[1],high,linear,c)*fraction;
            const auto actual=(*result)[i][c];
            if(!std::isfinite(actual))return false;
            const auto error=std::abs(actual-expected);maximum=std::max(maximum,error);
            // Explicit engineering budget for native8-bit UNORM filtering,
            // not an Xbox/Vulkan conformance oracle. Exact texel centers stay
            // strict; interpolated results may differ by at most half an8-bit
            // channel step. Keep reporting every smaller difference as well.
            if(i>=centerQueries && error>0.5/255.+0.000002)++filteredOutsideBudget;
            if(error>0.000002) {
                if(i<centerQueries)++centerOutside;
                if(outside<12)std::printf("MIP_SAMPLE_DIFFERENCE i=%u c=%u uv=%.9g,%.9g lod=%.9g cpu=%.9g gpu=%.9g delta_in_bytes=%.9g center=%u\n",
                    i,c,q[0],q[1],q[2],expected,actual,(actual-expected)*255.,unsigned(i<centerQueries));
                ++outside;
            }
        }
    }
    std::printf("MIP_SAMPLE_MEASURE queries=%zu floats=%zu center_queries=%zu max_abs=%.9g outside_2e_6=%u center_outside_2e_6=%u input_canaries_intact=1 explicit_lod=1 xbox_equivalence=0 tri_clamp=%u\n",
        queries.size(),queries.size()*4,centerQueries,maximum,outside,centerOutside,p->triClamp);
    std::printf("MIP_SAMPLE_POLICY center_limit=2e-6 filtered_limit=half_unorm8_step_plus_2e-6 filtered_outside_budget=%u\n",filteredOutsideBudget);
    return centerOutside==0 && filteredOutsideBudget==0;
}

bool exercise(ProbeDevice& device,const std::array<std::uint32_t,6>& words,
        const std::vector<std::uint8_t>& base,const std::vector<std::uint8_t>& mips,bool rejectLegacy) {
    const auto description=XenosTextureDescription::decode(words);
    const auto expected=linearizeXenosUnpackedMipChain(words,base,mips);
    if(!description || !expected)return false;
    const auto plan=planXenosUnpackedMipChain(words);if(!plan)return false;
    if(rejectLegacy && NativeTextureImage::create(device,*description,base))return false;
    const auto originalBase=base,originalMips=mips;
    auto image=NativeTextureImage::createUnpackedMips(device,*description,base,mips);
    if(!image || !image->view() || image->levelCount()!=expected->size() ||
        image->description().fetch!=words || image->upload(base) || image->readback(image->levelCount()))return false;
    const auto compare=[&] {
        for(unsigned i=0;i<expected->size();++i)if(image->readback(i)!=std::optional{(*expected)[i]})return false;
        return true;
    };
    if(!compare() || !image->uploadMipChain(base,mips) || !compare() || !sampling(device,*image,*expected))return false;
    auto shortBase=base;shortBase.resize(plan->baseBytes-1);
    if(image->uploadMipChain(shortBase,mips) || !compare())return false;
    if(plan->mipBytes) {
        auto shortMips=mips;shortMips.resize(plan->mipBytes-1);
        if(image->uploadMipChain(base,shortMips) || !compare())return false;
    }
    if(base!=originalBase || mips!=originalMips)return false;
    for(unsigned i=0;i<plan->levels.size();++i) {
        const auto& level=plan->levels[i];
        std::printf("MIP_LEVEL_PASS level=%u width=%u height=%u pitch=%u source=%u offset=%u decoded=%zu\n",
            i,level.layout.description.width,level.layout.description.height,
            level.layout.description.pitchTexels,level.layout.sourceBytes,level.planeOffset,(*expected)[i].size());
    }
    return true;
}
}

int main(int argc,char** argv) {
    if(argc>3 || !initializeProbeProcess())return 2;
    if(argc==3) {
        const auto code=read(argv[2],65536*4);if(!code || code->empty() || code->size()%4)return 2;
        sampleCode.resize(code->size()/4);std::memcpy(sampleCode.data(),code->data(),code->size());
    }
    ProbeDevice device;if(!device.inspect() || !device.create({}))return 2;
    if(argc>=2 && std::string(argv[1])!="--synthetic") {
        const std::string root=argv[1];
        const auto fetch=read(root+"/fetch.bin",24),base=read(root+"/base.bin",65536),mips=read(root+"/mips.bin",65536);
        if(!fetch || fetch->size()!=24 || !base || !mips)return 2;
        std::array<std::uint32_t,6> words{};
        for(unsigned i=0;i<6;++i)for(unsigned b=0;b<4;++b)words[i]=(words[i]<<8)|(*fetch)[i*4+b];
        if(!exercise(device,words,*base,*mips,true))return 1;
        std::printf("CAPTURED_MIP_IMAGE_PASS byte_exact_upload_readback=1 game_draw=0 explicit_lod_samples_measured=%u\n",unsigned(!sampleCode.empty()));
        return 0;
    }
    unsigned cases=0;
    for(unsigned format:{2u,6u,18u,20u})for(bool tiled:{false,true})for(unsigned endian=0;endian<4;++endian) {
        std::array<std::uint32_t,6> words{(tiled?0x80000000u:0)|(4u<<22)|2,
            0x10000000u|format|(endian<<6),127|(127u<<13),0x688u|(21u<<19),2u<<6,0x20000A00};
        const auto plan=planXenosUnpackedMipChain(words);if(!plan)return 1;
        std::vector<std::uint8_t> base(plan->baseBytes),mips(plan->mipBytes);
        for(unsigned i=0;i<base.size();++i)base[i]=static_cast<std::uint8_t>(i*17+(i>>8));
        for(unsigned i=0;i<mips.size();++i)mips[i]=static_cast<std::uint8_t>(i*31+(i>>9)+37);
        if(!exercise(device,words,base,mips,true))return 1;
        ++cases;
    }
    // Odd R8 dimensions force different4-byte staging padding at each level.
    for(unsigned format:{2u,6u})for(unsigned width:{1u,3u,7u}) {
        std::array<std::uint32_t,6> words{(1u<<22)|2,0x10000000u|format,
            (width-1)|(4u<<13),0x688u|(21u<<19),2u<<6,0x20000200};
        const auto plan=planXenosUnpackedMipChain(words);if(!plan)return 1;
        std::vector<std::uint8_t> base(plan->baseBytes,0x5A),mips(plan->mipBytes,0xA5);
        if(!exercise(device,words,base,mips,true))return 1;
        ++cases;
        // Legacy one-level interface is still intact and rejects mip API.
        words[4]=0;words[5]=0x200;
        const auto description=XenosTextureDescription::decode(words);
        auto image=NativeTextureImage::create(device,*description,base);
        if(!image || image->levelCount()!=1 || image->uploadMipChain(base,{}) ||
            image->readback()!=linearizeXenosTexture2D(*description,base))return 1;
    }
    std::printf("SYNTHETIC_MIP_IMAGE_PASS cases=%u byte_exact_upload_readback=1 rejected_update_preserves_all_levels=1\n",cases);
}
