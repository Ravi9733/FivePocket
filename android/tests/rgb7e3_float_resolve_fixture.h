#pragma once
#include "native_rgb7e3_float_resolve.h"
#include <cassert>
#include <fstream>
#include <string>
#ifdef RECOMP_PRIVATE_RGB7E3_RESOLVE_CALL
#include RECOMP_PRIVATE_RGB7E3_RESOLVE_CALL
#endif
namespace rgb7e3_test {
using namespace recomp::gpu;
inline void put(std::vector<std::uint8_t>& b,unsigned at,unsigned v) {
    assert(at+4<=b.size());for(unsigned i=0;i<4;++i)b[at+i]=v>>(24-8*i);
}
inline unsigned word(const auto& b,unsigned at) {
    assert(at+4<=b.size());unsigned v=0;for(unsigned i=0;i<4;++i)v=v*256+b[at+i];return v;
}
// Literal words, deliberately NOT the planner's formulas. Row2's words are
// the captured T1 call's (hg739jnq: surface, state, flags, clear vector and
// destination header), equal to the ABI41 draw's bound target; row3's are the
// captured T2 call's (1jamjqig: hi-Z pitch96, tiled destination pitch96); row4
// is the captured flags0 T1 re-resolve (99jco_dd), row2's words without clear.
struct AuthoredRgb7e3Profile {unsigned width,height,surfaceInfo,colorInfo,extent,fetch0,fetch2,flags;};
inline constexpr AuthoredRgb7e3Profile AuthoredRgb7e3Profiles[]{
    {640,360,0x0A000280,0x302D0,0x09FC0B38,0x85000156,0x2CE27F,0},
    {320,180,0x05000140,0x302D0,0x04FC0598,0x82800156,0x16613F,0},
    {160,90,0x028000A0,0x30300,0x027C02C8,0x81400156,0x0B209F,0x100},
    {80,45,0x01800050,0x3030C,0x013C0160,0x80C00156,0x05804F,0x100},
    {160,90,0x028000A0,0x30300,0x027C02C8,0x81400156,0x0B209F,0}};
inline Gta5TileResolveCapture authoredProfile(const AuthoredRgb7e3Profile& p) {
    Gta5TileResolveCapture c;c.stack=0x64000000;c.arguments={0x10001000,p.flags,0x64000090,0x10002000,0,0,0,0x64000080,0x823889E8};
    c.state=std::vector<std::uint8_t>(13860);c.surfaces.emplace();
    auto& s=c.surfaces->surfaces[0];s.object=0x10003000;s.header=std::vector<std::uint8_t>(40);
    put(*s.header,24,p.surfaceInfo);put(*s.header,28,p.colorInfo);put(*s.header,36,p.extent);
    put(*c.state,12816,s.object);put(*c.state,10368,p.surfaceInfo);put(*c.state,10372,p.colorInfo);put(*c.state,11068,0xC00);
    c.destinationHeader=std::vector<std::uint8_t>(52);
    const std::array<unsigned,6> f{p.fetch0,0xA100005D,p.fetch2,0xD11,0,0x200};
    for(unsigned i=0;i<6;++i)put(*c.destinationHeader,28+4*i,f[i]);
    c.pointed.rectangle=std::vector<std::uint8_t>(16);put(*c.pointed.rectangle,8,p.width);put(*c.pointed.rectangle,12,p.height);
    // 82388720 frame+128 vector {0,0,0,1.0f}; captured for every call, but
    // consumed by 823C2738 only under flag0x100.
    c.pointed.color=std::vector<std::uint8_t>(16);put(*c.pointed.color,12,0x3F800000);
    c.pointed.stack92=std::vector<std::uint8_t>(4);c.pointed.stack100=std::vector<std::uint8_t>(4);return c;
}
inline Gta5TileResolveCapture authored(bool half=false) {return authoredProfile(AuthoredRgb7e3Profiles[half?1:0]);}
inline Gta5TileResolveCapture captured(const std::string& dir) {
#ifdef RECOMP_PRIVATE_RGB7E3_RESOLVE_CALL
    const auto read=[&](const std::string& key,unsigned size) {
        std::ifstream f(dir+"/native-resolve-failure-"+key+".bin",std::ios::binary|std::ios::ate);
        assert(f && f.tellg()==size);f.seekg(0);std::vector<std::uint8_t> b(size);
        assert(f.read(reinterpret_cast<char*>(b.data()),size));return b;
    };
    Gta5TileResolveCapture c;c.arguments=SavedArgs;c.stack=SavedStack;c.f1=SavedF1;
    c.state=read("state",13860);c.destinationHeader=read("destination-header",52);c.surfaces.emplace();
    for(unsigned i=0;i<5;++i) {
        auto& s=c.surfaces->surfaces[i];s.object=word(*c.state,12816+4*i);
        if(s.object)s.header=read("surface-"+std::to_string(i),40);
    }
    c.pointed.rectangle=read("rectangle",16);c.pointed.stack92=read("stack92",4);c.pointed.stack100=read("stack100",4);
    if(c.arguments[1]&0x100)c.pointed.color=read("clear-color",16); // Consumed only by a clearing copy.
    return c;
#else
    (void)dir;assert(false && "receipt-bound original call identity required");return {};
#endif
}
// General outer/inner bank/pipe addressing, independent of production's
// specialized32x32 macro/micro routine; eight-byte texels, explicit pitch.
inline unsigned offset(unsigned x,unsigned y,unsigned pitch=640) {
    assert(pitch && pitch%32==0);
    const unsigned outer=((y/32)*(pitch/32)+x/32)*64,inner=(y/2%8)*8+x%8;
    const unsigned v=(outer+inner)*8,bank=y/16%2,pipe=(x/8%4)^((y/8%2)*2);
    return (v&15)|((y%2)*16)|((v&16)*2)|(pipe*64)|((v&224u)*8)|((v&~255u)*16)|(bank*2048);
}
}
