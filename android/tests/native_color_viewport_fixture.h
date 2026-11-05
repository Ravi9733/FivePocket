#pragma once
#include "native_color_viewport.h"
namespace recomp::gpu::test {
inline DrawCommandCandidate colorViewportDraw() {
    DrawCommandCandidate d;d.offsetWords=100;d.opcode=0x22;d.indexCount=4;d.primitive=6;
    d.predicate={{{{1,0,1,0}},15},true};
    const auto assign=[&](unsigned index,unsigned value){d.registers[index].bits.assign(value);};
    assign(0x2000,0x14000500);assign(0x200E,0);assign(0x200F,0x20002000);
    assign(0x2080,0);assign(0x2081,0);assign(0x2082,(720u<<16)|1280);
    assign(0x2100,0xFFFFFF);assign(0x2101,0);assign(0x2102,0);
    assign(0x210F,0x44200000);assign(0x2110,0x44200000); // +640, +640.
    assign(0x2111,0xC3B40000);assign(0x2112,0x43B40000); // -360, +360.
    assign(0x2113,0xBF800000);assign(0x2114,0x3F800000); // -1, +1.
    assign(0x2204,1u<<19);assign(0x2205,(1u<<16)|(1u<<15));assign(0x2206,(1u<<10)|63);assign(0x2208,4);
    assign(0x2284,0);assign(0x2292,0);assign(0x2293,0);
    assign(0x2301,0);assign(0x2302,4);
    assign(0x2303,0x40000000);assign(0x2304,0x3F800000);assign(0x2305,0x40000000);assign(0x2306,0x3F800000);
    return d;
}
}
