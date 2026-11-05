#include "native_texture_image.h"
#include "native_color8_resolve.h"
#include "mip_sample_probe.h"
#include "process_limits.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
using namespace recomp::gpu;
namespace {
std::vector<std::uint8_t> readBytes(const std::string& path,unsigned limit) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);assert(f);
    const auto n=f.tellg();assert(n>0 && std::uint64_t(n)<=limit);
    std::vector<std::uint8_t> b(static_cast<std::size_t>(n));f.seekg(0);
    assert(f.read(reinterpret_cast<char*>(b.data()),n));return b;
}
float pwl(float gamma) {
    gamma=std::clamp(gamma,0.f,1.f);
    const float scale=gamma>=192.f/255.f?8.f/1024.f:gamma>=96.f/255.f?4.f/1024.f:gamma>=64.f/255.f?2.f/1024.f:1.f/1024.f;
    const float offset=gamma>=192.f/255.f?-1024.f:gamma>=96.f/255.f?-256.f:gamma>=64.f/255.f?-64.f:0.f;
    float value=((gamma*(255.f*1024.f))*scale)+offset;
    value=value+std::trunc(value*scale);return value*(1.f/1023.f);
}
void exercise(ProbeDevice& device,const std::array<std::uint32_t,6>& f,
        const std::vector<std::uint8_t>& source,const std::vector<std::uint32_t>& code,bool sample) {
    const auto d=XenosTextureDescription::decode(f);assert(d);
    const auto p=planNativeTextureSamplerCubeBase(f);assert(p);
    assert(!NativeTextureImage::create(device,*d,source));
    auto image=NativeTextureImage::createCubeBase(device,*d,source);assert(image);
    assert(image->layerCount()==6 && image->levelCount()==1 && image->description().fetch==f);
    assert(!image->upload(source) && !image->uploadMipChain(source,{}) && !image->readback(1) && !image->readback(0,6));
    std::array<std::vector<std::uint8_t>,6> planes;
    const auto faceBytes=d->width*d->height*4;
    for(unsigned face=0;face<6;++face) {
        const auto plan=planNativeColor8ResolveFace(*d,face);assert(plan);
        const std::vector<std::uint8_t> part(source.begin()+face*faceBytes,source.begin()+(face+1)*faceBytes);
        const auto linear=linearizeXenosTexture2D(plan->storage.description,part);assert(linear);
        planes[face]=*linear;assert(image->readback(0,face)==linear);
    }
    auto shortSource=source;shortSource.pop_back();assert(!image->uploadCubeBase(shortSource));
    assert(!NativeTextureImage::createCubeBase(device,*d,shortSource));
    for(unsigned face=0;face<6;++face)assert(image->readback(0,face)==std::optional{planes[face]});
    assert(image->uploadCubeBase(source));
    if(!sample)return;
    unsigned queriesTested=0,differences=0;double maximum=0;
    std::vector<std::array<float,4>> rawSamples;
    for(unsigned face=0;face<6;++face)for(unsigned gamma=0;gamma<2;++gamma) {
        std::vector<std::array<float,4>> queries;
        for(unsigned y=0;y<d->height;y+=std::max(d->height/128,1u))
        for(unsigned x=0;x<d->width;x+=std::max(d->width/128,1u))
            queries.push_back({(x+.5f)/d->width,(y+.5f)/d->height,float(face),float(gamma)});
        const auto centers=queries.size();
        for(float u:{-.25f,0.f,.125f,.5001f,.501953125f,.749f,1.f,1.25f})
        for(float v:{-.25f,0.f,.25f,.3333f,.5f,.891f,1.f,1.25f})
            queries.push_back({u,v,float(face),float(gamma)});
        const auto result=sampleMipProbe(device,*image,*p,code,queries);assert(result);
        if(!gamma)rawSamples=*result;
        for(unsigned i=0;i<queries.size();++i)for(unsigned c=0;c<4;++c) {
            const auto& q=queries[i];const auto selected=d->swizzle[c];
            const auto value=[&](int x,int y) {
                if(selected>=4)return double(selected-4);
                const auto at=(std::clamp(y,0,int(d->height)-1)*d->width+std::clamp(x,0,int(d->width)-1))*4+selected;
                return double(planes[face][at])/255.;
            };
            const double x=double(q[0])*d->width,y=double(q[1])*d->height;
            const int left=int(std::floor(x-.5)),top=int(std::floor(y-.5));
            const double fx=x-.5-left,fy=y-.5-top;
            const double filtered=p->magFilter?
                (value(left,top)*(1-fx)+value(left+1,top)*fx)*(1-fy)+
                (value(left,top+1)*(1-fx)+value(left+1,top+1)*fx)*fy:value(int(std::floor(x)),int(std::floor(y)));
            const double expected=gamma && c<3?pwl(float(filtered)):float(filtered);
            const auto actual=(*result)[i][c];const double error=std::abs(actual-expected);
            assert(std::isfinite(actual));maximum=std::max(maximum,error);
            // Validate stages independently: manual filtering is within4e-6
            // of the double reference; PWL is within4e-6 of the CPU conversion
            // of that actual filtered input. PWL's truncation can jump by one
            // UNORM10 step when float filtering straddles an integer boundary.
            const double stageExpected=gamma && c<3?pwl(rawSamples[i][c]):expected;
            assert(std::abs(actual-stageExpected)<=0.000004);
            const double budget=gamma && c<3?1./1023.+.000004:.000004;
            if(error>0.000004 && differences++<8)std::printf("CUBE_SAMPLE_DIFFERENCE face=%u query=%u c=%u gamma=%u center=%u cpu=%.9g gpu=%.9g error=%.9g budget=%.9g\n",
                face,i,c,gamma,unsigned(i<centers),expected,actual,error,budget);
            if(error>budget)std::printf("CUBE_SAMPLE_OUTSIDE_BUDGET size=%u endian=%u face=%u query=%u c=%u gamma=%u center=%u filtered=%.17g cpu=%.9g gpu=%.9g error=%.9g budget=%.9g\n",
                d->width,d->endian,face,i,c,gamma,unsigned(i<centers),filtered,expected,actual,error,budget);
            assert(error<=budget);
        }
        queriesTested+=queries.size();
    }
    std::vector<std::array<float,4>> directions;
    for(int x=-8;x<=8;++x)for(int y=-8;y<=8;++y)for(int z=-8;z<=8;++z)
        directions.push_back({float(x),float(y),float(z),2});
    directions.push_back({0,0,-0.f,2});
    const auto cubes=sampleMipProbe(device,*image,*p,code,directions);assert(cubes);
    for(unsigned i=0;i<directions.size();++i) {
        const auto& q=directions[i];const float x=q[0],y=q[1],z=q[2];std::array<float,4> expected{};
        if(std::abs(z)>=std::abs(x) && std::abs(z)>=std::abs(y))expected={-y,z<0?-x:x,2*z,z<0?5.f:4.f};
        else if(std::abs(y)>=std::abs(x))expected={y<0?-z:z,x,2*y,y<0?3.f:2.f};
        else expected={-y,x<0?z:-z,2*x,x<0?1.f:0.f};
        assert(std::memcmp(expected.data(),(*cubes)[i].data(),16)==0);
    }
    std::printf("CUBE_IMAGE_SAMPLING_PASS size=%u endian=%u queries=%u differences_4e_6=%u maximum=%.9g cube_alu_exact=%zu canaries=1 xbox_conformance=0\n",
        d->width,d->endian,queriesTested,differences,maximum,directions.size());
}
}
int main(int argc,char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    if(argc!=3 || !initializeProbeProcess())return 2;
    const auto bytes=readBytes(argv[2],262144);assert(bytes.size()%4==0);
    std::vector<std::uint32_t> code(bytes.size()/4);std::memcpy(code.data(),bytes.data(),bytes.size());
    ProbeDevice device;if(!device.inspect() || !device.create({}))return 2;
    if(std::string(argv[1])!="--synthetic") {
        const std::string root=argv[1];const auto fetch=readBytes(root+"/fetch.bin",24),source=readBytes(root+"/base.bin",6*1024*1024);
        assert(fetch.size()==24);std::array<std::uint32_t,6> f{};
        for(unsigned i=0;i<6;++i)for(unsigned b=0;b<4;++b)f[i]=(f[i]<<8)|fetch[4*i+b];
        exercise(device,f,source,code,true);return 0;
    }
    unsigned cases=0;
    for(unsigned width:{32u,64u,128u,256u,512u})for(unsigned endian=0;endian<4;++endian) {
        const std::array<std::uint32_t,6> f{2u|((width/32)<<22)|0x800248FCu,
            0x100000u|6u|(endian<<6),(width-1)|((width-1)<<13)|(5u<<26),0xA80C14,3,0x610};
        const auto d=XenosTextureDescription::decode(f);assert(d);
        std::vector<std::uint8_t> source(width*width*4*6);
        // Independent nonuniform raw bytes distinguish every face and expose
        // tiling/endian/swizzle mistakes. Expected linearization is also
        // covered by the separate host layout tests.
        for(unsigned i=0;i<source.size();++i)source[i]=(i*73+(i>>7)*31+(i/(width*width*4))*47)&255;
        exercise(device,f,source,code,width<=128);++cases;
    }
    std::printf("CUBE_IMAGE_STORAGE_PASS cases=%u all_faces_byte_exact=1 incomplete_upload_preserves_all_layers=1\n",cases);
}
