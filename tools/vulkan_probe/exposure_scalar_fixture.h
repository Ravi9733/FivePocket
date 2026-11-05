#pragma once
#include "native_exposure_scalar_plan.h"
namespace recomp::gpu {
struct ExposureScalarFixture {Gta5InlineDraw draw;Gta5BoundSurfaceCapture bound;NativeTranslatedDrawProgram program;};
template<class Read> ExposureScalarFixture loadExposureScalarFixture(const std::string& p,Read&& read) {
    const auto word=[](const auto& b,unsigned at){return (unsigned{b[at]}<<24)|(unsigned{b[at+1]}<<16)|(unsigned{b[at+2]}<<8)|b[at+3];};
    ExposureScalarFixture f;auto& d=f.draw;auto& b=f.bound;auto& pr=f.program;
    d.primitive=8;d.vertexCount=3;d.stride=24;d.deviceState=read(p+"/device-state.bin",13860);
    d.vertices=read(p+"/vertices.bin",72);d.vertexShader=word(d.deviceState,13072);d.pixelShader=word(d.deviceState,13068);
    const auto m=read(p+"/capture-meta.bin",36);d.scissorRegisters=std::array<unsigned,3>{word(m,24),word(m,28),word(m,32)};
    Gta5InlineDraw::Texture t;t.slot=0;for(unsigned i=0;i<6;++i)t.fetch[i]=word(m,i*4);
    t.source=read(p+"/texture-0.bin",4096);d.textures={t};
    const auto vc=read(p+"/vertex-constants.bin",4096),pc=read(p+"/pixel-constants.bin",4096);
    for(unsigned i=0;i<1024;++i){d.vertexConstants[i]=word(vc,i*4);d.pixelConstants[i]=word(pc,i*4);}
    for(unsigned i=0;i<5;++i)b.surfaces[i].object=word(d.deviceState,12816+i*4);
    b.surfaces[0].header=read(p+"/surface-0-resource.bin",40);
    pr.abiVersion=39;pr.stride=24;pr.interpolators=0;pr.textureSlot=0;pr.textureSlots={0};
    pr.vertexIdentity=read(p+"/vertex-shader.bin",132);pr.pixelIdentity=read(p+"/pixel-shader.bin",240);
    return f;
}
}
