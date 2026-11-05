#include "xenos_tiled32_texture.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main(){
    const std::array<std::uint32_t,6> descriptor{{0x80400002,0x10000086,0x3E01F,0xA80C14,3,0x210}};
    Tiled32Texture plan;assert(decodeTiled32Texture(descriptor,plan));
    assert((plan.swizzle==std::array<std::uint8_t,4>{2,1,0,3}));
    std::array<bool,1024> seen{};std::array<std::uint8_t,4096> source{},output{};
    for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x){std::uint32_t at=9999;
        assert(tiled32TexelOffset(x,y,at));assert(at<4096 && at%4==0 && !seen[at/4]);seen[at/4]=true;
        source[at]=255;source[at+1]=static_cast<std::uint8_t>(x);source[at+2]=static_cast<std::uint8_t>(y);
        source[at+3]=static_cast<std::uint8_t>(x^y);}
    assert(std::all_of(seen.begin(),seen.end(),[](bool value){return value;}));
    assert(decodeTiled32Pixels(plan,source.data(),source.size(),output));
    for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x){const auto i=(y*32+x)*4;
        assert(output[i]==x && output[i+1]==y && output[i+2]==(x^y) && output[i+3]==255);}
    unsigned accepted=0,rejected=0;
    for(unsigned encoded=0;encoded<4096;++encoded){
        auto words=descriptor;words[3]=(words[3]&~0x1FFEu)|(encoded<<1);
        const auto saved=plan;bool valid=true;
        for(unsigned lane=0;lane<4;++lane)valid&=((encoded>>(3*lane))&7)<6;
        assert(decodeTiled32Texture(words,plan)==valid);
        if(valid){++accepted;assert(decodeTiled32Pixels(plan,source.data(),source.size(),output));}
        else{++rejected;assert(plan.swizzle==saved.swizzle);}
    }
    assert(accepted==1296 && rejected==2800);
    assert(decodeTiled32Texture(descriptor,plan));output.fill(0xA5);const auto sentinel=output;
    for(unsigned n:{0u,4092u,4095u,4097u}){
        assert(!decodeTiled32Pixels(plan,source.data(),n,output));assert(output==sentinel);}
    assert(!decodeTiled32Pixels(plan,nullptr,4096,output));assert(output==sentinel);
    plan.swizzle[3]=6;assert(!decodeTiled32Pixels(plan,source.data(),4096,output));assert(output==sentinel);
    for(unsigned word=0;word<6;++word){auto mutated=descriptor;mutated[word]^=word==1?(1u<<29):word==3?(1u<<13):1;
        assert(!decodeTiled32Texture(mutated,plan));}
    std::uint32_t offset=123;assert(!tiled32TexelOffset(32,0,offset) && offset==123);
    assert(!tiled32TexelOffset(0,32,offset) && offset==123);
    std::puts("TILED32_TEXTURE_V1 unique_texels=1024 swizzles=4096 gpu_upload=0 sampler_verified=0");
}
