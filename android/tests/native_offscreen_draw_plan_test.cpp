#include "native_offscreen_draw_plan.h"
#include <cassert>
#include <cstdio>
#include <limits>
using namespace recomp::gpu;
void word(std::vector<std::uint8_t>& b,unsigned at,std::uint32_t value) {
    assert(at+4<=b.size());for(unsigned i=0;i<4;++i)b[at+i]=value>>(24-8*i);
}
std::uint32_t bits(float value){std::uint32_t result;std::memcpy(&result,&value,4);return result;}
int main() {
    Gta5InlineDraw d;d.primitive=4;d.vertexCount=6;d.stride=16;d.vertices.resize(96);
    NativeAffinePositionXY affine{{0,4},{10,10},{11,11}};
    d.vertexConstants[10]=bits(2);d.vertexConstants[11]=bits(-1);
    unsigned accepted=0;
    for(unsigned pattern=0;pattern<4096;++pattern) {
        for(unsigned i=0;i<6;++i) {
            const auto corner=(pattern>>(2*i))&3;
            word(d.vertices,i*16,bits(float(corner&1)));word(d.vertices,i*16+4,bits(float(corner>>1)));
        }
        accepted+=nativeAffineFullRectangle(d,affine);
    }
    assert(accepted==144); // 2 diagonals * 2 triangle orders * 6 * 6 permutations.
    const unsigned corners[]{0,3,2,0,1,3};
    for(unsigned i=0;i<6;++i) {
        word(d.vertices,i*16,bits(float(corners[i]&1)));word(d.vertices,i*16+4,bits(float(corners[i]>>1)));
    }
    assert(nativeAffineFullRectangle(d,affine));
    const auto validVertices=d.vertices;
    for(float bad:{.25f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        word(d.vertices,0,bits(bad));assert(!nativeAffineFullRectangle(d,affine));
    }
    d.vertices=validVertices;
    auto badAffine=affine;badAffine.offsetBytes[0]=UINT32_MAX;assert(!nativeAffineFullRectangle(d,badAffine));
    badAffine=affine;badAffine.scaleWords[1]=1024;assert(!nativeAffineFullRectangle(d,badAffine));
    d.vertexCount=5;assert(!nativeAffineFullRectangle(d,affine));d.vertexCount=6;
    d.deviceState.resize(13860);d.scissorRegisters=std::array<std::uint32_t,3>{0,0,(32<<16)|64};
    const auto reg=[&](unsigned r,unsigned v){word(d.deviceState,*gta5DrawRegisterOffset(r),v);};
    constexpr unsigned state[][2]{
        {0x2000,80},{0x2001,17},{0x200E,0},{0x200F,0x20002000},
        {0x2100,0xFFFFFF},{0x2101,0},{0x2102,0},{0x2104,15},
        {0x2113,0xBF800000},{0x2114,0x3F800000},
        {0x2180,0x10010100},{0x2181,4},{0x2182,0x10001},{0x2183,0},{0x2184,0},
        {0x2200,0x700770},{0x2201,0x10001},{0x2202,0xAA000007},{0x2203,0x10},
        {0x2204,0x80000},{0x2205,0x18000},{0x2206,0x43F},{0x2208,4},
        {0x2284,0},{0x2292,0},{0x2293,0},{0x2301,0},{0x2302,4},
        {0x2303,0x40000000},{0x2304,0x3F800000},{0x2305,0x40000000},{0x2306,0x3F800000},
        {0x2307,0xFF000},{0x2308,0xFF100},{0x2312,0xFFFF}};
    for(const auto& r:state)reg(r[0],r[1]);
    reg(0x210F,bits(32));reg(0x2110,bits(32));reg(0x2111,bits(-16));reg(0x2112,bits(16));
    word(d.deviceState,12816,0x1000);
    Gta5BoundSurfaceCapture bound;bound.surfaces[0].object=0x1000;
    bound.surfaces[0].header=std::vector<std::uint8_t>(32);
    word(*bound.surfaces[0].header,24,80);word(*bound.surfaces[0].header,28,17);
    NativeTranslatedDrawProgram p;p.abiVersion=5;p.directTriangles=true;p.affinePositionXY=affine;
    p.stride=16;p.interpolators=1;p.textureSlot=2;p.textureSlots={2};d.textures.resize(1);d.textures[0].slot=2;
    auto plan=planNativeOffscreenDraw(d,p,bound);assert(plan);
    p.explicitLodRectangle=true;assert(!planNativeOffscreenDraw(d,p,bound));p.explicitLodRectangle=false;
    assert(((*plan)==NativeOffscreenDrawPlan{0x1000,64,32,80,17,2}));
    assert(!validateNativeTranslatedDraw(d,p)); // Never silently admitted as UI.
    const auto validState=d.deviceState;
    for(const auto& r:state) {
        if(r[0]==0x2302)continue; // Both documented pixel-center modes supported.
        reg(r[0],r[1]^1);assert(!planNativeOffscreenDraw(d,p,bound));reg(r[0],r[1]);
    }
    reg(0x2302,5);assert(planNativeOffscreenDraw(d,p,bound));reg(0x2302,6);assert(!planNativeOffscreenDraw(d,p,bound));
    d.deviceState=validState;d.deviceState[11068]=0x10;assert(!planNativeOffscreenDraw(d,p,bound));
    d.deviceState[11068]=0x20;assert(planNativeOffscreenDraw(d,p,bound));
    word(d.deviceState,13104,0x1000);bound.surfaces[0].tileCacheObject=0x1000;
    assert(!planNativeOffscreenDraw(d,p,bound));d.deviceState=validState;bound.surfaces[0].tileCacheObject=0;
    auto invalid=bound;invalid.surfaces[0].header.reset();assert(!planNativeOffscreenDraw(d,p,invalid));
    invalid=bound;invalid.surfaces[0].header->pop_back();assert(!planNativeOffscreenDraw(d,p,invalid));
    invalid=bound;invalid.surfaces[1].object=0x2000;word(d.deviceState,12820,0x2000);
    assert(!planNativeOffscreenDraw(d,p,invalid));d.deviceState=validState;
    for(unsigned color:{0x10000u,0x800u,2047u}) {
        invalid=bound;word(*invalid.surfaces[0].header,28,color);reg(0x2001,color);
        assert(!planNativeOffscreenDraw(d,p,invalid));
    }
    d.deviceState=validState;invalid=bound;word(*invalid.surfaces[0].header,24,0x10050);reg(0x2000,0x10050);
    assert(!planNativeOffscreenDraw(d,p,invalid));d.deviceState=validState;
    p.abiVersion=4;assert(!planNativeOffscreenDraw(d,p,bound));p.abiVersion=5;
    p.affinePositionXY.reset();assert(!planNativeOffscreenDraw(d,p,bound));p.affinePositionXY=affine;
    reg(0x210F,bits(31));assert(!planNativeOffscreenDraw(d,p,bound));d.deviceState=validState;
    assert(d.vertices==validVertices && planNativeOffscreenDraw(d,p,bound));
    std::puts("OFFSCREEN_PLAN_PASS rectangle_patterns=4096 accepted=144 negative_state_ownership_geometry=1 guest_writes=0");
}
