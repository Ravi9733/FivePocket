#pragma once
#include "native_color_render_state.h"
#include "native_color_viewport_fixture.h"
namespace recomp::gpu::test {
inline DrawCommandCandidate colorRenderStateDraw(unsigned role){
    auto draw=colorViewportDraw();
    const auto assign=[&](unsigned index,unsigned value){draw.registers[index].bits.assign(value);};
    assign(0x2001,(12u<<16)|720);assign(0x2104,15);
    assign(0x2180,0x10110003u|((role?2u:3u)<<8));assign(0x2181,8);assign(0x2182,0x10003);
    assign(0x2183,0);assign(0x2184,0);assign(0x2200,0x700770);
    assign(0x2201,6u|(7u<<8)|(6u<<16)|(7u<<24));assign(0x2202,0xAA000007);assign(0x2203,0x10);
    assign(0x2307,255u<<12);assign(0x2308,(255u<<12)|256);assign(0x2312,0xFFFF);
    return draw;
}
}
