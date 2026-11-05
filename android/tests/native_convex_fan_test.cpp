#include "native_convex_fan.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    Gta5InlineDraw d;d.primitive=5;d.stride=16;d.deviceState.resize(13860);
    d.scissorRegisters=std::array<std::uint32_t,3>{0,0,0x800080};
    const auto set=[&](unsigned r,unsigned v){auto at=*gta5DrawRegisterOffset(r);for(unsigned i=0;i<4;++i)d.deviceState[at+i]=v>>(24-8*i);};
    for(const auto r:std::array<std::array<unsigned,2>,9>{{{0x2204,0x80000},{0x2205,0x18000},{0x2206,0x43F},
        {0x2208,4},{0x210F,0x42800000},{0x2110,0x42800000},{0x2111,0xC2800000},{0x2112,0x42800000},{0x2302,4}}})set(r[0],r[1]);
    using Point=std::array<float,4>;
    std::vector<Point> square{{0,0,.5,1},{-.5,-.5,.5,1},{.5,-.5,.5,1},{.5,.5,.5,1},{-.5,.5,.5,1},{-.5,-.5,.5,1}};
    const auto check=[&](const std::vector<Point>& p){d.vertexCount=p.size();d.vertices.resize(p.size()*16);return nativeSingleCoverageConvexFan(d,p);};
    assert(check(square));auto reverse=square;std::reverse(reverse.begin()+1,reverse.end());assert(check(reverse));
    auto outside=square;outside[0][0]=.75;assert(!check(outside));
    auto open=square;open.back()[0]=-.499;assert(!check(open));
    auto folded=square;std::swap(folded[2],folded[3]);assert(!check(folded));
    auto flat=square;flat[2][1]=.5;assert(!check(flat));
    auto invalid=square;invalid[2][3]=-1;assert(!check(invalid));
    invalid=square;invalid[2][2]=2;assert(!check(invalid));
    invalid=square;invalid[2][0]=std::numeric_limits<float>::quiet_NaN();assert(!check(invalid));
    for(auto& p:invalid)p={0,0,.5,1};assert(!check(invalid));
    // A consistently turning pentagram must fail the all-vertices half-planes.
    std::vector<Point> star{{0,0,.5,1}};
    for(unsigned i=0;i<=5;++i){const unsigned j=(i*2)%5;const double angle=j*6.283185307179586/5;
        star.push_back({float(.5*std::cos(angle)),float(.5*std::sin(angle)),.5,1});}
    assert(!check(star));
    d.scissorRegisters=std::array<std::uint32_t,3>{0,0,0x200020};assert(!check(square));
    d.scissorRegisters=std::array<std::uint32_t,3>{0,0,0};assert(!check(square));
    std::puts("NATIVE_CONVEX_FAN_PASS convex_both_windings=1 star_overlap_and_open_rejected=1");
}
