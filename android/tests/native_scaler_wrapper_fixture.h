#pragma once
#include "native_scaler_wrapper.h"
#include <algorithm>

namespace recomp::gpu::test {
// Synthetic packet builder using supplied test addresses, not a game capture.
inline std::array<std::uint8_t,NativeScalerWrapper::Bytes> scalerWrapperBytes(
        const NativeScalerCommand& command,std::uint32_t scratch,std::uint32_t callback,std::uint32_t context,
        std::array<std::uint32_t,2> events={0x30000,0x31000},std::array<std::uint32_t,2> values={0x123,0xABC}) {
    std::array<std::uint8_t,NativeScalerWrapper::Bytes> bytes{};
    unsigned cursor=0;
    const auto put=[&](std::uint32_t value) { for (unsigned b=0;b<4;++b) bytes[cursor*4+b]=value>>(24-b*8);++cursor; };
    const auto reg=[&](unsigned address,unsigned value) { put(address);put(value); };
    const auto wait=[&](unsigned slot,unsigned value) {
        put(0xC0043C00);put(0x13);put(scratch+slot*4+2);put(value);put(0xFFFFFFFF);put(0x100);
    };
    put(0xC0006000);put(0xFFFFFFFF);reg(0x579,1);wait(1,1);reg(0x5C8,0x20000);
    put(0x1057C);put(callback);put(context);reg(0x578,4);wait(0,4);wait(5,context);wait(4,callback);
    put(0xC0005400);put(4);reg(0x45E,4);wait(0,0);reg(0x57C,0x0BADF00D);wait(1,0);
    reg(0x1973,0x10000);reg(0x1964,1);reg(0x1973,0);
    put(0xC0043C00);put(3);put(0x1973);put(0);put(1);put(0x100);reg(0x1852,0);reg(0x1838,1);
    std::copy(command.bytes().begin(),command.bytes().end(),bytes.begin()+cursor*4);cursor+=NativeScalerCommand::Words;
    put(0xC0003B00);put(0x7FFF);put(0xC0001000);put(0x77777777);
    for (unsigned i=0;i<2;++i) { put(0xC0025800);put(3);put(events[i]+2);put(values[i]); }
    return bytes;
}
}
