#include "native_color_chain_readback.h"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace recomp::gpu;
int main(){
    unsigned accepted=0;
    for(unsigned word=0;word<65536;++word){
        const unsigned e=(word>>10)&31,m=word&1023;
        std::uint32_t out=0xABCD1234;const bool okay=e!=31 && (e!=0 || !m);
        assert(nativeColorChainHalfBits(static_cast<std::uint16_t>(word),out)==okay);
        if(okay){++accepted;const float magnitude=e?std::ldexp(float(1024+m),int(e)-25):0;
            assert(out==colorBits(word&0x8000?-magnitude:magnitude));}
        else assert(out==0xABCD1234);
    }
    assert(accepted==61442);
    using P=NativeColorChainProfile<true,true,true>;
    std::vector<std::uint8_t> half(P::OutputBytes,0x5A),edram(P::BackingBytes,0x5A),tiled(P::CopyBytes,0x5A);
    const std::array<std::array<std::uint16_t,4>,5> colors{{{0,0,0,0},{0,0,0,0x3C00},{0x3C00,0,0,0x3C00},
        {0,0x3C00,0,0x3800},{0,0,0x3C00,0x3400}}};
    const std::array<std::uint32_t,5> packed{0,0xC0000000,0xC0000180,0x80060000,0x58000000};
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x){
        const auto i=y*1280+x,n=i%5;std::uint32_t from=0,to=0;
        assert(aliasSampleOffset({0x14000500,0xC02D0,1280,720,false},x,y,0,from));
        assert(colorCopyOffset(1280,720,x,y,to));const auto copied=copyColorWord(packed[n],true);
        for(unsigned c=0;c<4;++c){
            half[16+i*8+c*2]=static_cast<std::uint8_t>(colors[n][c]);half[17+i*8+c*2]=static_cast<std::uint8_t>(colors[n][c]>>8);
            edram[16+from+c]=static_cast<std::uint8_t>(packed[n]>>(c*8));tiled[16+to+c]=static_cast<std::uint8_t>(copied>>(c*8));
        }
    }
    const auto audit=[&](){return auditNativeColorChainReadback(half.data(),half.size(),edram.data(),edram.size(),tiled.data(),tiled.size());};
    assert(audit());unsigned failures=0;
    for(auto* buffer:{&half,&edram,&tiled}){
        for(std::size_t at:{std::size_t{0},std::size_t{15},buffer->size()-16,buffer->size()-1}){
            (*buffer)[at]^=1;assert(!audit());++failures;(*buffer)[at]^=1;
        }
    }
    for(auto at:{16u,16u+3686399,16u+7372800,P::BackingBytes-17}){
        edram[at]^=1;assert(!audit());++failures;edram[at]^=1;
    }
    for(unsigned i:{0u,12345u,921599u}){
        std::uint32_t from=0,to=0;assert(aliasSampleOffset({0x14000500,0xC02D0,1280,720,false},i%1280,i/1280,0,from));
        assert(colorCopyOffset(1280,720,i%1280,i/1280,to));
        edram[16+from]^=1;assert(!audit());++failures;edram[16+from]^=1;
        tiled[16+to]^=1;assert(!audit());++failures;tiled[16+to]^=1;
        const auto offset=16+i*8;const auto low=half[offset],high=half[offset+1];
        for(unsigned invalid:{1u,0x8001u,0x7C00u,0xFC00u,0x7E00u}){
            half[offset]=static_cast<std::uint8_t>(invalid);half[offset+1]=static_cast<std::uint8_t>(invalid>>8);
            assert(!audit());++failures;
        }
        half[offset]=low;half[offset+1]=high;
    }
    for(unsigned y:{720u,735u}){std::uint32_t to=0;assert(colorCopyOffset(1280,736,1279,y,to));
        tiled[16+to]^=1;assert(!audit());++failures;tiled[16+to]^=1;}
    assert(!auditNativeColorChainReadback(nullptr,half.size(),edram.data(),edram.size(),tiled.data(),tiled.size()));
    assert(!auditNativeColorChainReadback(half.data(),half.size()-4,edram.data(),edram.size(),tiled.data(),tiled.size()));
    assert(!auditNativeColorChainReadback(half.data(),half.size(),nullptr,edram.size(),tiled.data(),tiled.size()));
    assert(!auditNativeColorChainReadback(half.data(),half.size(),edram.data(),edram.size()+4,tiled.data(),tiled.size()));
    assert(!auditNativeColorChainReadback(half.data(),half.size(),edram.data(),edram.size(),nullptr,tiled.size()));
    assert(!auditNativeColorChainReadback(half.data(),half.size(),edram.data(),edram.size(),tiled.data(),tiled.size()-4));
    assert(audit());
    std::printf("NATIVE_COLOR_CHAIN_READBACK_V1 half_words=65536 pixels=921600 rejection_controls=%u expected_scene_pixels=0 gpu_executed=0\n",failures+6);
}
