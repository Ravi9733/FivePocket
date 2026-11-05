#include "gta5_clear_capture.h"
#include <cassert>
#include <cstdio>
#include <map>
using namespace recomp::gpu;
using Bytes=std::vector<std::uint8_t>;
void word(Bytes& b,unsigned at,std::uint32_t v){for(unsigned c=0;c<4;++c)b[at+c]=v>>(24-8*c);}
int main(){
    Gta5ClearCall call{0x64000000,0x1004CD00,0x3F,0x64000080,0x64000090,7,0x8241B904,0x3FF0000000000000ULL};
    std::map<unsigned,Bytes> h,a,g;
    h[call.device]=Bytes(13864);h[0x10274D10]=Bytes(40);g[0xABF01000]=Bytes(40);
    word(h[call.device],12816,0x10274D10);word(h[call.device],12832,0xABF01000);
    word(h[call.device],13120,0xABF01000);h[call.device][11068]=0x20;
    a[call.rectangle]=Bytes(16);word(a[call.rectangle],8,128);word(a[call.rectangle],12,128);
    a[call.color]=Bytes(16);word(a[call.color],12,0x3F800000);
    for(unsigned i=0;i<12;++i){a[call.stack+i*256]=Bytes(4);word(a[call.stack+i*256],0,call.stack+(i+1)*256);
        a[call.stack+(i+1)*256-8]=Bytes(4);word(a[call.stack+(i+1)*256-8],0,0x82220000+i*4);}
    auto reader=[](auto& m){return [&m](unsigned at,unsigned n)->std::optional<Bytes>{auto it=m.find(at);
        return it!=m.end() && it->second.size()==n?std::optional{it->second}:std::nullopt;};};
    auto capture=[&]{return captureGta5ClearCall(call,reader(h),reader(g),reader(a));};
    const auto c=capture();assert(c.state && c.rectangle && c.color && c.frames.size()==8);
    assert(c.call.stencil==7 && c.call.depthBits==0x3FF0000000000000ULL);
    assert(c.surfaces[0].header && c.surfaces[4].header && !c.surfaces[1].header);
    assert(c.surfaces[4].tileCache==0xABF01000 && c.frames[0].savedLink==0x82220000);
    // One missing input must not erase other evidence or silently become zero.
    a.erase(call.color);g.clear();auto missing=capture();
    assert(!missing.color && missing.rectangle && missing.state && missing.frames.size()==8);
    assert(!missing.surfaces[4].header && missing.surfaces[4].object==0xABF01000);
    h.erase(call.device);missing=capture();assert(!missing.state && missing.rectangle && missing.frames.size()==8);
    word(a[call.stack],0,call.stack);assert(capture().frames.empty());
    word(a[call.stack],0,call.stack+65552);assert(capture().frames.empty());
    word(a[call.stack],0,call.stack+17);assert(capture().frames.empty());
    call.rectangle=UINT32_MAX-3;assert(!capture().rectangle);
    call.color=0x64000094;a[call.color]=Bytes(16);assert(!capture().color);
    std::puts("GTA5_CLEAR_CAPTURE_PASS all bindings,40-byte headers,depth/stencil,owned backchain,independent missing evidence");
}
