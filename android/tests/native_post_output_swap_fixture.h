#pragma once
#include "native_post_output_swap_wrapper.h"
namespace recomp::gpu::test {
inline std::array<std::uint8_t,NativePostOutputSwapWrapper::Bytes> postOutputSwapBytes(
        const NativeSwapCommand& command,unsigned scratch=0x30000,unsigned callback=0x82001000,
        unsigned counter=0x8000,unsigned event0=0x9000,unsigned event1=0xA000){
    std::array<std::uint8_t,NativePostOutputSwapWrapper::Bytes> bytes{};
    const auto put=[&](unsigned at,unsigned value){for(unsigned b=0;b<4;++b)bytes[at*4+b]=value>>(24-b*8);};
    const std::array<unsigned,10> prefix{0x5C8,0x20000,0xE00,1,0xE40,1,0xC0025800,0x80000003,counter,0xDEADBEEF};
    for(unsigned i=0;i<prefix.size();++i)put(i,prefix[i]);
    std::copy(command.bytes().begin(),command.bytes().end(),bytes.begin()+40);
    const auto context=command.identity().imagePhysical|0x208;
    const std::array<unsigned,55> tail{
        0xC0025800,0x80000003,counter+4,0xDEADBEEF,
        0xC0006000,0xFFFFFFFF,0x5C8,0x20000,0x1057C,callback,context,0x578,4,
        0xC0043C00,0x13,scratch+2,4,0xFFFFFFFF,0x100,
        0xC0043C00,0x13,scratch+22,context,0xFFFFFFFF,0x100,
        0xC0043C00,0x13,scratch+18,callback,0xFFFFFFFF,0x100,
        0xC0005400,4,0x45E,4,0xC0043C00,0x13,scratch+2,0,0xFFFFFFFF,0x100,0x57C,0x0BADF00D,
        0xC0003B00,0x7FFF,0xC0001000,0x77777777,
        0xC0025800,3,event0+2,0x12345678,0xC0025800,3,event1+2,0xABCDEF01};
    for(unsigned i=0;i<tail.size();++i)put(74+i,tail[i]);
    return bytes;
}
}
