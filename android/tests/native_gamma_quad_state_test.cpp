#include "native_gamma_quad_state.h"
#include "xenos_depth_stencil_state.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    Gta5InlineDraw draw;draw.primitive=13;draw.vertexCount=16;draw.stride=8;draw.vertices.resize(128);draw.deviceState.resize(13860);
    draw.scissorRegisters=std::array<std::uint32_t,3>{0,0,0x800080};
    const auto put=[](auto& b,unsigned at,unsigned v){for(unsigned i=0;i<4;++i)b[at+i]=v>>(24-8*i);};
    const auto set=[&](unsigned r,unsigned v){put(draw.deviceState,*gta5DrawRegisterOffset(r),v);};
    constexpr unsigned regs[][2]{{0x2000,0x20100A0},{0x2001,0x10000},{0x2002,0x10020},{0x200F,0x20002000},{0x2100,0xFFFFFF},
        {0x2104,15},{0x210C,0xFFFF00},{0x210D,0xFFFFFF},{0x210F,0x42800000},{0x2110,0x42800000},{0x2111,0xC2800000},{0x2112,0x42800000},
        {0x2113,0x3F800000},{0x2180,0x10110401},{0x2181,4},{0x2182,0x30003},{0x2200,0x700746},{0x2201,0x10001},
        {0x2202,0xAA000007},{0x2203,0x13},{0x2204,0x80000},{0x2205,0x18002},{0x2206,0x43F},{0x2208,4},
        {0x2209,0x10001},{0x220A,0x10001},{0x220B,0x10001},
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
    // Authored 32x32 BC1, linear clamp, base-only resource; no private payload.
    Gta5InlineDraw::Texture texture;texture.slot=3;
    texture.fetch={2u|(1u<<22)|(2u<<10)|(2u<<13),0x100000u|18u,31u|(31u<<13),0x688u|(1u<<19)|(1u<<21),0,1u<<9};
    const auto layout=planXenosTexture2D(*XenosTextureDescription::decode(texture.fetch));assert(layout);
    texture.source.resize(layout->sourceBytes);draw.textures.push_back(texture);
    NativeTranslatedDrawProgram program;program.abiVersion=6;program.stride=8;program.textureSlot=3;program.textureSlots={3};program.interpolators=2;
    const auto check=[&]{return nativeGammaQuadState(draw,program,plan,bindings);};assert(check());unsigned cases=0;
    for(const auto& r:regs){set(r[0],r[1]^(r[0]==0x210D?0x100:1));assert(!check());set(r[0],r[1]);++cases;}
    for(unsigned reference=0;reference<256;++reference) {
        set(0x210D,0xFFFF00|reference);assert(check());
        const auto ds=*decodeXenosDepthStencilState(0x700746,0xFFFF00|reference,0xFFFF00);
        assert(!ds.stencilEnable);
        for(bool back:{false,true}) {
            const auto pass=evaluateXenosDepthStencil(ds,.5f,0.f,255,back);
            assert(pass.colorPass && pass.depthWritten && pass.stencil==255);
            const auto reject=evaluateXenosDepthStencil(ds,0.f,0.f,255,back);
            assert(!reject.colorPass && !reject.depthWritten && reject.stencil==255);
        }
    }
    set(0x210D,0xFFFFFF);
    for(unsigned abi:{4u,5u,7u}){program.abiVersion=abi;assert(!check());}program.abiVersion=6;
    program.directTriangles=true;assert(!check());program.directTriangles=false;
    program.indexed=true;assert(!check());program.indexed=false;
    program.textureSlots={2};assert(!check());program.textureSlots={3};
    draw.textures[0].literalZero=true;assert(!check());draw.textures[0].literalZero=false;
    draw.textures[0].source.pop_back();assert(!check());draw.textures[0]=texture;
    draw.textures[0].fetch[4]|=1u<<6;assert(!check());draw.textures[0]=texture;
    draw.textures[0].fetch[3]^=1u<<19;assert(!check());draw.textures[0]=texture;
    auto good=plan;plan.depth->value=*planExactXenosFloat24Clear(.5,255);assert(!check());plan=good;
    plan.depth->baseTile=0;assert(!check());plan=good;
    bindings.surfaces[0].tileCacheObject=0x3000;assert(!check());bindings.surfaces[0].tileCacheObject=0;
    bindings.surfaces[4].header->pop_back();assert(!check());
    std::printf("NATIVE_GAMMA_QUAD_STATE_PASS register_negatives=%u resource_shape_filter_and_depth_guards=1\n",cases);
}
