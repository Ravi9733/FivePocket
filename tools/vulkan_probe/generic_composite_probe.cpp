// EXPERIMENTAL device probe: render one captured draw through the generic
// XenosRecomp executor (native_generic_draw.h). Usage: <prepared dir> <output.bin>
#include "native_generic_draw.h"
#include "shader_module_pack.h"
#include "xenos_texture_2d.h"
#include "xenos_texture_description.h"
#include "xenos_expanded16_float_texture.h"
#include "xenos_raw10_texture.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

using namespace recomp::gpu;
static std::vector<std::uint8_t> readAll(const std::string& path) {
    std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};
}
static float depth20e4(std::uint32_t v) {
    const unsigned e=(v>>20)&15,m=v&0xFFFFF;
    return e?std::ldexp(1.0f+float(m)/1048576.0f,int(e)-15):std::ldexp(float(m)/1048576.0f,-14);
}
static std::uint16_t toHalf(float f) { // Round-to-nearest, finite non-negative inputs.
    if(!(f>0))return 0;
    int e=0;const float m=std::frexp(f,&e); // f = m*2^e, m in [0.5,1)
    if(e>16)return 0x7BFF;
    if(e<-13){const auto sub=std::lround(std::ldexp(f,24));return std::uint16_t(sub>1023?0x400:sub);}
    auto mant=std::lround((m*2-1)*1024);unsigned exp=unsigned(e-1+15);
    if(mant==1024){mant=0;++exp;}
    return exp>=31?0x7BFF:std::uint16_t((exp<<10)|mant);
}
static VkComponentSwizzle component(unsigned s) {
    switch(s){case 0:return VK_COMPONENT_SWIZZLE_R;case 1:return VK_COMPONENT_SWIZZLE_G;case 2:return VK_COMPONENT_SWIZZLE_B;
              case 3:return VK_COMPONENT_SWIZZLE_A;case 4:return VK_COMPONENT_SWIZZLE_ZERO;default:return VK_COMPONENT_SWIZZLE_ONE;}
}
int main(int argc,char** argv) {
    if(argc!=3){std::fprintf(stderr,"usage: %s <dir> <output.bin>\n",argv[0]);return 2;}
    const std::string dir=argv[1];
    ProbeDevice device;ShaderRequirements req;req.clipDistance=true;req.sampledImageArrayDynamicIndexing=true;req.runtimeDescriptorArray=true;
    if(!device.inspect() || !device.create(req)){std::printf("GENERIC_COMPOSITE_DEVICE_FAILED\n");return 1;}
    GenericDraw draw;
    const auto words=[](const std::vector<std::uint8_t>& b){std::vector<std::uint32_t> w(b.size()/4);std::memcpy(w.data(),b.data(),w.size()*4);return w;};
    draw.vertexCode=words(readAll(dir+"/vs.spv"));draw.pixelCode=words(readAll(dir+"/ps.spv"));
    draw.vertices=readAll(dir+"/vertices-le.bin");draw.stride=24;draw.vertexCount=6;
    draw.attributes={{0,VK_FORMAT_R32G32B32_SFLOAT,0},{13,VK_FORMAT_R32G32_SFLOAT,16}};
    draw.vertexConstants=readAll(dir+"/vconst-le.bin");draw.pixelConstants=readAll(dir+"/pconst-le.bin");
    draw.width=1280;draw.height=720;draw.target=VK_FORMAT_R8G8B8A8_UNORM;draw.targetBytesPerPixel=4;
    std::ifstream list(dir+"/textures.txt");std::string line;
    while(std::getline(list,line)) {
        std::istringstream in(line);unsigned reg=0;std::array<std::uint32_t,6> fetch{};std::string file;
        in>>reg;for(auto& f:fetch)in>>f;in>>file;
        const auto source=readAll(dir+"/"+file);
        auto desc=XenosTextureDescription::decode(fetch);
        if(!desc){std::printf("GENERIC_COMPOSITE_FETCH_REFUSED reg=%u\n",reg);return 1;}
        GenericTexture t;t.reg=reg;t.width=desc->width;t.height=desc->height;
        t.swizzle={component(desc->swizzle[0]),component(desc->swizzle[1]),component(desc->swizzle[2]),component(desc->swizzle[3])};
        std::optional<std::vector<std::uint8_t>> linear;
        const unsigned format=fetch[1]&0x3F;
        if(format==23) { // k_24_8_FLOAT: 32bpp tiling of format 6, depth in the upper 24 bits (20e4).
            // The planner re-decodes the fetch words, so rewrite them: format 6,
            // unsigned, no integer/bias, identity swizzle. Tiling is unchanged.
            auto f=fetch;f[1]=(f[1]&~63u)|6u;f[0]&=~(0xFFu<<2);
            f[3]&=~(1u|(0xFFFu<<1)|(63u<<13));f[3]|=(0u|1u<<3|2u<<6|3u<<9)<<1;
            const auto raw=XenosTextureDescription::decode(f);
            if(raw)linear=linearizeXenosTexture2D(*raw,source);
            if(linear) {
                std::vector<std::uint8_t> f(std::size_t{t.width}*t.height*4);
                for(std::size_t i=0;i<std::size_t{t.width}*t.height;++i) {
                    std::uint32_t w=0;std::memcpy(&w,linear->data()+i*4,4);
                    const float z=depth20e4(w>>8);std::memcpy(f.data()+i*4,&z,4);
                }
                linear=f;t.format=VK_FORMAT_R32_SFLOAT;t.filter=VK_FILTER_NEAREST;
                t.swizzle={VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_R};
            }
        } else if(format==54) {
            // Sampler exponent bias applies after fetch; a UNORM view cannot carry
            // it, so widen to RGBA16F scaled by 2^bias (filtering is linear).
            linear=linearizeXenosRaw10UnsignedTexture(*desc,source);t.format=VK_FORMAT_A2B10G10R10_UNORM_PACK32;
            if(linear && desc->exponentBias) {
                const float scale=std::ldexp(1.0f,desc->exponentBias);
                std::vector<std::uint8_t> wide(std::size_t{t.width}*t.height*8);
                for(std::size_t i=0;i<std::size_t{t.width}*t.height;++i) {
                    std::uint32_t v=0;std::memcpy(&v,linear->data()+i*4,4);
                    const float c[4]{float(v&1023)/1023*scale,float((v>>10)&1023)/1023*scale,float((v>>20)&1023)/1023*scale,float(v>>30)/3*scale};
                    for(unsigned k=0;k<4;++k){const auto h=toHalf(c[k]);std::memcpy(wide.data()+i*8+k*2,&h,2);}
                }
                linear=std::move(wide);t.format=VK_FORMAT_R16G16B16A16_SFLOAT;
            }
        } else if(format==29) {
            linear=linearizeXenosExpanded16FloatStorage(*desc,source);t.format=VK_FORMAT_R16G16B16A16_SFLOAT;
        } else {
            linear=linearizeXenosTexture2D(*desc,source);t.format=format==2?VK_FORMAT_R8_UNORM:VK_FORMAT_R8G8B8A8_UNORM;
        }
        if(!linear){std::printf("GENERIC_COMPOSITE_TEXTURE_REFUSED reg=%u format=%u bytes=%zu\n",reg,format,source.size());return 1;}
        t.pixels=std::move(*linear);
        std::printf("GENERIC_COMPOSITE_TEXTURE reg=%u format=%u size=%ux%u bytes=%zu\n",reg,format,t.width,t.height,t.pixels.size());
        draw.textures.push_back(std::move(t));
    }
    const char* why=nullptr;
    const auto out=renderGenericDraw(device,draw,&why);
    if(!out){std::printf("GENERIC_COMPOSITE_RENDER_REFUSED %s\n",why);return 1;}
    std::ofstream(argv[2],std::ios::binary).write(reinterpret_cast<const char*>(out->data()),out->size());
    std::size_t nonzero=0;for(std::size_t i=0;i<out->size();i+=4)nonzero+=(*out)[i]||(*out)[i+1]||(*out)[i+2];
    std::printf("GENERIC_COMPOSITE_PASS %s pixels=%zu nonzero=%zu experimental=1\n",why,out->size()/4,nonzero);
}
