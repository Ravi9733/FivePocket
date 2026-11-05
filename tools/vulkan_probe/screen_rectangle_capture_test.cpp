// Private captured state -> ABI26 admission, HOST ONLY. Metadata is authored;
// separate shader/GPU tests must establish actual program and output semantics.
#include "native_composite_rectangle_plan.h"
#include <cassert>
#include <cstdio>
#include <fstream>
using namespace recomp::gpu;
int main(int argc,char** argv) {
    assert(argc==2);const std::string folder=argv[1];
    auto read=[&](const char* name,unsigned size){std::ifstream f(folder+"/"+name,std::ios::binary|std::ios::ate);
        assert(f && f.tellg()==size);f.seekg(0);std::vector<std::uint8_t> b(size);
        assert(f.read(reinterpret_cast<char*>(b.data()),size));return b;};
    auto word=[](const auto& b,unsigned at){return (std::uint32_t(b[at])<<24)|(std::uint32_t(b[at+1])<<16)|
        (std::uint32_t(b[at+2])<<8)|b[at+3];};
    Gta5InlineDraw d;d.deviceState=read("device-state.bin",13860);d.vertices=read("vertices.bin",72);
    d.primitive=8;d.vertexCount=3;d.stride=24;
    d.scissorRegisters=std::array<std::uint32_t,3>{0,0,47187200};
    Gta5InlineDraw::Texture t;t.slot=2;d.textures={t};
    Gta5BoundSurfaceCapture b;
    for(unsigned i=0;i<5;++i){b.surfaces[i].object=word(d.deviceState,12816+4*i);
        b.surfaces[i].tileCacheObject=word(d.deviceState,13104+4*i);}
    b.surfaces[0].header=read("surface-0-argument.bin",40);
    NativeTranslatedDrawProgram p;p.abiVersion=26;p.compositeRectangle=true;p.screenSpaceRectangle=true;
    p.stride=24;p.textureSlot=2;p.textureSlots={2};
    const char* why=nullptr;auto plan=planNativeCompositeRectangleDraw(d,p,b,&why);
    if(!plan)std::printf("SCREEN_CAPTURE_PLAN_REFUSED stage=%s\n",why);
    assert(plan && plan->baseTile==0 && plan->width==1280 && plan->height==720 && plan->tileCount==720);
    std::puts("SCREEN_CAPTURE_PLAN_PASS original_state=1 generated_r0_xy=1 target_base=0 full720=1 gpu=0 gameplay=0");
}
