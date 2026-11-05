#pragma once
// Attempt v15, DIAGNOSTIC BUILDS ONLY: "show frames" mode (RECOMP_DIAGNOSTIC_SHOW_FRAMES).
// The user asked to see the frames the game renders, correct or wrong, instead of the last
// loading frame that stays on screen once gameplay resolves stop matching the proven shapes.
// Everything here drops state proofs on purpose: a draw whose translated program exists runs
// with that program's fixed pipeline whatever the guest's blend/depth/viewport registers say,
// uncaptured clears of the back buffer still clear its colour, and the front-buffer resolve
// publishes whatever the presentation image holds. Output is wrong wherever state differs.
// Never an installed path.
#include "gta5_color_clear.h"
#include "native_translated_draw.h"
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace recomp::gpu {
// Structural requirements only: the translated program's ABI, its texture slots, and the
// geometry it can rasterise. Returns the first failed requirement (nullptr when drawable).
inline const char* diagShowFramesTranslatedDraw(const Gta5InlineDraw& draw,const NativeTranslatedDrawProgram& program,
                                                const std::vector<std::uint32_t>& indices) {
    if(program.abiVersion!=4 || program.cubeRectanglePwl || program.explicitLodRectangle || program.compositeRectangle ||
       program.depthOnlyRectangle || program.rectangleMrt || program.directTriangles || program.affinePositionXY)return "program-abi";
    const auto slots=nativeTranslatedTextureSlots(program);
    const bool textured=!slots.empty();
    if(slots.size()>2 || (textured && slots[0]!=program.textureSlot))return "program-slots";
    for(unsigned i=0;i<slots.size();++i)if(slots[i]>=32 || (i && slots[i]==slots[0]))return "program-slots";
    if(draw.primitive!=(program.indexed?4u:13u))return "primitive";
    if(!draw.vertexCount || (!program.indexed && (draw.vertexCount%4 || !indices.empty())) ||
       (program.indexed && (indices.empty() || indices.size()%3 || indices.size()>65535)))return "vertex-count";
    if(draw.stride!=program.stride || draw.vertices.size()!=std::uint64_t{draw.vertexCount}*draw.stride ||
       draw.vertices.size()>256*1024)return "vertex-bytes";
    if(draw.textures.size()!=slots.size())return "texture-count";
    for(unsigned i=0;i<slots.size();++i)if(draw.textures[i].slot!=slots[i])return "texture-slot";
    for(const auto i:indices)if(i>=draw.vertexCount)return "index-range";
    const auto center=gta5DrawRegister(draw,0x2302);
    if(!center || (*center!=4 && *center!=5))return "pixel-center";
    return nullptr;
}
inline std::array<std::uint32_t,4> diagShowFramesScissor(const Gta5InlineDraw& draw) {
    if(const auto s=nativeTranslatedScissor(draw))return *s;
    return {0,0,1280,720};
}
// Same mapping as nativeTranslatedViewport; an unreadable or off-screen viewport falls back to
// the full 1280x720 one (xscale 640, xoffset 640, yscale -360, yoffset 360).
inline std::array<float,4> diagShowFramesViewport(const Gta5InlineDraw& draw) {
    if(const auto v=nativeTranslatedViewport(draw))return *v;
    const auto center=gta5DrawRegister(draw,0x2302);
    const float correction=center && *center==4?0.5f:0.0f;
    return {640.0f/1024.0f,-360.0f/512.0f,(640.0f+correction)/1024.0f-1.0f,(360.0f+correction)/512.0f-1.0f};
}
// captureGta5ColorClear without its flag, binning and other-target refusals: a full-rectangle
// clear of the 1280x720 back buffer clears its colour even when depth/stencil/extra targets are
// cleared with it (the presentation image has no depth). Partial rectangles still refuse.
inline std::optional<Gta5ColorClear> diagShowFramesColorClear(std::uint32_t device,
        std::uint32_t flags,std::uint32_t rectangle,std::uint32_t color,
        const Gta5ResourceReader& heap,const Gta5ResourceReader& argument) {
    if(!heap || !argument || !(flags&1) || !rectangle || !color)return std::nullopt;
    const auto state=heap(device,13128),rect=argument(rectangle,16),rgba=argument(color,16);
    if(!state || state->size()!=13128 || !rect || rect->size()!=16 || !rgba || rgba->size()!=16)return std::nullopt;
    const auto word=[](const auto& data,unsigned at) {
        return (std::uint32_t{data[at]}<<24)|(std::uint32_t{data[at+1]}<<16)|
            (std::uint32_t{data[at+2]}<<8)|data[at+3];
    };
    Gta5ColorClear out;out.target=word(*state,12816);
    const auto surface=out.target?heap(out.target,32):std::nullopt;
    if(word(*state,10368)!=0x14000500 || !surface || surface->size()!=32)return std::nullopt;
    const auto colorInfo=word(*surface,28);
    if(colorInfo!=0x302D0 && colorInfo!=0xC02D0)return std::nullopt;
    std::array<int,4> clipped{},viewport{};
    for(unsigned i=0;i<4;++i) {
        const float v=colorFloat(word(*state,13024+i*4));
        if(!std::isfinite(v) || v<-8192 || v>8192)return std::nullopt;
        viewport[i]=static_cast<int>(v);
        clipped[i]=static_cast<std::int32_t>(word(*rect,i*4));
    }
    viewport[2]+=viewport[0];viewport[3]+=viewport[1];
    for(unsigned i=0;i<4;++i)clipped[i]=i<2?std::max(clipped[i],viewport[i]):std::min(clipped[i],viewport[i]);
    if(word(*state,12288))for(unsigned i=0;i<4;++i) {
        const auto s=static_cast<std::int32_t>(word(*state,13052+i*4));
        clipped[i]=i<2?std::max(clipped[i],s):std::min(clipped[i],s);
    }
    if(clipped!=std::array<int,4>{0,0,1280,720})return std::nullopt;
    std::array<std::uint32_t,4> bits{};
    for(unsigned i=0;i<4;++i)bits[i]=word(*rgba,i*4);
    if(!packColor7e3(bits,out.packedColor))return std::nullopt;
    const auto rounded=unpackColor7e3(out.packedColor);
    for(unsigned i=0;i<4;++i)out.color[i]=colorFloat(rounded[i]);
    return out;
}
}
