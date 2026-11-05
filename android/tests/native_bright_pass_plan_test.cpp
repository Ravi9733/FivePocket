#include "bright_pass_fixture.h"
#include <cassert>
#include <cstdio>
#include <fstream>
using namespace recomp::gpu;
int main(int argc,char** argv) {
    assert(argc==2);
    const auto read=[](const std::string& p,unsigned n){
        std::ifstream file(p,std::ios::binary|std::ios::ate);assert(file && file.tellg()==n);file.seekg(0);
        std::vector<std::uint8_t> b(n);assert(file.read(reinterpret_cast<char*>(b.data()),n));return b;
    };
    auto f=loadBrightPassFixture(argv[1],read);auto& d=f.draw;auto& b=f.bound;auto& p=f.program;
    p.vertexCode=p.pixelCode={0x07230203,0,0,0,0}; // Metadata test, no GPU module.
    assert(validateNativeBrightPassProgram(p));
    const auto check=[&](const auto& draw,const auto& bound,const auto& program,bool idle=true,bool indexed=false){
        return planNativeBrightPassDraw(draw,bound,program,p.vertexIdentity,p.pixelIdentity,indexed,idle);
    };
    const char* why=nullptr;auto plan=planNativeBrightPassDraw(d,b,p,p.vertexIdentity,p.pixelIdentity,false,true,&why);
    if(!plan)std::printf("BRIGHT_PASS_PLAN_REFUSED %s\n",why);
    assert(plan && plan->target.width==320 && plan->target.height==180 && plan->target.pitch==320 &&
        plan->target.baseTile==720 && plan->target.tileCount==48 && plan->target.format==3 &&
        plan->texel.width==1 && plan->texel.height==1 && plan->scene.width==640 && plan->scene.height==360 &&
        plan->texelStorage.sourceBytes==8192 && plan->sceneStorage.sourceBytes==1966080 &&
        plan->texelStorage.description.format==32 && plan->sceneStorage.description.format==32);
    assert((320/80)*((180+15)/16)==48); // recordCompletedNativeRgb7e3Replacement footprint
    assert(std::string(why)=="bright-pass-state-resources-planned-not-executed");
    unsigned negatives=0;
    for(auto r:NativeFilterWeightsRegisters){auto q=d;q.deviceState[*gta5DrawRegisterOffset(r[0])+3]^=1;assert(!check(q,b,p));++negatives;}
    for(unsigned i=0;i<30;++i){
        auto q=d;auto bound=b;auto pr=p;bool idle=true,indexed=false;
        if(i==0)idle=false;if(i==1)indexed=true;if(i==2)q.textures[0].source.pop_back();
        if(i==3)q.textures[1].source.pop_back();if(i==4)q.textures[0].fetch[1]^=1;
        if(i==5)q.textures[1].fetch[3]&=~(1u<<19); // scene magnification POINT
        if(i==6)q.textures[1].fetch[3]&=~(1u<<21); // scene minification POINT
        if(i==7)q.textures[2].fetch[1]^=0x1000;     // slot17 names another texel
        if(i==8)q.textures[2].source[0]^=1;         // slot17 bytes differ
        if(i==9)std::swap(q.textures[0],q.textures[1]);
        if(i==10)q.textures.pop_back();if(i==11)q.textures.push_back(q.textures[0]);
        if(i==12)q.vertices[0]^=1;if(i==13)pr.vertexIdentity[0]^=1;if(i==14)pr.pixelIdentity[0]^=1;
        if(i==15)pr.abiVersion=39;if(i==16)pr.textureSlots={1,10};if(i==17)pr.interpolators=0;
        if(i==18)bound.surfaces[0].header->at(24)^=1;if(i==19)bound.surfaces[0].header->at(36)^=1;
        if(i==20)bound.surfaces[1].object=0x1000;if(i==21)q.vertexShader^=4;
        if(i==22)q.scissorRegisters.reset();if(i==23)q.textures[1].fetch[2]=0x2CE13F;
        if(i==24)q.textures[0].literalZero=true;if(i==25)pr.screenSpaceRectangle=true;
        if(i==26)q.textures[1].fetch[4]=0;if(i==27)q.textures[0].fetch[0]^=1u<<26;
        if(i==28)(*q.scissorRegisters)[2]^=1;if(i==29)q.deviceState[11068]|=0x10;
        assert(!check(q,bound,pr,idle,indexed));++negatives;
    }
    // Coherent wrong format in register AND header must still be refused.
    for(unsigned format:{6u,7u,14u}){auto q=d;auto bound=b;
        q.deviceState[*gta5DrawRegisterOffset(0x2001)+1]=format;
        bound.surfaces[0].header->at(29)=format;assert(!check(q,bound,p));++negatives;}
    std::printf("BRIGHT_PASS_PLAN_PASS actual_state=1 target=320x180 format=3 physical_base=720 tiles=48 "
                "scene=640x360_linear texel=1x1_point slot17_equals_slot1=1 negatives=%u gpu=0 gameplay=0\n",negatives);
}
