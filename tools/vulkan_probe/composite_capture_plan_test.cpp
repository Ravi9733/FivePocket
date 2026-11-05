// Exact private captured STATE -> production planner, host only. Shader
// identity/execution and byte output are independently tested on the GPU.
#include "native_composite_rectangle_plan.h"
#include "composite_derived_loop_fixture.h"
#include <cassert>
#include <cstdio>
#include <fstream>
using namespace recomp::gpu;
int main(int argc,char** argv) {
    assert(argc==3);const std::string folder=argv[1];
    assert(std::string(argv[2])=="0" || std::string(argv[2])=="32");
    const unsigned base=std::string(argv[2])=="32"?32:0;
    const auto read=[&](const char* name,unsigned size) {
        std::ifstream f(folder+"/"+name,std::ios::binary|std::ios::ate);
        assert(f && f.tellg()==size);f.seekg(0);std::vector<std::uint8_t> b(size);
        assert(f.read(reinterpret_cast<char*>(b.data()),size));return b;
    };
    const auto word=[](const auto& b,unsigned at) {
        return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|
            (std::uint32_t{b[at+2]}<<8)|b[at+3];
    };
    const auto meta=read("replay-metadata.bin",48);
    Gta5InlineDraw d;d.primitive=word(meta,0);d.vertexCount=word(meta,4);d.stride=word(meta,8);
    d.deviceState=read("device-state.bin",13860);d.vertices=read("vertices.bin",72);
    d.scissorRegisters=std::array<std::uint32_t,3>{word(meta,12),word(meta,16),word(meta,20)};
    Gta5InlineDraw::Texture t;t.slot=2;for(unsigned i=0;i<6;++i)t.fetch[i]=word(meta,24+4*i);d.textures={t};
    Gta5BoundSurfaceCapture b;
    for(unsigned i=0;i<5;++i){b.surfaces[i].object=word(d.deviceState,12816+4*i);
        b.surfaces[i].tileCacheObject=word(d.deviceState,13104+4*i);}
    b.surfaces[0].header=read("surface-0-argument.bin",40);
    if(b.surfaces[4].object)b.surfaces[4].header=read("surface-4-argument.bin",40);
    NativeTranslatedDrawProgram p;p.abiVersion=20;p.compositeRectangle=true;p.stride=24;
    p.interpolators=1;p.textureSlot=2;p.textureSlots={2};
    const char* why=nullptr;const auto plan=planNativeCompositeRectangleDraw(d,p,b,&why);
    if(!plan)std::printf("COMPOSITE_CAPTURE_PLAN_REFUSED stage=%s\n",why);
    assert(plan && plan->baseTile==base && plan->width==1280 && plan->height==32);
    assert(gta5DrawRegister(d,0x2200)==(base?0x700772u:0x700770u));
    assert(gta5DrawRegister(d,0x2203)==(base?0x715u:0x714u));
    std::printf("COMPOSITE_CAPTURE_PLAN_PASS base=%u actual_state=1 gpu=0 gameplay=0\n",base);
    if(!base) {
        for(unsigned band=0;band<23;++band) {
            auto next=d;auto bound=b;test::deriveCompositeRestoreBand(next,bound,band);
            const auto bp=planNativeCompositeRectangleDraw(next,p,bound,&why);
            if(!bp)std::printf("COMPOSITE_DERIVED_PLAN_REFUSED band=%u stage=%s\n",band,why);
            assert(bp && bp->baseTile==band*32 && bp->tileCount==32);
        }
        std::puts("COMPOSITE_DERIVED_STATE_PASS bands=23 paired_depth_hiz=1 previous_header=1 gpu=0");
    }
}
