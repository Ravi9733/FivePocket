#pragma once
#include "gta5_logical_tile_clear.h"
#include <cassert>

namespace recomp::gpu::test {
inline void tileWord(std::vector<std::uint8_t>& b,unsigned at,std::uint32_t v) {
    assert(at+4<=b.size());for(unsigned i=0;i<4;++i)b[at+i]=v>>(24-i*8);
}
inline std::uint32_t tileWord(const std::vector<std::uint8_t>& b,unsigned at) {
    assert(at+4<=b.size());return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|
        (std::uint32_t{b[at+2]}<<8)|b[at+3];
}
inline void tileFloat(std::vector<std::uint8_t>& b,unsigned at,float v) {
    std::uint32_t bits;std::memcpy(&bits,&v,4);tileWord(b,at,bits);
}
inline void clearRectangle(Gta5ClearCapture& c,const std::array<unsigned,4>& r) {
    c.rectangle=std::vector<std::uint8_t>(16);for(unsigned i=0;i<4;++i)tileWord(*c.rectangle,i*4,r[i]);
}
inline std::vector<std::uint8_t> authoredTileState() {
    std::vector<std::uint8_t> b(13864);b[11068]=0x21;
    for(unsigned i=0;i<5;++i){tileWord(b,12816+i*4,0x10020000+i*64);tileWord(b,13104+i*4,0x10020000+i*64);}
    tileWord(b,13100,0x10020000);tileWord(b,13124,2);
    for(unsigned i=0;i<8;++i)tileWord(b,13128+i*4,std::array<unsigned,8>{0,0,1280,384,0,384,1280,720}[i]);
    tileWord(b,13380,384);tileWord(b,13556,1280);tileWord(b,13560,720);tileWord(b,13564,1);
    for(unsigned i=0;i<6;++i)tileWord(b,10368+i*4,std::array<unsigned,6>{0x14000500,384,0x10000,768,1536,1152}[i]);
    return b;
}
// ALL resource headers and API arguments are AUTHORED. When a captured device
// state is supplied, this does not promote these headers to live evidence.
inline Gta5ClearCapture authoredLogicalClear(std::vector<std::uint8_t> state=authoredTileState()) {
    const auto layout=describeGta5DriverTiling(state);assert(layout);
    Gta5ClearCapture c;c.state=std::move(state);c.call.device=0x10000000;c.call.flags=0x3F;
    tileWord(*c.state,13084,UINT32_MAX);tileWord(*c.state,13088,0);tileWord(*c.state,12288,0);
    tileFloat(*c.state,13024,0);tileFloat(*c.state,13028,0);
    tileFloat(*c.state,13032,layout->width);tileFloat(*c.state,13036,layout->height);
    unsigned height=0;for(const auto& t:layout->tiles)height=std::max(height,t.local[3]);
    for(unsigned i=0;i<5;++i) {
        auto& s=c.surfaces[i];s.object=layout->boundTargets[i];s.tileCache=layout->targets[i];
        if(!s.object)continue;
        s.header=std::vector<std::uint8_t>(40);
        tileWord(*s.header,24,tileWord(*c.state,10368));
        tileWord(*s.header,28,tileWord(*c.state,i==4?10376:i==0?10372:10376+i*4));
        tileWord(*s.header,36,((layout->width-1)<<18)|((height-1)<<3));
    }
    clearRectangle(c,{0,0,layout->width,layout->height});
    c.color=std::vector<std::uint8_t>(16);
    for(unsigned i=0;i<4;++i)tileFloat(*c.color,i*4,std::array<float,4>{0.25f,0.5f,0.75f,1.f}[i]);
    const double depth=0.5;std::memcpy(&c.call.depthBits,&depth,8);c.call.stencil=37;
    return c;
}
}
