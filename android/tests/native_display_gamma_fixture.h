#pragma once
#include "native_display_gamma.h"
namespace recomp::gpu::test {
inline std::array<std::uint8_t,NativeDisplayGammaWrapper::Bytes> gammaWrapperBytes(
        const NativeDisplayGammaRamp::Packed& packed,std::uint32_t frontbuffer=0x100000) {
    std::array<std::uint8_t,NativeDisplayGammaWrapper::Bytes> bytes{};
    const auto put=[&](unsigned at,std::uint32_t v){for(unsigned b=0;b<4;++b)bytes[at*4+b]=v>>(24-b*8);};
    const std::array<std::uint32_t,23> prefix{0xC0022100,0x1841,0xFFFFF8FF,0,0x1930,0,0x1844,frontbuffer,
        0x1838,0,0xC0043C00,3,0x1973,0,1,0x100,0x1852,1,0x11921,0,0,0x1927,7};
    for(unsigned i=0;i<prefix.size();++i)put(i,prefix[i]);
    for(unsigned i=0;i<256;++i){const std::array<std::uint32_t,9> entry{0x1925,packed[i],0xC0054500,7,0x1925,packed[i],0xFFFFFFFF,0x1922,i+1};
        for(unsigned j=0;j<entry.size();++j)put(23+i*9+j,entry[j]);}
    const std::array<std::uint32_t,12> tail{0xC0003B00,0x7FFF,0xC0001000,0x77777777,0xC0025800,3,0x20002,0x1234,0xC0025800,3,0x20006,0xABCD};
    for(unsigned i=0;i<tail.size();++i)put(2327+i,tail[i]);
    return bytes;
}
}
