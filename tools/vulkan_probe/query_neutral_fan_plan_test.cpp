#include "query_neutral_fan_fixture.h"
#include <cstdio>
using namespace recomp::gpu;
int main(int argc,char** argv) {
    assert(argc==2);QueryNeutralFanFixture f(argv[1]);const auto& p=f.program;
    const auto valid=[&](const auto& d){return planNativeQueryNeutralFan(p,d,p.vertexIdentity,p.pixelIdentity);};
    assert(valid(f.draw));unsigned negatives=0;
    // Every admitted raster-control bit must remain exact. Unrelated padding
    // is not claimed to be a register or counted as a new compatibility case.
    const unsigned regs[]={0x2000,0x2001,0x2002,0x2104,0x210D,0x210F,0x2110,0x2111,0x2112,
        0x2113,0x2114,0x2180,0x2181,0x2182,0x2183,0x2184,0x2200,0x2201,0x2202,0x2203,
        0x2204,0x2205,0x2206,0x2207,0x2208,0x2284,0x2292,0x2293,0x2300,0x2301,0x2302,
        0x2307,0x2308,0x2312,0x231C,0x2324,0x2325};
    for(auto reg:regs)for(unsigned bit=0;bit<32;++bit) {
        auto d=f.draw;const auto at=*gta5DrawRegisterOffset(reg);
        d.deviceState[at+3-bit/8]^=1u<<(bit%8);assert(!valid(d));++negatives;
    }
    for(unsigned at:{12408u,11068u,11072u,12497u,12498u,12563u,12424u,12428u,12433u}) {
        auto d=f.draw;d.deviceState[at]^=1;assert(!valid(d));++negatives;
    }
    auto d=f.draw;d.vertices.pop_back();assert(!valid(d));++negatives;
    d=f.draw;d.vertexCount=2;assert(!valid(d));++negatives;
    d=f.draw;d.primitive=4;assert(!valid(d));++negatives;
    d=f.draw;d.vertexShader^=4;assert(!valid(d));++negatives;
    d=f.draw;d.pixelShader^=4;assert(!valid(d));++negatives;
    d=f.draw;d.scissorRegisters.reset();assert(!valid(d));++negatives;
    auto v=p.vertexIdentity;v[0]^=1;assert(!planNativeQueryNeutralFan(p,f.draw,v,p.pixelIdentity));++negatives;
    auto ps=p.pixelIdentity;ps[0]^=1;assert(!planNativeQueryNeutralFan(p,f.draw,p.vertexIdentity,ps));++negatives;
    assert(negatives==1201);
    std::printf("QUERY_NEUTRAL_FAN_PLAN_PASS current_capture=1 negatives=%u gpu_execution=0 query_completion=0\n",negatives);
}
