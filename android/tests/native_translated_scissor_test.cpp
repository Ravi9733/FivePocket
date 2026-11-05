#include "native_translated_draw.h"
#include "native_inline_indices.h"
#include <cassert>
int main() {
    const std::vector<std::uint8_t> encoded{0,0,0,2,0,1};
    const auto indices=recomp::gpu::decodeNativeInlineIndices(3,1,0,3,encoded);
    assert(indices && (*indices==std::vector<std::uint32_t>{0,2,1}));
    assert(!recomp::gpu::decodeNativeInlineIndices(3,1,0,2,encoded));
    assert(!recomp::gpu::decodeNativeInlineIndices(3,1,1,3,encoded));
    assert(!recomp::gpu::decodeNativeInlineIndices(3,0,0,3,encoded));
    assert(!recomp::gpu::decodeNativeInlineIndices(6,1,0,3,encoded));
    recomp::gpu::Gta5InlineDraw draw;
    assert(!recomp::gpu::nativeTranslatedScissor(draw));
    draw.scissorRegisters=std::array<std::uint32_t,3>{0,95,44892445};
    const auto rectangle=recomp::gpu::nativeTranslatedScissor(draw);
    assert((rectangle==std::array<std::uint32_t,4>{95,0,190,685}));
    draw.scissorRegisters=std::array<std::uint32_t,3>{0,0,0x2D00500};
    assert((recomp::gpu::nativeTranslatedScissor(draw)==std::array<std::uint32_t,4>{0,0,1280,720}));
    draw.scissorRegisters=std::array<std::uint32_t,3>{0,96,95};
    assert(!recomp::gpu::nativeTranslatedScissor(draw));
    draw.scissorRegisters=std::array<std::uint32_t,3>{1,0,0x2D00500};
    assert(!recomp::gpu::nativeTranslatedScissor(draw));
    draw.scissorRegisters=std::array<std::uint32_t,3>{0,0,0x2D00501};
    assert(!recomp::gpu::nativeTranslatedScissor(draw));
    draw.deviceState.resize(11000);
    const auto word=[&](unsigned reg,std::uint32_t bits){
        const auto at=*recomp::gpu::gta5DrawRegisterOffset(reg);
        for(unsigned i=0;i<4;++i)draw.deviceState[at+i]=static_cast<std::uint8_t>(bits>>(24-i*8));
    };
    word(0x210F,0x42BE0000);word(0x2110,0x433E0000);
    word(0x2111,0xC3AB4000);word(0x2112,0x43AB4000);word(0x2302,5);
    const auto viewport=recomp::gpu::nativeTranslatedViewport(draw);assert(viewport);
    // Original clip-space corners land at the actual panel bounds.
    assert(((-(*viewport)[0]+(*viewport)[2])+1)*1024==95);
    assert((((*viewport)[0]+(*viewport)[2])+1)*1024==285);
    assert((((*viewport)[1]+(*viewport)[3])+1)*512==0);
    assert(((-(*viewport)[1]+(*viewport)[3])+1)*512==685);
    word(0x2302,4);const auto zeroCenter=recomp::gpu::nativeTranslatedViewport(draw);assert(zeroCenter);
    assert(((-(*zeroCenter)[0]+(*zeroCenter)[2])+1)*1024==95.5f);
    word(0x210F,0x7FC00000);assert(!recomp::gpu::nativeTranslatedViewport(draw));

    // Complete synthetic two-texture UI state; no game payload is embedded.
    constexpr unsigned state[][2]{
        {0x2000,0x14000500},{0x2001,0xC02D0},{0x200E,0},{0x200F,0x20002000},
        {0x2100,0xFFFFFF},{0x2101,0},{0x2102,0},{0x2104,15},
        {0x210F,0x44200000},{0x2110,0x44200000},{0x2111,0xC3B40000},
        {0x2112,0x43B40000},{0x2113,0xBF800000},{0x2114,0x3F800000},
        {0x2180,0x10310304},{0x2181,8},{0x2182,0x3000F},{0x2183,0},{0x2184,0},
        {0x2200,0x700770},{0x2201,0x10706},{0x2202,0xAA000007},{0x2203,0x10},
        {0x2204,0x80000},{0x2205,0x18000},{0x2206,0x43F},{0x2208,4},
        {0x2284,0},{0x2292,0},{0x2293,0},{0x2301,0},{0x2302,4},
        {0x2303,0x40000000},{0x2304,0x3F800000},{0x2305,0x40000000},{0x2306,0x3F800000},
        {0x2307,0xFF000},{0x2308,0xFF100},{0x2312,0xFFFF}
    };
    for(const auto& value:state)word(value[0],value[1]);
    draw.scissorRegisters=std::array<std::uint32_t,3>{0,0,0x2D00500};
    draw.primitive=4;draw.vertexCount=71;draw.stride=12;draw.vertices.resize(71*12);
    draw.textures.resize(2);draw.textures[0].slot=2;draw.textures[1].slot=3;
    recomp::gpu::NativeTranslatedDrawProgram program;
    program.stride=12;program.textureSlot=2;program.indexed=true;
    program.interpolators=4;program.textureSlots={2,3};
    const std::vector<std::uint32_t> triangles{0,1,2,2,1,70};
    assert(recomp::gpu::validateNativeTranslatedDraw(draw,program,triangles));
    program.explicitLodRectangle=true;assert(!recomp::gpu::validateNativeTranslatedDraw(draw,program,triangles));program.explicitLodRectangle=false;
    draw.textures[1].slot=2;assert(!recomp::gpu::validateNativeTranslatedDraw(draw,program,triangles));
    draw.textures[1].slot=3;program.textureSlots={3,2};
    assert(!recomp::gpu::validateNativeTranslatedDraw(draw,program,triangles));
    program.textureSlots={2,2};assert(!recomp::gpu::validateNativeTranslatedDraw(draw,program,triangles));
    program.textureSlots={2,32};assert(!recomp::gpu::validateNativeTranslatedDraw(draw,program,triangles));
    program.textureSlots={2,3};draw.textures.resize(1);
    assert(!recomp::gpu::validateNativeTranslatedDraw(draw,program,triangles));
    program.textureSlots.clear();program.interpolators=3;
    word(0x2180,0x10210204);word(0x2182,0x10007);
    assert(recomp::gpu::validateNativeTranslatedDraw(draw,program,triangles));
    program.interpolators=2;word(0x2180,0x10110103);word(0x2182,0x10003);
    assert(recomp::gpu::validateNativeTranslatedDraw(draw,program,triangles));
    draw.textures.clear();program.textureSlot=UINT32_MAX;program.interpolators=1;
    word(0x2180,0x10010103);word(0x2181,0);word(0x2182,1);
    assert(recomp::gpu::validateNativeTranslatedDraw(draw,program,triangles));
    assert(!recomp::gpu::validateNativeTranslatedDraw(draw,program,{0,1,71}));
    assert(!recomp::gpu::validateNativeTranslatedDraw(draw,program,{0,1}));
    program.indexed=false;draw.primitive=13;draw.vertexCount=4;draw.vertices.resize(48);
    assert(recomp::gpu::validateNativeTranslatedDraw(draw,program));
    program.abiVersion=5;assert(!recomp::gpu::validateNativeTranslatedDraw(draw,program));program.abiVersion=4;
    program.directTriangles=true;assert(!recomp::gpu::validateNativeTranslatedDraw(draw,program));program.directTriangles=false;
    program.affinePositionXY.emplace();assert(!recomp::gpu::validateNativeTranslatedDraw(draw,program));
}
