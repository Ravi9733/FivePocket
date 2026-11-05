#pragma once
#include "native_display_output.h"
namespace recomp::gpu::test {
inline std::array<std::uint8_t,64> displayOutputBytes(unsigned event0=0x20000,unsigned event1=0x20004,
        unsigned value0=0x1234,unsigned value1=0xABCD) {
    const std::array<unsigned,16> words{0xC0022100,0x1E4E,0xFFFFFFFE,0,0xC0003B00,0x7FFF,
        0xC0001000,0x77777777,0xC0025800,3,event0|2u,value0,0xC0025800,3,event1|2u,value1};
    std::array<std::uint8_t,64> bytes{};
    for(unsigned i=0;i<16;++i)for(unsigned b=0;b<4;++b)bytes[i*4+b]=words[i]>>(24-b*8);
    return bytes;
}
}
