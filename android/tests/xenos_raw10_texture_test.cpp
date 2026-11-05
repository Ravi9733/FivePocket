// Storage and copy-consumer tests. Authored bytes / captured snapshots are not
// live allocation ownership. Optional Vulkan point sampling is a component
// measurement, not the game's shader or Xbox filtering-conformance proof.
#include "xenos_raw10_texture.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#ifdef RECOMP_RAW10_GPU
#include "mip_sample_probe.h"
#include "process_limits.h"
#endif
using namespace recomp::gpu;
namespace {
std::vector<std::uint8_t> read(const std::string& path,unsigned limit) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);assert(f);
    const auto n=f.tellg();assert(n>0 && std::uint64_t(n)<=limit);f.seekg(0);
    std::vector<std::uint8_t> b(static_cast<std::size_t>(n));
    assert(f.read(reinterpret_cast<char*>(b.data()),n));return b;
}
void check(const XenosTextureDescription& d,const std::vector<std::uint8_t>& bytes) {
    const auto f=d.fetch;const auto p=planXenosRaw10UnsignedStorage(d);assert(p);
    assert(p->bytesPerTexel==4 && p->linearBytes==d.width*d.height*4);
    const auto linear=linearizeXenosRaw10UnsignedTexture(d,bytes);assert(linear);
    assert(storeXenosTexture2D(p->description,*linear,bytes)==std::optional{bytes});
    // Neither the legacy importer nor a changed representation may silently
    // become packed10 because this opt-in helper exists.
    assert(!planXenosTexture2D(d));
    for(unsigned format:{2u,6u,7u,30u,32u,36u,55u}) {
        auto wrong=f;wrong[1]=(wrong[1]&~63u)|format;
        assert(!planXenosRaw10UnsignedStorage(*XenosTextureDescription::decode(wrong)));
    }
    for(unsigned lane=0;lane<4;++lane)for(unsigned sign=1;sign<4;++sign) {
        auto wrong=f;wrong[0]|=sign<<(2+2*lane);
        assert(!planXenosRaw10UnsignedStorage(*XenosTextureDescription::decode(wrong)));
    }
    for(const auto edit:std::array<std::array<unsigned,2>,7>{{
        {3,1},{4,4},{4,64},{5,4096},{5,2048},{1,1024},{5,1024}}}) {
        auto wrong=f;wrong[edit[0]]|=edit[1];
        assert(!planXenosRaw10UnsignedStorage(*XenosTextureDescription::decode(wrong)));
    }
    for(unsigned swizzle=6;swizzle<8;++swizzle) {
        auto wrong=f;wrong[3]=(wrong[3]&~14u)|(swizzle<<1);
        assert(!planXenosRaw10UnsignedStorage(*XenosTextureDescription::decode(wrong)));
    }
    auto absent=f;absent[1]&=4095;
    assert(!planXenosRaw10UnsignedStorage(*XenosTextureDescription::decode(absent)));
    auto shortBytes=bytes;shortBytes.pop_back();assert(!linearizeXenosRaw10UnsignedTexture(d,shortBytes));
    auto longBytes=bytes;longBytes.push_back(0);assert(!linearizeXenosRaw10UnsignedTexture(d,longBytes));
    auto forged=d;forged.width=1;forged.format=6;forged.exponentBias=0;
    assert(linearizeXenosRaw10UnsignedTexture(forged,bytes)==linear); // Re-decodes authoritative fetch.
    for(unsigned bias=0;bias<64;++bias) {
        auto changed=f;changed[3]=(changed[3]&~(63u<<13))|(bias<<13);
        const auto adjusted=XenosTextureDescription::decode(changed);assert(adjusted);
        assert(adjusted->exponentBias==int(bias<32?bias:bias-64));
        assert(linearizeXenosRaw10UnsignedTexture(*adjusted,bytes)==linear);
    }
    assert(d.fetch==f);
}
#ifdef RECOMP_RAW10_GPU
void gpu(ProbeDevice& device,const XenosTextureDescription& d,
        const std::vector<std::uint8_t>& bytes,const std::vector<std::uint32_t>& code) {
    const auto linear=linearizeXenosRaw10UnsignedTexture(d,bytes);assert(linear);
    assert(!NativeTextureImage::create(device,d,bytes));
    auto image=NativeTextureImage::createRaw10Unsigned(device,d,bytes);assert(image);
    assert(image->description().fetch==d.fetch && image->levelCount()==1 && image->layerCount()==1);
    assert(image->readback()==linear && !image->readback(1) && !image->readback(0,1));
    assert(!image->upload(bytes) && !image->uploadDepth24Float(bytes));
    assert(!image->uploadMipChain(bytes,{}) && !image->uploadCubeBase(bytes));
    auto truncated=bytes;truncated.pop_back();assert(!image->uploadRaw10Unsigned(truncated));
    assert(!NativeTextureImage::createRaw10Unsigned(device,d,truncated));
    assert(image->readback()==linear && image->uploadRaw10Unsigned(bytes));
    // Explicit base-level POINT policy for this measurement. This is NOT a
    // decoder for the game's instruction, implicit derivatives or getWeights.
    NativeTextureSamplerPlan plan{{d.clamp[0],d.clamp[1]},0,0,0,0,0};
    std::vector<std::array<float,4>> queries;
    const unsigned step=d.width>64?13:1;
    for(unsigned y=0;y<d.height;y+=step)for(unsigned x=0;x<d.width;x+=step)
        queries.push_back({(x+.5f)/d.width,(y+.5f)/d.height,0,float(d.exponentBias)});
    // Include texel-edge neighbourhoods and wrap/clamp edges; avoid asserting
    // undocumented ties exactly on a boundary after normalized division.
    for(float x:{-1.75f,-.25f,.125f,.75f,1.25f,31.125f})
    for(float y:{-.25f,.125f,.75f,1.25f,31.125f})
        queries.push_back({x/d.width,y/d.height,0,float(d.exponentBias)});
    const auto result=sampleMipProbe(device,*image,plan,code,queries);assert(result);
    const auto address=[](int at,unsigned size,unsigned clamp) {
        assert(clamp==0 || clamp==2);
        return clamp==2?std::clamp(at,0,int(size)-1):(at%int(size)+int(size))%int(size);
    };
    double maximum=0;
    for(unsigned i=0;i<queries.size();++i) {
        const auto x=address(int(std::floor(double(queries[i][0])*d.width)),d.width,d.clamp[0]);
        const auto y=address(int(std::floor(double(queries[i][1])*d.height)),d.height,d.clamp[1]);
        const auto at=(std::size_t(y)*d.width+x)*4;
        const auto w=std::uint32_t{(*linear)[at]}|(std::uint32_t{(*linear)[at+1]}<<8)|
            (std::uint32_t{(*linear)[at+2]}<<16)|(std::uint32_t{(*linear)[at+3]}<<24);
        const double raw[]{double(w&1023)/1023,double((w>>10)&1023)/1023,
            double((w>>20)&1023)/1023,double(w>>30)/3,0,1};
        for(unsigned c=0;c<4;++c) {
            const auto wanted=std::ldexp(raw[d.swizzle[c]],d.exponentBias);
            const auto error=std::abs((*result)[i][c]-wanted);maximum=std::max(maximum,error);
            assert(std::isfinite((*result)[i][c]) && error<=std::ldexp(2e-6,d.exponentBias));
        }
    }
    assert(image->readback()==linear);
    std::printf("RAW10_GPU_SAMPLE_CASE_PASS width=%u height=%u bias=%d queries=%zu max_error=%.9g storage_exact=1 canaries=1 game_shader=0\n",
        d.width,d.height,d.exponentBias,queries.size(),maximum);
}
#endif
}
int main(int argc,char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
#ifdef RECOMP_RAW10_GPU
    assert(argc==4 && initializeProbeProcess());
    ProbeDevice device;assert(device.inspect() && device.create({}));
    const auto binary=read(argv[3],262144);assert(binary.size()%4==0);
    std::vector<std::uint32_t> code(binary.size()/4);std::memcpy(code.data(),binary.data(),binary.size());
#else
    assert(argc==3);
#endif
    unsigned cases=0;
    for(unsigned tiled=0;tiled<2;++tiled)for(unsigned endian=0;endian<4;++endian)
    for(unsigned bias:{0u,3u}) {
        const std::array<unsigned,6> f{2u|(1u<<22)|(tiled<<31)|0x24800u,
            0x100000u|54u|(endian<<6),31u|(30u<<13),0xC14u|(bias<<13),0,0x200};
        const auto d=XenosTextureDescription::decode(f);assert(d);
        const auto p=planXenosRaw10UnsignedStorage(*d);assert(p);
        std::vector<std::uint8_t> bytes(p->sourceBytes);
        for(unsigned i=0;i<bytes.size();++i)bytes[i]=(i*73+(i>>5)*31)&255;
        check(*d,bytes);++cases;
#ifdef RECOMP_RAW10_GPU
        gpu(device,*d,bytes,code);
#endif
    }
    const auto fetch=read(argv[1],24);assert(fetch.size()==24);std::array<unsigned,6> f{};
    for(unsigned i=0;i<6;++i)for(unsigned j=0;j<4;++j)f[i]=(f[i]<<8)|fetch[i*4+j];
    const auto d=XenosTextureDescription::decode(f);assert(d);
    const auto bytes=read(argv[2],64*1024*1024);check(*d,bytes);
#ifdef RECOMP_RAW10_GPU
    gpu(device,*d,bytes,code);
    // Constant swizzles test that exponent follows view selection, not raw
    // channel unpacking. Stored texture words must remain bit-identical.
    auto constants=f;constants[3]=(constants[3]&~0x1FFEu)|(4u<<1)|(5u<<4)|(0u<<7)|(3u<<10);
    gpu(device,*XenosTextureDescription::decode(constants),bytes,code);
    std::puts("RAW10_GPU_IMPORT_PASS actual_capture=1 point_sampling=1 signed_mips_short_refused=1 native_game_admission=0");
#endif
    std::printf("RAW10_STORAGE_IMPORT_PASS authored_cases=%u actual_capture=1 bias_profiles=64 byte_exact=1 invalid_refused=1\n",cases);
}
