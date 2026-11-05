#include "native_geometry_draw_state.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    unsigned cases=0;
    for(unsigned selector:{0x10310A05u,0x10310A06u,0x10610D0Au}) {
        const bool complex=selector==0x10610D0A;
        Gta5InlineDraw draw;draw.deviceState.resize(13860);draw.primitive=4;draw.stride=24;
        draw.scissorRegisters=std::array<unsigned,3>{0,0,0x800080};
        const auto set=[&](unsigned reg,unsigned v){const auto at=*gta5DrawRegisterOffset(reg);
            for(unsigned c=0;c<4;++c)draw.deviceState[at+c]=v>>(24-8*c);};
        const unsigned state[][2]{
            {0x2000,0x20100A0},{0x2001,0x10000},{0x2002,0x10020},{0x200E,0},{0x200F,0x20002000},
            {0x2100,0xFFFFFF},{0x2101,0},{0x2102,0},{0x2104,15},{0x210C,0xFFFF00},{0x210D,complex?0xFFFFu:0xFFFF01u},
            {0x210F,0x42800000},{0x2110,0x42800000},{0x2111,0xC2800000},{0x2112,0x42800000},
            {0x2113,complex?0u:0x3F800000u},{0x2114,complex?0x3F800000u:0u},
            {0x2180,selector},{0x2181,complex?4u:8u},{0x2182,complex?0x7F007Fu:0x7000Fu},
            {0x2200,complex?0x700263u:0x708367u},{0x2201,0x10001},{0x2202,0xAA000007},{0x2203,0x13},{0x2204,0x80000},
            {0x2205,complex?0x18000u:0x18002u},{0x2206,0x43F},{0x2208,4},{0x2209,0x10001},{0x220A,0x10001},{0x220B,0x10001},
            {0x2300,0x400},{0x2301,0x8001},{0x2302,4},{0x2303,0x40000000},{0x2304,0x3F800000},
            {0x2305,0x40000000},{0x2306,0x3F800000},{0x2307,0xFF000},{0x2308,0xFF100},{0x2312,0xFFFF}};
        for(const auto& r:state)set(r[0],r[1]);
        NativeTranslatedDrawProgram p;p.abiVersion=13;p.indexed=true;p.geometryDepth20e4=true;p.geometryColor8Format=1;
        p.stride=24;p.geometryCentroidMask=complex?0:0xFFF8;
        p.geometrySampleDepth=true;
        const auto result=planNativeGeometryDrawState(draw,p);assert(result && result->samples==2);
        assert(result->cull==(complex?0u:2u) && result->minDepth==(complex?1.f:0.f));++cases;
        for(unsigned r:{0x2104u,0x2183u,0x2184u,0x2201u,0x2202u,0x2203u,0x2204u,0x2205u,0x2206u,0x2207u,0x2208u,0x2300u,0x2302u,0x2312u}) {
            const auto before=*gta5DrawRegister(draw,r);set(r,before^0x80000000u);
            assert(!planNativeGeometryDrawState(draw,p));set(r,before);++cases;
        }
        p.geometryCentroidMask^=1;assert(!planNativeGeometryDrawState(draw,p));p.geometryCentroidMask^=1;
        p.abiVersion=12;assert(!planNativeGeometryDrawState(draw,p));p.abiVersion=13;
        p.geometrySampleDepth=false;assert(!planNativeGeometryDrawState(draw,p));p.geometrySampleDepth=true;++cases;
        set(0x2000,0x20200A0);assert(!planNativeGeometryDrawState(draw,p));set(0x2000,0x20100A0);
        set(0x2113,0x7FC00000);assert(!planNativeGeometryDrawState(draw,p));cases+=4;
    }
    std::printf("NATIVE_GEOMETRY_DRAW_STATE_PASS cases=%u no_ownership_claim=1\n",cases);
}
