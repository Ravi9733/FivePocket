#pragma once
#include "native_indexed_blend_depth_plan.h"
#include <cassert>
#include <fstream>
namespace recomp::gpu::test {
// Only a loader. Proprietary captures and compiled modules stay private.
inline std::vector<std::uint8_t> indexedBlendRead(const std::string& path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);assert(f);const auto size=f.tellg();
    assert(size>0 && size<=4194304);f.seekg(0);std::vector<std::uint8_t> b(static_cast<std::size_t>(size));
    assert(f.read(reinterpret_cast<char*>(b.data()),size));return b;
}
inline unsigned indexedBlendBe(const auto& b,unsigned at) {
    assert(at+4<=b.size());return(unsigned(b[at])<<24)|(unsigned(b[at+1])<<16)|(unsigned(b[at+2])<<8)|b[at+3];
}
inline std::vector<unsigned> indexedBlendCode(const std::string& path) {
    const auto b=indexedBlendRead(path);assert(b.size()%4==0);std::vector<unsigned> v(b.size()/4);
    std::memcpy(v.data(),b.data(),b.size());return v;
}
struct IndexedBlendDepthFixture {
    NativeTranslatedDrawProgram program;
    Gta5InlineDraw draw;
    Gta5BoundSurfaceCapture bound;
    NativeIndexedBlendDepthTextures textures;
    std::vector<unsigned> indices;
    explicit IndexedBlendDepthFixture(const std::string& folder) {
        program.abiVersion=12;program.indexed=true;program.stride=28;program.interpolators=10;
        program.textureSlot=3;program.textureSlots={3,2,11};
        program.vertexCode=indexedBlendCode(folder+"/original.vert.spv");
        program.pixelCode=indexedBlendCode(folder+"/original.frag.spv");
        program.vertexIdentity=indexedBlendRead(folder+"/vs.bin");program.pixelIdentity=indexedBlendRead(folder+"/ps.bin");
        draw.primitive=4;draw.stride=28;draw.vertices=indexedBlendRead(folder+"/vertices.bin");
        assert(draw.vertices.size()%28==0);draw.vertexCount=draw.vertices.size()/28;
        draw.deviceState=indexedBlendRead(folder+"/device.bin");assert(draw.deviceState.size()==13860);
        draw.vertexShader=indexedBlendBe(draw.deviceState,13072);draw.pixelShader=indexedBlendBe(draw.deviceState,13068);
        draw.scissorRegisters=std::array<unsigned,3>{0,0,47187200};
        const auto vc=indexedBlendRead(folder+"/vertex-constants.bin"),pc=indexedBlendRead(folder+"/pixel-constants.bin");
        assert(vc.size()==4096 && pc.size()==4096);
        for(unsigned i=0;i<1024;++i){draw.vertexConstants[i]=indexedBlendBe(vc,4*i);draw.pixelConstants[i]=indexedBlendBe(pc,4*i);}
        for(unsigned i=0;i<5;++i)bound.surfaces[i].object=indexedBlendBe(draw.deviceState,12816+4*i);
        bound.surfaces[0].header=indexedBlendRead(folder+"/surface-0-header.bin");
        bound.surfaces[4].header=indexedBlendRead(folder+"/surface-4-header.bin");
        const auto fetches=indexedBlendRead(folder+"/fetches.bin");assert(fetches.size()==72);
        for(unsigned i=0;i<3;++i) {
            const unsigned slot=program.textureSlots[i],at=slot==2?0:slot==3?24:48;
            for(unsigned c=0;c<6;++c)textures[i].fetch[c]=indexedBlendBe(fetches,at+4*c);
            textures[i].base=indexedBlendRead(folder+"/texture-"+std::to_string(slot)+"-base.bin");
        }
        const auto raw=indexedBlendRead(folder+"/effective-indices-u32be.bin");assert(raw.size()%4==0);
        for(unsigned i=0;i<raw.size();i+=4)indices.push_back(indexedBlendBe(raw,i));
    }
};
}
