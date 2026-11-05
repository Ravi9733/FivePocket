#pragma once
#include "native_sampled_stencil_plan.h"
#include <cassert>
#include <fstream>

namespace recomp::gpu::test {
// Owned private inputs only; no payloads or shader identities in public source.
struct SampledStencilFixture {
    Gta5InlineDraw draw;Gta5BoundSurfaceCapture bindings;NativeTranslatedDrawProgram program;
    static std::vector<std::uint8_t> read(const std::string& name) {
        std::ifstream f(name,std::ios::binary|std::ios::ate);assert(f);const auto size=f.tellg();
        assert(size>0 && size<=4194304);f.seekg(0);std::vector<std::uint8_t> result(static_cast<std::size_t>(size));
        assert(f.read(reinterpret_cast<char*>(result.data()),size));return result;
    }
    static unsigned word(const auto& b,unsigned at) {
        assert(at+4<=b.size());return(unsigned(b[at])<<24)|(unsigned(b[at+1])<<16)|(unsigned(b[at+2])<<8)|b[at+3];
    }
    explicit SampledStencilFixture(const std::string& path) {
        draw.primitive=4;draw.stride=36;draw.vertexCount=3;
        draw.deviceState=read(path+"/device-state.bin");assert(draw.deviceState.size()==13860);
        draw.vertices=read(path+"/vertices.bin");assert(draw.vertices.size()==108);
        draw.vertexShader=word(draw.deviceState,13072);draw.pixelShader=word(draw.deviceState,13068);
        const auto scissor=read(path+"/scissor.bin");assert(scissor.size()==12);
        draw.scissorRegisters=std::array<unsigned,3>{word(scissor,0),word(scissor,4),word(scissor,8)};
        const auto vc=read(path+"/vertex-constants.bin"),pc=read(path+"/pixel-constants.bin");assert(vc.size()==4096 && pc.size()==4096);
        for(unsigned i=0;i<1024;++i){draw.vertexConstants[i]=word(vc,4*i);draw.pixelConstants[i]=word(pc,4*i);}
        for(unsigned i=0;i<5;++i){bindings.surfaces[i].object=word(draw.deviceState,12816+4*i);
            if(bindings.surfaces[i].object)bindings.surfaces[i].header=read(path+"/surface-"+std::to_string(i)+"-resource.bin");}
        Gta5InlineDraw::Texture t;t.slot=1;t.source=read(path+"/texture.bin");
        const auto fetch=read(path+"/fetch.bin");assert(fetch.size()==24);
        for(unsigned i=0;i<6;++i)t.fetch[i]=word(fetch,4*i);draw.textures.push_back(std::move(t));
        program.abiVersion=34;program.directTriangles=true;program.stride=36;program.interpolators=1;program.textureSlot=1;
        program.vertexIdentity=read(path+"/vertex-shader.bin");program.pixelIdentity=read(path+"/pixel-shader.bin");
        const auto code=[&](const char* name){auto b=read(path+"/"+name);assert(b.size()%4==0);
            std::vector<unsigned> result(b.size()/4);std::memcpy(result.data(),b.data(),b.size());return result;};
        program.vertexCode=code("original.vert.spv");program.pixelCode=code("stencil.frag.spv");
    }
};
}
