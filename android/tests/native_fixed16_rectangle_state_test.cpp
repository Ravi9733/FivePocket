#include "native_fixed16_draw_state.h"
#include "native_fixed16_alias_plan.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
static void put(std::vector<std::uint8_t>& bytes,unsigned offset,std::uint32_t value) {
    for(unsigned i=0;i<4;++i)bytes.at(offset+i)=value>>(24-8*i);
}
static void reg(Gta5InlineDraw& draw,unsigned r,unsigned value) {
    for(const auto& range:std::array<std::array<unsigned,3>,5>{{{0x2000,16,10368},{0x2100,21,10444},
            {0x2180,5,10528},{0x2200,12,10548},{0x2300,38,10680}}})
        if(r>=range[0] && r<range[0]+range[1]) {put(draw.deviceState,range[2]+4*(r-range[0]),value);return;}
    assert(false);
}
int main() {
    Gta5Fixed16ClearPlan original;original.object=0x1000;original.width=original.height=256;
    original.pitch=320;original.baseTile=64;original.tileCount=64;original.samples=1;original.format=4;
    original.depth.emplace();auto& z=*original.depth;
    z.object=0x2000;z.width=z.height=256;z.pitch=320;z.baseTile=0;z.tileCount=64;z.samples=1;z.format=1;
    z.value=*planExactXenosFloat24Clear(0.f,0);
    std::vector<std::uint8_t> color(40),depth(40);
    put(color,24,0x04000140);put(color,28,0x40040);put(color,36,0x03FC07F8);
    put(depth,24,0x04000140);put(depth,28,0x10000);put(depth,32,0x11);put(depth,36,0x03FC07F8);
    Gta5BoundSurfaceCapture bindings;
    bindings.surfaces[0].object=0x3000;bindings.surfaces[0].header=color;
    bindings.surfaces[4].object=z.object;bindings.surfaces[4].header=depth;
    const auto alias=planNativeFixed16ColorAlias(original,color,depth,bindings);
    assert(alias && alias->object==0x3000 && nativeFixed16SameStorageRetag(original,*alias));
    auto altered=*alias;altered.baseTile++;assert(!nativeFixed16SameStorageRetag(original,altered));
    altered=*alias;altered.depth->object++;assert(!nativeFixed16SameStorageRetag(original,altered));
    altered=*alias;altered.color++;assert(!nativeFixed16SameStorageRetag(original,altered));
    for(unsigned field=0;field<7;++field) {
        auto bad=bindings;
        switch(field) {
        case 0:bad.surfaces[0].object=z.object;break;
        case 1:bad.surfaces[0].object=0x3001;break;
        case 2:put(*bad.surfaces[0].header,28,0x40041);break;
        case 3:put(*bad.surfaces[0].header,36,0x03FC07F0);break;
        case 4:bad.surfaces[4].object=0x4000;break;
        case 5:(*bad.surfaces[4].header)[0]^=1;break;
        case 6:bad.surfaces[1].object=0x5000;break;
        }
        assert(!planNativeFixed16ColorAlias(original,color,depth,bad));
    }
    NativeTranslatedDrawProgram p;p.abiVersion=19;p.fixed16RectangleDepth=true;p.geometryDepth20e4=true;
    p.stride=24;p.interpolators=1;p.textureSlot=2;
    Gta5InlineDraw draw;draw.primitive=8;draw.vertexCount=3;draw.stride=24;
    draw.vertices.resize(72);draw.deviceState.resize(13860);draw.scissorRegisters=std::array<unsigned,3>{0,0,0x1000100};
    put(draw.deviceState,12816,0x3000);put(draw.deviceState,12832,z.object);
    constexpr unsigned registers[][2]{
        {0x2000,0x4000140},{0x2001,0x40040},{0x2002,0x10000},{0x2003,0x300},{0x200E,0},{0x200F,0x20002000},
        {0x2100,0xFFFFFF},{0x2101,0},{0x2102,0},{0x2104,15},{0x210C,0xFFFF00},{0x210D,0xFFFFFF},
        {0x210F,0x43000000},{0x2110,0x43000000},{0x2111,0xC3000000},{0x2112,0x43000000},
        {0x2113,0xBF800000},{0x2114,0x3F800000},{0x2180,0x18010101},{0x2181,4},{0x2182,0x10001},{0x2183,0},{0x2184,0},
        {0x2200,0x700776},{0x2201,0x10001},{0x2202,0xAA000007},{0x2203,0x11},{0x2204,0x80000},
        {0x2205,0x19802},{0x2206,0x43F},{0x2207,0},{0x2208,4},{0x2209,0x10001},{0x220A,0x10001},{0x220B,0x10001},
        {0x2300,0x400},{0x2301,0},{0x2302,4},{0x2303,0x40000000},{0x2304,0x3F800000},
        {0x2305,0x40000000},{0x2306,0x3F800000},{0x2307,0xFF000},{0x2308,0xFF100},{0x2312,0xFFFF}};
    for(const auto& r:registers)reg(draw,r[0],r[1]);
    auto state=planNativeFixed16RectangleState(draw,p,*alias,bindings);
    assert(state && state->depth.depthCompare==7 && state->depth.depthWrite && !state->depth.stencilEnable && state->cull==0);
    assert(!planNativeFixed16RectangleState(draw,p,original,bindings));
    assert(!planNativeFixed16DrawState(draw,p,*alias,bindings));
    for(unsigned field=0;field<9;++field) {
        auto bad=draw;
        switch(field) {
        case 0:bad.primitive=4;break;
        case 1:bad.vertexCount=4;bad.vertices.resize(96);break;
        case 2:reg(bad,0x2200,0x700766);break;
        case 3:bad.deviceState[11068]=1;break;
        case 4:reg(bad,0x2104,3);break;
        case 5:put(bad.deviceState,12816,original.object);break;
        case 6:bad.vertices.pop_back();break;
        case 7:reg(bad,0x2180,0x18110202);break;
        case 8:bad.scissorRegisters=std::array<unsigned,3>{0,0,0x800080};break;
        }
        assert(!planNativeFixed16RectangleState(bad,p,*alias,bindings));
    }
    p.abiVersion=17;p.fixed16RectangleDepth=false;p.geometryFixed16GradientDepth=true;p.indexed=true;p.textureSlot=UINT32_MAX;
    draw.primitive=4;reg(draw,0x2200,0x700766);
    assert(planNativeFixed16DrawState(draw,p,*alias,bindings)->cull==2);
    assert(!planNativeFixed16RectangleState(draw,p,*alias,bindings));
    std::puts("FIXED16_RECTANGLE_STATE_PASS authored_alias_to_state=1 depth_always=1 indexed_ge_preserved=1 rejection_controls=16 gpu=0");
}
