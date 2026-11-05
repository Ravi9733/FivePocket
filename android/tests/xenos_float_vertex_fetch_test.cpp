#include "xenos_float_vertex_fetch.h"
#include <cassert>
#include <iostream>
#include <vector>
using namespace recomp::gpu;
namespace {
std::array<std::uint32_t,3> encoded(unsigned format,unsigned swizzle,unsigned stride=7,
        unsigned offset=0,bool mini=false,unsigned dst=1,unsigned slot=0,unsigned src=0) {
    return {(src<<5)|(dst<<12)|(1<<19)|((slot/3)<<20)|((slot%3)<<25),
        swizzle|0x3000|(format<<16)|(unsigned(mini)<<30),(offset<<8)|stride};
}
void append(std::vector<std::uint8_t>& b,float f) {
    std::uint32_t w;std::memcpy(&w,&f,4);
    for(unsigned i=0;i<4;++i)b.push_back(static_cast<std::uint8_t>(w>>(24-i*8)));
}
}
int main() {
    const auto full=encoded(57,0xA88);
    FloatVertexFetch p; assert(decodeFloatVertexFetch(full,{},p));
    assert(p.fetchSlot==0 && p.strideWords==7 && p.components==3 && !p.mini);
    const auto prior=p;
    assert(decodeFloatVertexFetch(encoded(38,0x688,255,3,true,0,95,63),prior,p));
    assert(p.fetchSlot==0 && p.strideWords==7 && p.sourceRegister==0 && p.offsetWords==3 && p.mini);
    const auto mini=p;
    const auto sentinel=FloatVertexFetch{1,2,3,4,5,6,2,{0,1,4,5},false};
    unsigned guardCount=0;
    for (const auto& [word,bit]:std::array<std::pair<unsigned,unsigned>,11>{{
        {0,0},{0,11},{0,18},{0,19},{1,14},{1,15},{1,22},{1,24},{1,31},{2,30},{2,31}}}) {
        auto bad=full;bad[word]^=1u<<bit;p=sentinel;
        assert(!decodeFloatVertexFetch(bad,{},p) && p.offsetWords==6 && p.fetchSlot==4);++guardCount;
    }
    for (const auto& bad: {encoded(57,0x688),encoded(6,0x688),encoded(57,0xC88),
                          encoded(57,0xA88,0),encoded(57,0xA88,7,0,true)}) {
        p=sentinel;assert(!decodeFloatVertexFetch(bad,{},p) && p.fetchSlot==4);++guardCount;
    }
    std::vector<std::uint8_t> bytes;
    for (auto f:std::array<float,21>{-.5f,-.5f,.75f,0,0,0,1,639.5f,-.5f,.75f,0,0,0,1,
                                    639.5f,359.5f,.75f,0,0,0,1})append(bytes,f);
    KnownVertexVector old{{17,18,19,20},15}, out=old;
    for (unsigned i=0;i<3;++i) {
        assert(readFloatVertexFetch(prior,bytes.data(),bytes.size(),i,{},out));
        assert(out.known==15 && out.values[2]==.75f && out.values[3]==1);
        assert(readFloatVertexFetch(mini,bytes.data(),bytes.size(),i,{},out));
        assert((out.values==std::array<float,4>{0,0,0,1}));
    }
    for (unsigned badIndex: {3u,0x1000000u,UINT32_MAX}) {
        out=old;assert(!readFloatVertexFetch(prior,bytes.data(),bytes.size(),badIndex,{},out));
        assert(out.values==old.values);
    }
    for (unsigned size: {0u,1u,8u,4097u}) {
        out=old;assert(!readFloatVertexFetch(prior,bytes.data(),size,0,{},out));assert(out.values==old.values);
    }
    for (std::uint32_t bits: {0x7FC00000u,0x7F800000u,0xFF800000u}) {
        auto bad=bytes;for(unsigned i=0;i<4;++i)bad[i]=bits>>(24-i*8);
        out=old;assert(!readFloatVertexFetch(prior,bad.data(),bad.size(),0,{},out));assert(out.values==old.values);
    }
    // Exhaustively exercise legal/illegal component swizzles for all4 formats.
    unsigned swizzles=0;
    for (unsigned components=1;components<=4;++components)for(unsigned s=0;s<4096;++s) {
        const unsigned format=components==1?36:components==2?37:components==3?57:38;
        bool valid=true;for(unsigned i=0;i<4;++i){const auto v=(s>>(i*3))&7;valid&=v!=6 && !(v<4 && v>=components);}
        assert(decodeFloatVertexFetch(encoded(format,s),{},p)==valid);
        if(valid){assert(readFloatVertexFetch(p,bytes.data(),bytes.size(),0,{},out));
            for(unsigned i=0;i<4;++i)assert(bool(out.known&(1<<i))==(((s>>(i*3))&7)!=7));}
        ++swizzles;
    }
    std::cout<<"FLOAT_VERTEX_FETCH_V1 guards="<<guardCount<<" swizzles="<<swizzles<<" gpu_executed=0\n";
}
