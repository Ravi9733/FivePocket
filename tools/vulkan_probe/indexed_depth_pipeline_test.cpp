// Connected authored ABI12 raster test. No game/Xbox equivalence claim.
#include "native_linear_sample_target.h"
#include "native_texture_image.h"
#include "process_limits.h"
#include <cassert>
#include <cstdio>
#include <fstream>
using namespace recomp::gpu;
namespace {
std::vector<std::uint32_t> code(const char* path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);assert(f);
    const auto size=static_cast<std::size_t>(f.tellg());assert(size && size%4==0 && size<65536);
    std::vector<std::uint32_t> result(size/4);f.seekg(0);
    assert(f.read(reinterpret_cast<char*>(result.data()),size));return result;
}
void word(std::vector<std::uint8_t>& input,unsigned at,std::uint32_t value) {
    for(unsigned b=0;b<4;++b)input.at(at+b)=static_cast<std::uint8_t>(value>>(24-b*8));
}
void scalar(std::vector<std::uint8_t>& input,unsigned at,float f) {
    std::uint32_t value;std::memcpy(&value,&f,4);word(input,at,value);
}
void expect(const std::optional<std::vector<float>>& pixels,const std::array<float,4>& inside,
        const std::array<unsigned,4>& rectangle={0,0,8,8}) {
    assert(pixels && pixels->size()==8*8*4);
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)for(unsigned c=0;c<4;++c) {
        const bool covered=x>=rectangle[0] && y>=rectangle[1] &&
            x<rectangle[0]+rectangle[2] && y<rectangle[1]+rectangle[3];
        const auto actual=(*pixels)[(y*8+x)*4+c],expected=covered?inside[c]:0.f;
        if(actual!=expected)std::fprintf(stderr,"INDEXED_PIXEL_MISMATCH x=%u y=%u c=%u expected=%.9g actual=%.9g\n",x,y,c,expected,actual);
        assert(actual==expected);
    }
}
}
int main(int argc,char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    assert(argc==4 && initializeProbeProcess());
    ProbeDevice device;assert(device.inspect() && device.create({}));
    NativeTranslatedDrawProgram program;program.vertexCode=code(argv[1]);program.pixelCode=code(argv[2]);
    program.stride=32;program.indexed=true;program.abiVersion=12;program.textureSlot=UINT32_MAX;
    std::vector<std::uint8_t> input(8208+6*32);word(input,0,6);
    const std::array<std::array<float,2>,3> points{{{-1,-1},{3,-1},{-1,3}}};
    const std::array<float,4> red{1,0,0,1},green{0,1,0,1},black{},white{1,1,1,1};
    for(unsigned i=0;i<6;++i) {
        const std::array<float,4> p{points[i%3][0],points[i%3][1],i<3?.5f:.25f,1};
        for(unsigned c=0;c<4;++c) {
            scalar(input,8208+i*32+c*4,p[c]);scalar(input,8208+i*32+16+c*4,(i<3?red:green)[c]);
        }
    }
    const std::vector<std::uint32_t> rear{2,0,1},front{5,3,4};
    unsigned cases=0;
    for(unsigned samples:{1u,2u,4u}) {
        auto target=NativeLinearSampleTarget::create(device,8,8,samples);assert(target);
        assert(!target->borrowDepthStencilSamplesForReadOnlyDiagnostic());
        NativeTranslatedPipelineOptions o;o.samples=samples;o.minDepth=0;o.maxDepth=1;
        XenosDepthStencilState state;state.depthEnable=state.depthWrite=true;state.depthCompare=1;o.depthStencil=state;
        const auto draw=[&](const std::vector<std::uint32_t>& indices,bool color=false,bool depth=false,
                std::optional<std::array<unsigned,4>> scissor=std::nullopt) {
            ++cases;std::printf("INDEXED_CASE_BEGIN case=%u samples=%u color_load=%u depth_load=%u\n",cases,samples,unsigned(color),unsigned(depth));
            return target->draw(program,input,6,{1,1,0,0},{},o,NativeTranslatedBlend::Replace,
                black,1,0,color,indices,depth,scissor);
        };
        assert(!draw(front,false,true)); // Cannot load never-initialized depth.
        assert(!draw({}));assert(!draw({0,1,6}));assert(!draw({0,1}));
        assert(!draw(front,false,false,std::array<unsigned,4>{7,0,2,8}));
        auto badInput=input;word(badInput,0,7);
        assert(!target->draw(program,badInput,6,{1,1,0,0},{},o,NativeTranslatedBlend::Replace,black,1,0,false,front));
        badInput=input;badInput.pop_back();
        assert(!target->draw(program,badInput,6,{1,1,0,0},{},o,NativeTranslatedBlend::Replace,black,1,0,false,front));
        auto badProgram=program;badProgram.abiVersion=4;
        assert(!target->draw(badProgram,input,6,{1,1,0,0},{},o,NativeTranslatedBlend::Replace,black,1,0,false,front));
        expect(draw(front),green); // DrawArrays(3) would incorrectly draw red.
        assert(target->borrowDepthStencilSamplesForReadOnlyDiagnostic());
        expect(draw(rear,true,true),green); // Existing .25 blocks later .5.
        expect(draw(rear,true,false),red); // Explicit depth clear admits .5.
        expect(draw(front),green);
        o.depthStencil->depthWrite=false;
        expect(draw(rear,false,true),black); // Color clear MUST NOT clear depth.
        o.depthStencil->depthWrite=true;
        const std::array<unsigned,4> scissor{2,1,3,5};
        expect(draw(front,false,false,scissor),green,scissor);
        expect(draw(front,false,false,std::array<unsigned,4>{8,8,0,0}),black);
        // Preserve stencil writes as well as depth between distinct pipelines.
        o.depthStencil->depthEnable=false;o.depthStencil->stencilEnable=true;
        o.depthStencil->front={7,0,2,0,37,255,255};
        expect(draw(front),green);
        o.depthStencil->front={2,0,0,0,37,255,0};
        expect(draw(rear,false,true),red);
        expect(draw(rear,false,false),black);
        std::printf("INDEXED_DEPTH_GPU_CASE_PASS samples=%u effective_indices=1 depth_preserved=1 stencil_preserved=1 scissor=1\n",samples);
    }
    // Four distinct borrowed images, then a rotated binding negative control.
    const std::array<std::uint32_t,6> fetch{(1u<<22)|2,0x10000006u,0,0xD10u|(21u<<19),0,0x200};
    const auto desc=XenosTextureDescription::decode(fetch);assert(desc);
    assert((desc->swizzle==std::array<unsigned,4>{0,1,2,3}));
    const auto plan=planXenosTexture2D(*desc);assert(plan);
    std::array<std::unique_ptr<NativeTextureImage>,4> images;
    VkSamplerCreateInfo si{};si.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter=si.minFilter=VK_FILTER_NEAREST;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler{};assert(vkCreateSampler(device.device,&si,nullptr,&sampler)==VK_SUCCESS);
    NativeTranslatedPipelineOptions o;o.minDepth=0;o.maxDepth=1;o.depthStencil=XenosDepthStencilState{};
    for(unsigned i=0;i<4;++i) {
        std::vector<std::uint8_t> bytes(plan->sourceBytes);bytes[i]=255;
        images[i]=NativeTextureImage::create(device,*desc,bytes);assert(images[i]);
        o.externalTextures.push_back({sampler,images[i]->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }
    program.pixelCode=code(argv[3]);const std::vector<XenosTextureDescription> descriptions(4,*desc);
    auto target=NativeLinearSampleTarget::create(device,8,8,1);assert(target);
    expect(target->draw(program,input,6,{1,1,0,0},descriptions,o,NativeTranslatedBlend::Replace,black,1,0,false,front),white);
    std::rotate(o.externalTextures.begin(),o.externalTextures.begin()+1,o.externalTextures.end());
    expect(target->draw(program,input,6,{1,1,0,0},descriptions,o,NativeTranslatedBlend::Replace,black,1,0,false,front),black);
    auto bad=program;bad.abiVersion=4;bad.indexed=false;
    assert(!target->draw(bad,input,6,{1,1,0,0},descriptions,o,NativeTranslatedBlend::Replace,black,1,0));
    o.externalTextures.pop_back();
    assert(!target->draw(program,input,6,{1,1,0,0},descriptions,o,NativeTranslatedBlend::Replace,black,1,0,false,front));
    vkDestroySampler(device.device,sampler,nullptr);
    std::printf("INDEXED_FOUR_TEXTURE_GPU_PASS distinct_bindings=4 rotated_control=1 legacy_rejected=1\n");
    std::printf("INDEXED_DEPTH_GPU_PASS cases=%u authored_pixels=1 game_rendering=0 xbox_equivalence=0\n",cases);
}
