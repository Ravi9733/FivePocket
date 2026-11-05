#include "native_gamma_fan_state.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    Gta5InlineDraw draw;draw.primitive=5;draw.vertexCount=6;draw.stride=16;draw.vertices.resize(96);draw.deviceState.resize(13860);
    draw.scissorRegisters=std::array<std::uint32_t,3>{0,0,0x800080};
    const auto put=[](auto& b,unsigned at,unsigned v){for(unsigned i=0;i<4;++i)b[at+i]=v>>(24-8*i);};
    const auto set=[&](unsigned r,unsigned v){put(draw.deviceState,*gta5DrawRegisterOffset(r),v);};
    constexpr unsigned regs[][2]{{0x2000,0x20100A0},{0x2001,0x10000},{0x2002,0x10020},{0x200F,0x20002000},{0x2100,0xFFFFFF},
        {0x2104,15},{0x210C,0xFFFF00},{0x210D,0xFFFF},{0x210F,0x42800000},{0x2110,0x42800000},{0x2111,0xC2800000},{0x2112,0x42800000},
        {0x2114,0x3F800000},{0x2180,0x10010104},{0x2181,4},{0x2182,0x10001},{0x2200,0x700263},{0x2201,0x1010A},
        {0x2202,0xAA000007},{0x2203,0x13},{0x2204,0x80000},{0x2205,0x18000},{0x2206,0x43F},{0x2208,4},
        {0x2301,0x8001},{0x2302,4},{0x2303,0x40000000},{0x2304,0x3F800000},{0x2305,0x40000000},{0x2306,0x3F800000},
        {0x2307,0xFF000},{0x2308,0xFF100},{0x2312,0xFFFF}};
    for(const auto& r:regs)set(r[0],r[1]);
    Gta5Color8ClearPlan plan{0x1000,128,128,160,0,32,2,1,{0,0,0,255},{}};
    plan.depth=Gta5Depth24ClearPlan{0x2000,128,128,160,32,32,2,1,*planExactXenosFloat24Clear(0,255)};
    Gta5BoundSurfaceCapture bindings;
    for(unsigned slot:{0u,4u}) {
        auto& b=bindings.surfaces[slot];b.object=slot?plan.depth->object:plan.object;b.header=std::vector<std::uint8_t>(40);
        put(*b.header,24,0x20100A0);put(*b.header,28,slot?0x10020:0x10000);put(*b.header,36,(127u<<18)|(127u<<3));
        put(draw.deviceState,12816+4*slot,b.object);
    }
    NativeTranslatedDrawProgram program;program.abiVersion=7;program.color8SourceClamped=true;program.stride=16;program.textureSlot=UINT32_MAX;program.interpolators=1;
    const auto check=[&]{return nativeGammaFanState(draw,program,plan,bindings);};assert(check());
    for(const auto& r:regs){set(r[0],r[1]^1);assert(!check());set(r[0],r[1]);}
    program.color8SourceClamped=false;assert(!check());program.color8SourceClamped=true;
    for(unsigned abi:{4u,5u,6u,8u}){program.abiVersion=abi;assert(!check());}program.abiVersion=7;
    program.textureSlot=0;assert(!check());program.textureSlot=UINT32_MAX;
    auto good=plan;plan.depth.reset();assert(!check());plan=good;
    plan.depth->value=*planExactXenosFloat24Clear(1,255);assert(!check());plan=good;
    plan.depth->value=*planExactXenosFloat24Clear(0,254);assert(!check());plan=good;
    plan.tileCount=31;assert(!check());plan=good;
    plan.depth->baseTile=0;assert(!check());plan=good;
    bindings.surfaces[4].header->resize(32);assert(!check());bindings.surfaces[4].header->resize(40);
    put(*bindings.surfaces[4].header,36,(127u<<18)|(127u<<3));assert(check());
    draw.deviceState[11068]=0x10;assert(!check());draw.deviceState[11068]=0x20;assert(check());
    bindings.surfaces[0].tileCacheObject=0x1000;assert(!check());bindings.surfaces[0].tileCacheObject=0;
    put(draw.deviceState,13104,0x1000);assert(!check());put(draw.deviceState,13104,0);
    put(draw.deviceState,12820,0x3000);assert(!check());put(draw.deviceState,12820,0);
    draw.scissorRegisters=std::array<std::uint32_t,3>{0,0,0x400040};assert(!check());
    draw.scissorRegisters=std::array<std::uint32_t,3>{0,0,0x800080};
    NativeMipTextureCapture mip;
    mip.fetch={0x81004802,0xA1000014,127|(127u<<13),0xA80000,2u<<6,0xA2000A10};
    const auto chain=planXenosUnpackedMipChain(mip.fetch);assert(chain);
    mip.base.resize(chain->baseBytes);mip.mips.resize(chain->mipBytes);
    program.textureSlot=7;program.textureSlots={7};program.interpolators=2;
    set(0x2180,0x10110406);set(0x2182,0x30003);
    const auto textured=[&]{return nativeGammaFanState(draw,program,plan,bindings,&mip);};
    assert(textured() && !check());
    for(const auto& r:regs) {
        const auto original=*gta5DrawRegister(draw,r[0]);set(r[0],original^1);
        assert(!textured());set(r[0],original);
    }
    const auto originalMip=mip;
    for(unsigned failure=0;failure<9;++failure) {
        if(failure==0)mip.base.pop_back();
        if(failure==1)mip.mips.pop_back();
        if(failure==2)mip.mips.push_back(0);
        if(failure==3)mip.fetch[1]^=1; // BC3 only.
        if(failure==4)mip.fetch[0]^=0x80000000; // Linear storage rejected.
        if(failure==5)mip.fetch[3]^=1u<<19; // Point min/mag rejected.
        if(failure==6)mip.fetch[4]|=1u<<12; // Nonzero LOD bias rejected.
        if(failure==7)mip.fetch[5]^=8; // Different TriClamp rejected.
        if(failure==8)mip.fetch[0]^=1u<<10; // Different address mode rejected.
        assert(!textured());mip=originalMip;
    }
    program.textureSlots={3};assert(!textured());program.textureSlots={7};
    program.interpolators=1;assert(!textured());program.interpolators=2;
    draw.textures.push_back({});assert(!textured());draw.textures.clear();
    assert(textured());
    std::puts("NATIVE_GAMMA_FAN_STATE_PASS original_bindings_exact_clear_no_depth_writes_and_abi7_required=1");
}
