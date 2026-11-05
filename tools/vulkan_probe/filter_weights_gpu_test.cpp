// Actual shader arithmetic in a diagnostic compute kernel, not a game draw.
#include "mip_sample_probe.h"
#include "process_limits.h"
#include "native_rgb7e3_replacement.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
using namespace recomp::gpu;
namespace {
std::vector<std::uint8_t> read(const std::string& path,unsigned limit) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);assert(f);
    const auto n=f.tellg();assert(n>0 && std::uint64_t(n)<=limit);f.seekg(0);
    std::vector<std::uint8_t> b(static_cast<std::size_t>(n));assert(f.read(reinterpret_cast<char*>(b.data()),n));return b;
}
}
int main(int argc,char** argv) {
    assert((argc==2 || (argc==3 && std::string(argv[2])=="--full-plane")) && initializeProbeProcess());std::setvbuf(stdout,nullptr,_IONBF,0);
    const std::string prefix=argv[1];const auto fetch=read(prefix+"/fetch.bin",24);assert(fetch.size()==24);
    std::array<unsigned,6> words{};for(unsigned i=0;i<6;++i)for(unsigned j=0;j<4;++j)words[i]=words[i]*256+fetch[i*4+j];
    const auto d=XenosTextureDescription::decode(words);assert(d);
    const auto raw=read(prefix+"/base.bin",8*1024*1024);
    const auto input=read(prefix+"/queries.bin",32768*16),expected=read(prefix+"/expected.bin",32768*16);
    assert(input.size()%16==0 && input.size()==expected.size());
    std::vector<std::array<float,4>> queries(input.size()/16),reference(expected.size()/16);
    std::memcpy(queries.data(),input.data(),input.size());std::memcpy(reference.data(),expected.data(),expected.size());
    const auto binary=read(prefix+"/original.comp.spv",262144);assert(binary.size()%4==0);
    std::vector<unsigned> code(binary.size()/4);std::memcpy(code.data(),binary.data(),binary.size());
    ProbeDevice device;assert(device.inspect() && device.create({}));
    auto image=NativeTextureImage::createRaw10Unsigned(device,*d,raw);assert(image);
    const auto before=image->readback();assert(before);
    const NativeTextureSamplerPlan sampler{{d->clamp[0],d->clamp[1]},0,0,0,0,0};
    const auto result=sampleMipProbe(device,*image,sampler,code,queries);assert(result);
    double maximum=0,relative=0;unsigned differences=0;
    for(unsigned i=0;i<result->size();++i)for(unsigned c=0;c<4;++c) {
        const float wanted=reference[i][c],actual=(*result)[i][c];
        assert(std::isfinite(wanted) && std::isfinite(actual));
        const double error=std::abs(double(actual)-wanted),scale=std::max(1.,std::abs(double(wanted)));
        maximum=std::max(maximum,error);relative=std::max(relative,error/scale);
        if(error>2e-5*scale) {
            if(differences<8)std::printf("FILTER_WEIGHTS_MISMATCH query=%u lane=%u uv=%.9g,%.9g actual=%.9g expected=%.9g error=%.9g\n",i,c,queries[i][0],queries[i][1],actual,wanted,error);
            ++differences;
        }
    }
    assert(!differences && image->readback()==before && image->description().fetch==words);
    if(argc==3) {
        constexpr unsigned width=640,height=360,pixels=width*height;
        std::vector<float> full(pixels*4);
        for(unsigned begin=0;begin<pixels;begin+=32768) {
            const auto count=std::min(32768u,pixels-begin);
            std::vector<std::array<float,4>> grid(count);
            for(unsigned i=0;i<count;++i)grid[i]={((begin+i)%width+.5f)/width,((begin+i)/width+.5f)/height,0,0};
            const auto part=sampleMipProbe(device,*image,sampler,code,grid);assert(part && part->size()==count);
            for(unsigned i=0;i<count;++i)for(unsigned c=0;c<4;++c)full[(begin+i)*4+c]=(*part)[i][c];
        }
        for(unsigned i=0;i<queries.size();++i) {
            const auto x=std::lround(queries[i][0]*width-.5f),y=std::lround(queries[i][1]*height-.5f);
            assert(x>=0 && x<width && y>=0 && y<height);
            for(unsigned c=0;c<4;++c)assert(full[(y*width+x)*4+c]==(*result)[i][c]);
        }
        NativeEdram32Bands owner;
        const std::vector<std::uint8_t> initial(1280*720*4,0x5A);
        assert(owner.recordCompletedRawColorRows(720,1280,1280,720,initial,1));
        const NativeOffscreenDrawPlan plan{0x1000,width,height,width,720,184,3};
        {NativeEdram32Mutation mutation(owner);
         assert(recordCompletedNativeRgb7e3Replacement(mutation,plan,full,Color7e3Rounding::XeniaShiftRound));mutation.commit();}
        const auto output=owner.readColorWords(720,1280,1280,720);assert(output);
        auto expectedWords=std::vector<unsigned>(1280*720,0x5A5A5A5A);
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
            std::array<unsigned,4> bits{};
            for(unsigned c=0;c<4;++c)bits[c]=colorBits(full[(y*width+x)*4+c]);
            unsigned packed=0;assert(packColor7e3(bits,packed,Color7e3Rounding::XeniaShiftRound));
            const auto tile=(y/16)*8+x/80;
            expectedWords[((tile/16)*16+y%16)*1280+(tile%16)*80+x%80]=packed;
        }
        assert(*output==expectedWords && owner.nextSerial()==3 && image->readback()==before);
        NativeEdram32Bands partial;
        {NativeEdram32Mutation mutation(partial);
         assert(recordCompletedNativeRgb7e3Replacement(mutation,plan,full,Color7e3Rounding::XeniaShiftRound));mutation.commit();}
        assert(partial.knownTiles()==176 && !partial.readColorWords(720,width,width,368));
        std::puts("FILTER_WEIGHTS_PHYSICAL_CONNECTION_PASS pixels=230400 sampled_reference_points=3249 rgba7e3a2=1 padding_and_other_tiles_preserved=1 unknown_padding_not_invented=1 authored_initial_owner=1 shader_compute_only=1 rasterization=0 live_publication=0");
    }
    std::printf("FILTER_WEIGHTS_GPU_PASS queries=%zu components=%zu max_absolute=%.9g max_scaled=%.9g tolerance=2e-5 source_unchanged=1 canaries=1 original_ps_arithmetic=1 native_draw=0 xbox_filter_conformance=0\n",queries.size(),queries.size()*4,maximum,relative);
}
