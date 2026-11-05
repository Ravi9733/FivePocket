#include "native_swap_image.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
namespace {
// Independent bit-routing oracle from the documented bank/pipe layout in
// Xenia texture_address.h (95a5c3ee250f80c3b9d139658649d9ffb6db3eec).
// Do not call the production tile helper to construct its own test input.
unsigned referenceOffset(unsigned x,unsigned y) {
    const auto macro=(y/32*40+x/32)*64;
    const auto micro=((y/2%8)*8+x%8);
    const auto blockBytes=(macro+micro)*4;
    const auto bank=y/16%2,pipe=(x/8%4)^((y/8%2)*2);
    return (y%2)*16+pipe*64+bank*2048+(blockBytes%16)+
        (blockBytes/16%2)*32+(blockBytes/32%8)*256+(blockBytes/256)*4096;
}
}
int main() {
    constexpr std::uint32_t Virtual=0xBF2D8000,Physical=0x1F2D8000;
    const std::array<std::uint32_t,6> fetch{0x8A000002,Virtual|6,0x0059E4FF,0x1414,0,0x200};
    auto make=[&](auto f){return NativeSwapImagePlan::make(f,Virtual,Physical,6,0,1280,720);};
    auto plan=make(fetch);assert(plan && plan->virtualBase()==Virtual && plan->physicalBase()==Physical);
    for(unsigned word=0;word<6;++word)for(unsigned bit=0;bit<32;++bit){
        auto bad=fetch;bad[word]^=1u<<bit;assert(!make(bad));
    }
    const std::array<std::uint32_t,6> arguments{Virtual,Physical,6,0,1280,720};
    for(unsigned arg=0;arg<6;++arg){auto bad=arguments;bad[arg]^=1;
        assert(!NativeSwapImagePlan::make(fetch,bad[0],bad[1],bad[2],bad[3],bad[4],bad[5]));
    }
    auto movedFetch=fetch;movedFetch[1]=0xFFFFF006;
    assert(!NativeSwapImagePlan::make(movedFetch,0xFFFFF000,Physical,6,0,1280,720));
    assert(!NativeSwapImagePlan::make(fetch,Virtual,0x1FFFF000,6,0,1280,720));
    auto otherFetch=fetch;otherFetch[1]=0xA0200006;
    assert(NativeSwapImagePlan::make(otherFetch,0xA0200000,0x200000,6,0,1280,720));
    std::vector<std::uint8_t> tiled(NativeSwapImagePlan::Capacity,0xCD),active(tiled.size()/4),expected(NativeSwapImagePlan::PixelBytes);
    assert(referenceOffset(0,0)==0 && referenceOffset(4,0)==32 && referenceOffset(8,0)==64 &&
        referenceOffset(16,0)==128 && referenceOffset(0,16)==2048 && referenceOffset(32,0)==4096 && referenceOffset(0,32)==163840);
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x){
        const auto offset=referenceOffset(x,y);assert(offset+4<=tiled.size() && !active[offset/4]);active[offset/4]=1;
        const auto r=std::uint8_t(x*13+y*7),g=std::uint8_t(x*3+y*19),b=std::uint8_t(x*29+y*5);
        tiled[offset]=b;tiled[offset+1]=g;tiled[offset+2]=r;tiled[offset+3]=std::uint8_t(x+y); // Alpha must be ignored.
        const auto at=(y*1280+x)*4;expected[at]=r;expected[at+1]=g;expected[at+2]=b;expected[at+3]=255;
    }
    std::vector<std::uint8_t> result{1,2,3};
    for(auto size:{std::size_t{0},tiled.size()-1,tiled.size()+1}){
        assert(!plan->decode(tiled.data(),size,result) && (result==std::vector<std::uint8_t>{1,2,3}));
    }
    assert(!plan->decode(nullptr,tiled.size(),result) && (result==std::vector<std::uint8_t>{1,2,3}));
    assert(!plan->decode(reinterpret_cast<const std::uint8_t*>(~std::uintptr_t{0}-3),tiled.size(),result) &&
        (result==std::vector<std::uint8_t>{1,2,3}));
    assert(plan->decode(tiled.data(),tiled.size(),result) && result==expected);
    for(unsigned word=0;word<active.size();++word)if(!active[word])std::fill_n(tiled.data()+word*4,4,0x17);
    assert(plan->decode(tiled.data(),tiled.size(),result) && result==expected);
    assert(plan->decode(tiled.data(),tiled.size(),tiled) && tiled==expected); // Supported alias.
    std::puts("NATIVE_SWAP_IMAGE: 205 rejection controls, 921600 asymmetric pixels, independent tiling oracle, padding and alias PASS; no presentation");
}
