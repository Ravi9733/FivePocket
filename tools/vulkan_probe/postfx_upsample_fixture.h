#pragma once
#include "native_postfx_upsample_plan.h"
namespace recomp::gpu {
struct PostfxUpsampleFixture {Gta5InlineDraw draw;Gta5BoundSurfaceCapture bound;NativeTranslatedDrawProgram program;};
// Private captured PostFX pass83 inputs (same file names as the ABI41/42
// fixtures); the caller supplies SPIR-V separately.
template<class Read> PostfxUpsampleFixture loadPostfxUpsampleFixture(const std::string& p,Read&& read) {
    const auto word=[](const auto& b,unsigned at){return (unsigned{b[at]}<<24)|(unsigned{b[at+1]}<<16)|(unsigned{b[at+2]}<<8)|b[at+3];};
    PostfxUpsampleFixture f;auto& d=f.draw;auto& b=f.bound;auto& pr=f.program;
    d.primitive=8;d.vertexCount=3;d.stride=24;d.deviceState=read(p+"/device-state.bin",13860);
    d.vertices=read(p+"/vertices.bin",72);d.vertexShader=word(d.deviceState,13072);d.pixelShader=word(d.deviceState,13068);
    const auto m=read(p+"/capture-meta.bin",36); // 6 BE slot5 fetch words + 3 BE scissor words.
    d.scissorRegisters=std::array<unsigned,3>{word(m,24),word(m,28),word(m,32)};
    Gta5InlineDraw::Texture t;t.slot=5;for(unsigned i=0;i<6;++i)t.fetch[i]=word(m,i*4);
    // Owned source size follows the captured fetch (80x45, tiled pitch96 -> 49152 bytes).
    const auto description=XenosTextureDescription::decode(t.fetch);
    const auto storage=description?planXenosExpanded16FloatStorage(*description):std::nullopt;
    t.source=read(p+"/source.bin",storage?unsigned(storage->sourceBytes):0u);d.textures={t};
    const auto vc=read(p+"/vertex-constants.bin",4096),pc=read(p+"/pixel-constants.bin",4096);
    for(unsigned i=0;i<1024;++i){d.vertexConstants[i]=word(vc,i*4);d.pixelConstants[i]=word(pc,i*4);}
    for(unsigned i=0;i<5;++i)b.surfaces[i].object=word(d.deviceState,12816+i*4);
    b.surfaces[0].header=read(p+"/surface-0-resource.bin",40);
    pr.abiVersion=44;pr.stride=24;pr.interpolators=1;pr.textureSlot=5;pr.textureSlots={5};
    pr.vertexIdentity=read(p+"/vertex-shader.bin",96);pr.pixelIdentity=read(p+"/pixel-shader.bin",60);
    return f;
}
}
