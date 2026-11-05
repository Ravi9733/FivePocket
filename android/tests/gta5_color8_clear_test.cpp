#include "gta5_color8_clear.h"
#include <cassert>
#include <cstdio>
#include <limits>
using namespace recomp::gpu;
int main(){
    for(unsigned format:{0u,1u}) {
        assert((*packGta5Color8Clear({0,0,0,0},format)==std::array<std::uint8_t,4>{0,0,0,0}));
        assert((*packGta5Color8Clear({1,1,1,1},format)==std::array<std::uint8_t,4>{255,255,255,255}));
        for(unsigned i=0;i<256;++i) {
            const float f=format?xenosPwlGammaByteToLinear(i):float(i)/255.f;
            const auto p=packGta5Color8Clear({f,f,f,float(i)/255.f},format);
            assert(p && (*p==std::array<std::uint8_t,4>{std::uint8_t(i),std::uint8_t(i),std::uint8_t(i),std::uint8_t(i)}));
        }
        for(float bad:{-.1f,1.1f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
            assert(!packGta5Color8Clear({0,bad,0,1},format));
    }
    assert(!packGta5Color8Clear({0,0,0,1},2));
    const auto half=packGta5Color8Clear({.5f,.5f,.5f,.5f},1);
    assert(half && (*half==std::array<std::uint8_t,4>{192,192,192,128}));
    for(unsigned i=0;i<1024;++i) {
        const float f=float(i)/1023.f;
        const auto value=(*packGta5Color8Clear({f,0,0,1},1))[0];
        const unsigned expected=i<64?i:i<128?(i/2)+32:i<512?(i/4)+64:(i/8)+128;
        assert(value==expected);
        if(i)assert(value>=(*packGta5Color8Clear({float(i-1)/1023.f,0,0,1},1))[0]);
    }
    // The original's nearest-even step is observable, not interchangeable
    // with truncating the input directly as some public gamma references do.
    assert((*packGta5Color8Clear({.75f/1023.f,1.5f/1023.f,2.5f/1023.f,1},1)==
        std::array<std::uint8_t,4>{1,2,2,255}));
    std::puts("GTA5_COLOR8_CLEAR_PASS UNORM/PWL levels, nearest-even boundaries, alpha, invalid input");
}
