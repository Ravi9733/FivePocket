// Exact saved-state regression. Shader metadata and scissor are an authored
// planner harness; this does NOT execute or validate the original shader.
#include "native_composite_rectangle_plan.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
using namespace recomp::gpu;
int main(int argc,char** argv) {
    assert(argc==2);const std::string root=argv[1];
    const auto read=[&](const char* name,unsigned size) {
        std::ifstream stream(root+"/"+name,std::ios::binary|std::ios::ate);
        assert(stream && stream.tellg()==size);stream.seekg(0);
        std::vector<std::uint8_t> bytes(size);
        assert(stream.read(reinterpret_cast<char*>(bytes.data()),size));return bytes;
    };
    Gta5InlineDraw draw;draw.deviceState=read("device-state.bin",13860);
    draw.vertices=read("vertices.bin",72);draw.primitive=8;draw.vertexCount=3;draw.stride=24;
    draw.scissorRegisters=std::array<std::uint32_t,3>{0,0,(32u<<16)|1280u};
    Gta5InlineDraw::Texture texture;texture.slot=2;draw.textures={texture};
    NativeTranslatedDrawProgram program;program.abiVersion=20;program.compositeRectangle=true;
    program.stride=24;program.interpolators=1;program.textureSlot=2;program.textureSlots={2};
    program.vertexCode={1};program.pixelCode={1};
    const auto word=[](const auto& b,unsigned at) {
        return (unsigned(b[at])<<24)|(unsigned(b[at+1])<<16)|(unsigned(b[at+2])<<8)|b[at+3];
    };
    Gta5BoundSurfaceCapture bindings;
    for(unsigned i=0;i<5;++i) {
        bindings.surfaces[i].object=word(draw.deviceState,12816+4*i);
        bindings.surfaces[i].tileCacheObject=word(draw.deviceState,13104+4*i);
    }
    // This is the earlier scissor's header, the evidence the old implementation
    // cites for cached object 0x102718b0. Its contents do NOT prove a current bind.
    bindings.surfaces[0].tileCacheHeader=read("scissor-surface-0.bin",40);
    assert(bindings.surfaces[0].object==0x640AF9E0u);
    assert(bindings.surfaces[0].tileCacheObject==0x102718B0u);
    assert(gta5DrawRegister(draw,0x2001)==0);
    assert(word(*bindings.surfaces[0].tileCacheHeader,28)==384);
    const char* why=nullptr;
    const auto plan=planNativeCompositeRectangleDraw(draw,program,bindings,&why);
    if(plan) {
        std::printf("COMPOSITE_WRONG_TARGET_REPRODUCED live_base=0 accepted_base=%u gpu=0 gameplay=0\n",plan->baseTile);
        return 1;
    }
    assert(why && std::string(why)=="current-target-header");
    std::puts("COMPOSITE_CAPTURE_MISMATCH_REJECTED live_base=0 cached_base=384 wrong_target_acknowledged=0 gpu=0 gameplay=0");
}
