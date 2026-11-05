#pragma once
#include "native_composite_rectangle_plan.h"
#include <cassert>
#include <cstring>
namespace recomp::gpu::test {
// TEST ONLY:823A64D8's 23-band progression, not23 live captures. Input is a
// fresh copy of the first captured state/binding on every invocation.
//823AB070 copies previous depth+28 to RB_DEPTH_INFO, depth+32 bit0 to
// device11070 bit5;823CC8F8/8B0/8D0 request enable1/write0/compare7, then
//823BB650 derives paired HiZ. The exact second capture corroborates these.
inline void deriveCompositeRestoreBand(Gta5InlineDraw& draw,
        Gta5BoundSurfaceCapture& bound,unsigned band,bool authoredPartialTail=false) {
    assert(band<23 && bound.surfaces[0].header && !bound.surfaces[4].object);
    const auto put=[](auto& b,unsigned at,std::uint32_t v) {
        for(unsigned c=0;c<4;++c)b[at+c]=v>>(24-8*c);
    };
    const auto base=band*32;
    put(draw.deviceState,10372,base);put(*bound.surfaces[0].header,28,base);
    const float textureY=static_cast<float>(base);
    std::memcpy(&draw.vertexConstants[923],&textureY,4);
    if(!band)return;
    bound.surfaces[4].object=0x640afa10;put(draw.deviceState,12832,0x640afa10);
    bound.surfaces[4].header=*bound.surfaces[0].header;
    put(*bound.surfaces[4].header,28,0x10000+base-32);
    // The descriptor's HiZ offset starts0 and advances80 32x16 blocks for a
    //1280x32 band (823A64D8 r16).82370998 encodes it at bit17. Separately
    // checked by executing that original constructor, not by this formula.
    const auto hizAddress=((band-1)*80u)<<17;
    put(*bound.surfaces[4].header,32,hizAddress|0x11u);
    put(draw.deviceState,10376,0x10000+base-32);
    draw.deviceState[11070]|=0x20;
    put(draw.deviceState,12308,1);
    put(draw.deviceState,*gta5DrawRegisterOffset(0x2200),0x700772);
    put(draw.deviceState,*gta5DrawRegisterOffset(0x2203),hizAddress|0x715u);
    // Original823A64D8 reuses virtual+20 (the WIDTH getter) for its remainder.
    //1280 gives32, so live runyzxy4wse writes all32 rows even on band22.
    // The half-height tail is a SEPARATE authored safety case, not live state.
    if(authoredPartialTail && band==22){put(draw.vertices,4,0);put(draw.vertices,28,0);}
}
}
