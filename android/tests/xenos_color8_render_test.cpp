#include "xenos_color8_render.h"
#include <cassert>
#include <cstdio>
#include <limits>
#include <cstring>
using namespace recomp::gpu;
int main() {
    for(unsigned i=0;i<=255;++i) {
        const float reference=float(i)/255.f;std::uint32_t bits;std::memcpy(&bits,&reference,4);
        assert(xenosUnorm8FloatBits(i)==bits);
    }
    assert(!xenosUnorm8FloatBits(256));
    for(unsigned i=0;i<=1023;++i) {
        const float reference=float(i)/1023.f;std::uint32_t bits;std::memcpy(&bits,&reference,4);
        assert(xenosUnorm10FloatBits(i)==bits);
    }
    assert(!xenosUnorm10FloatBits(1024));
    const auto pack=[](float f){return (*packXenosColor8Render({f,f,f,f},1))[0];};
    assert(pack(0)==0 && pack(1)==255 && pack(.5f)==191);
    assert(pack(-100)==0 && pack(100)==255);
    assert(pack(.75f/1023.f)==0 && pack(1.5f/1023.f)==1 && pack(2.5f/1023.f)==2);
    assert((*packGta5Color8Clear({.5f,.5f,.5f,.5f},1))[0]==192);
    for(const auto [threshold,before,after]:std::array<std::array<unsigned,3>,3>{{{64,63,64},{128,95,96},{512,191,192}}}) {
        const float f=float(threshold)/1023.f;
        assert(pack(std::nextafter(f,0.f))==before);
        assert(pack(f)==after && pack(std::nextafter(f,1.f))==after);
    }
    // Authored dyadic inputs have exact multiply results: independent integer
    // expectations check every segment without recomputing the float formula.
    unsigned previous=0;
    for(unsigned i=0;i<=65536;++i) {
        const float f=float(i)/65536.f;
        const unsigned shift=i*1023u<64u*65536?0:i*1023u<128u*65536?1:i*1023u<512u*65536?2:3;
        const unsigned expected=(std::uint64_t{i}*1023/(65536u<<shift))+(shift?16u<<shift:0);
        const auto current=pack(f);assert(current==expected && current>=previous);previous=current;
    }
    unsigned roundtripChanges=0;
    for(unsigned byte=0;byte<256;++byte) {
        const auto result=pack(xenosPwlGammaByteToLinear(byte));
        assert(result<=byte && byte-result<=1);roundtripChanges+=result!=byte;
        const auto plain=packXenosColor8Render({float(byte)/255.f,0,0,float(byte)/255.f},0);
        assert(plain && (*plain)[0]==byte && (*plain)[3]==byte);
    }
    for(unsigned format:{0u,1u})for(float bad:{std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
        assert(!packXenosColor8Render({0,bad,0,1},format));
    assert(!packXenosColor8Render({0,0,0,1},2));
    assert((*packXenosColor8Render({.5f,.5f,.5f,.5f},0)==std::array<std::uint8_t,4>{128,128,128,128}));
    std::printf("XENOS_COLOR8_RENDER_PASS dyadic_cases=65537 roundtrip_changes=%u clear_semantics_unchanged=1\n",roundtripChanges);
}
