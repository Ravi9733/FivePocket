#pragma once
#include "native_indexed_lod_plan.h"
#include "native_vertex_clip_program.h"
#include <cassert>
#include <cstring>
#include <fstream>

namespace recomp::gpu::test {
struct IndexedLodFixture {
    Gta5InlineDraw draw;Gta5BoundSurfaceCapture bound;NativeIndexedLodTextures textures;
    NativeClippedFanProgram positions;std::vector<unsigned> vertexCode,pixelCode,indices;
    static std::vector<std::uint8_t> read(const std::string& p) {
        std::ifstream f(p,std::ios::binary|std::ios::ate);assert(f);const auto size=f.tellg();
        assert(size>0 && size<=4194304);f.seekg(0);std::vector<std::uint8_t> b(static_cast<std::size_t>(size));
        assert(f.read(reinterpret_cast<char*>(b.data()),size));return b;
    }
    static unsigned be(const auto& b,unsigned at) {
        assert(at+4<=b.size());return(unsigned(b[at])<<24)|(unsigned(b[at+1])<<16)|(unsigned(b[at+2])<<8)|b[at+3];
    }
    static std::vector<unsigned> code(const std::string& p) {
        const auto b=read(p);assert(b.size()%4==0);std::vector<unsigned> result(b.size()/4);
        std::memcpy(result.data(),b.data(),b.size());return result;
    }
    explicit IndexedLodFixture(const std::string& folder,
            NativeIndexedLodProfile profile=NativeIndexedLodProfile::Indexed600) {
        draw.primitive=4;draw.stride=28;draw.vertices=read(folder+"/vertices.bin");draw.vertexCount=draw.vertices.size()/28;
        draw.deviceState=read(folder+"/device.bin");assert(draw.deviceState.size()==13860);
        draw.vertexShader=be(draw.deviceState,13072);draw.pixelShader=be(draw.deviceState,13068);
        draw.scissorRegisters=std::array<unsigned,3>{0,0,47187200};
        const auto vc=read(folder+"/vertex-constants.bin"),pc=read(folder+"/pixel-constants.bin");assert(vc.size()==4096 && pc.size()==4096);
        for(unsigned i=0;i<1024;++i){draw.vertexConstants[i]=be(vc,4*i);draw.pixelConstants[i]=be(pc,4*i);}
        for(unsigned i=0;i<5;++i)bound.surfaces[i].object=be(draw.deviceState,12816+4*i);
        bound.surfaces[0].header=read(folder+"/surface-0-header.bin");bound.surfaces[4].header=read(folder+"/surface-4-header.bin");
        const auto slots=nativeIndexedLodSlots(profile);
        for(unsigned i=0;i<5;++i) {
            const auto prefix=folder+"/texture-"+std::to_string(slots[i]);const auto raw=read(prefix+"-fetch.bin");assert(raw.size()==24);
            for(unsigned c=0;c<6;++c)textures[i].fetch[c]=be(raw,4*c);
            textures[i].base=read(prefix+"-base.bin");if(slots[i]!=15)textures[i].mips=read(prefix+"-mips.bin");
        }
        const auto raw=read(folder+"/effective-indices-u32be.bin");assert(raw.size()%4==0);
        for(unsigned i=0;i<raw.size();i+=4)indices.push_back(be(raw,i));
        positions={read(folder+"/vs.bin"),read(folder+"/ps.bin"),code(folder+"/position.comp.spv"),28};
        vertexCode=code(folder+"/original.vert.spv");pixelCode=code(folder+"/trace.frag.spv");
    }
};
}
