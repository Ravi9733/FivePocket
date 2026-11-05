#pragma once
#include "native_query_neutral_fan_plan.h"
#include <cassert>
#include <fstream>
#include <string>

// Public test loader; private game bytes are supplied externally, never embedded.
struct QueryNeutralFanFixture {
    recomp::gpu::NativeClippedFanProgram program;
    recomp::gpu::Gta5InlineDraw draw;
    std::vector<std::uint8_t> visibleConstants;
    static std::vector<std::uint8_t> bytes(const std::string& path) {
        std::ifstream f(path,std::ios::binary|std::ios::ate);assert(f);
        const auto n=f.tellg();assert(n>0 && n<1024*1024);
        std::vector<std::uint8_t> b(n);f.seekg(0);assert(f.read(reinterpret_cast<char*>(b.data()),n));return b;
    }
    static unsigned word(const std::vector<std::uint8_t>& b,unsigned at) {
        assert(at+4<=b.size());return (unsigned(b[at])<<24)|(unsigned(b[at+1])<<16)|(unsigned(b[at+2])<<8)|b[at+3];
    }
    explicit QueryNeutralFanFixture(const std::string& dir) {
        program.stride=36;program.vertexIdentity=bytes(dir+"/vertex-shader.bin");
        program.pixelIdentity=bytes(dir+"/pixel-shader.bin");
        draw.deviceState=bytes(dir+"/device-state.bin");draw.vertices=bytes(dir+"/vertices.bin");
        draw.device=4;draw.primitive=5;draw.stride=36;draw.vertexCount=draw.vertices.size()/36;
        draw.vertexAddress=word(draw.deviceState,13848);
        draw.vertexShader=word(draw.deviceState,13072);draw.pixelShader=word(draw.deviceState,13068);
        draw.scissorRegisters=std::array<unsigned,3>{0,0,0x02D00500};
        const auto vc=bytes(dir+"/vertex-constants.bin"),pc=bytes(dir+"/pixel-constants.bin");
        assert(vc.size()==4096 && pc.size()==4096);
        for(unsigned i=0;i<1024;++i){draw.vertexConstants[i]=word(vc,4*i);draw.pixelConstants[i]=word(pc,4*i);}
        visibleConstants=bytes(dir+"/visible-constants.bin");
    }
};
