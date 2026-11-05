#include "xenos_color_7e3.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main(){
    unsigned rejected=0;
    for(unsigned code=0;code<1024;++code){
        std::uint32_t decoded=0,packed=17;assert(unpack7e3(code,decoded));
        assert(pack7e3Checked(decoded,packed) && packed==code);
        for(unsigned alpha=0;alpha<4;++alpha){
            const std::uint32_t word=code|((1023-code)<<10)|(((code*37)&1023)<<20)|(alpha<<30);
            const auto values=unpackColor7e3(word);
            assert(packColor7e3(values,packed) && packed==word);
        }
        if(code==1023)continue;
        std::uint32_t next=0;assert(unpack7e3(code+1,next));
        const auto midpoint=colorBits((colorFloat(decoded)+colorFloat(next))*.5f);
        for(int direction:{-1,0,1}){
            const std::uint32_t input=midpoint+direction;
            const unsigned expect=direction<0?code:direction>0?code+1:code+(code&1);
            packed=0xABCD1234;
            if(pack7e3Checked(input,packed))assert(packed==expect);
            else {assert(packed==0xABCD1234);assert(code<128 && !(code&1) && direction==1);++rejected;}
        }
    }
    assert(rejected==64);
    for(auto invalid:{0x7F800000u,0xFF800000u,0x7FC00000u,0xFFC00000u}){
        std::uint32_t out=19;assert(!pack7e3Checked(invalid,out) && out==19);
        for(unsigned lane=0;lane<4;++lane){std::array<std::uint32_t,4> v{0,0,0,0};v[lane]=invalid;
            assert(!packColor7e3(v,out) && out==19);}
    }
    std::uint32_t out=19;assert(!unpack7e3(1024,out) && out==19);
    for(auto input:{0x80000000u,0xBF800000u,0xFF7FFFFFu})assert(pack7e3Checked(input,out) && out==0);
    for(auto input:{0x42000000u,0x7F7FFFFFu})assert(pack7e3Checked(input,out) && out==1023);
    assert(packColor7e3({0,0,0,0x3F800000},out) && out==0xC0000000);
    assert(unpackColor7e3(0xC0000000)==(std::array<std::uint32_t,4>{0,0,0,0x3F800000}));
    std::printf("COLOR_7E3_REFERENCE_V1 decoded=1024 rgba_roundtrips=4096 midpoint_neighbors=3069 "
        "pre_round_disagreements_rejected=%u retail_rop_verified=0\n",rejected);
}
