#pragma once
#include "native_bright_pass_plan.h"
namespace recomp::gpu {
struct BrightPassFixture {Gta5InlineDraw draw;Gta5BoundSurfaceCapture bound;NativeTranslatedDrawProgram program;};
// Private captured inputs only; the caller supplies SPIR-V code separately.
template<class Read> BrightPassFixture loadBrightPassFixture(const std::string& p,Read&& read) {
    const auto word=[](const auto& b,unsigned at){return (unsigned{b[at]}<<24)|(unsigned{b[at+1]}<<16)|(unsigned{b[at+2]}<<8)|b[at+3];};
    BrightPassFixture f;auto& d=f.draw;auto& b=f.bound;auto& pr=f.program;
    d.primitive=8;d.vertexCount=3;d.stride=24;d.deviceState=read(p+"/device-state.bin",13860);
    d.vertices=read(p+"/vertices.bin",72);d.vertexShader=word(d.deviceState,13072);d.pixelShader=word(d.deviceState,13068);
    const auto m=read(p+"/capture-meta.bin",84);
    d.scissorRegisters=std::array<unsigned,3>{word(m,72),word(m,76),word(m,80)};
    const auto texel=read(p+"/expanded16-texture-1.bin",8192);
    const auto scene=read(p+"/scene.bin",1966080);
    const unsigned slots[3]{1,10,17};
    for(unsigned t=0;t<3;++t) {
        Gta5InlineDraw::Texture texture;texture.slot=slots[t];
        for(unsigned i=0;i<6;++i)texture.fetch[i]=word(m,t*24+i*4);
        texture.source=t==1?scene:texel;d.textures.push_back(std::move(texture));
    }
    const auto vc=read(p+"/vertex-constants.bin",4096),pc=read(p+"/pixel-constants.bin",4096);
    for(unsigned i=0;i<1024;++i){d.vertexConstants[i]=word(vc,i*4);d.pixelConstants[i]=word(pc,i*4);}
    for(unsigned i=0;i<5;++i)b.surfaces[i].object=word(d.deviceState,12816+i*4);
    b.surfaces[0].header=read(p+"/surface-0-resource.bin",40);
    pr.abiVersion=40;pr.stride=24;pr.interpolators=1;pr.textureSlot=1;pr.textureSlots={1,10,17};
    pr.vertexIdentity=read(p+"/vertex-shader.bin",132);pr.pixelIdentity=read(p+"/pixel-shader.bin",204);
    return f;
}
}
