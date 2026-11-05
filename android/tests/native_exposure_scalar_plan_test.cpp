#include "exposure_scalar_fixture.h"
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
    auto f=loadExposureScalarFixture(argv[1],read);auto& d=f.draw;auto& b=f.bound;auto& p=f.program;
    p.vertexCode=p.pixelCode={0x07230203,0,0,0,0}; // Metadata test, no GPU module.
    const auto check=[&](const auto& draw,const auto& bound,const auto& program,bool idle=true,bool indexed=false){
        return planNativeExposureScalarDraw(draw,bound,program,p.vertexIdentity,p.pixelIdentity,indexed,idle);
    };
    const char* why=nullptr;auto plan=planNativeExposureScalarDraw(d,b,p,p.vertexIdentity,p.pixelIdentity,false,true,&why);
    if(!plan)std::printf("EXPOSURE_PLAN_REFUSED %s\n",why);
    assert(plan && plan->target.width==1 && plan->target.height==1 && plan->target.pitch==80 && plan->target.baseTile==720 &&
        plan->target.tileCount==2 && plan->target.format==7 && plan->storage.sourceBytes==4096 && plan->submitted.format==36);
    assert(nativeOffscreenTileCount(80,1,7)==2);
    unsigned negatives=0;
    for(auto r:NativeFilterWeightsRegisters){auto q=d;q.deviceState[*gta5DrawRegisterOffset(r[0])+3]^=1;assert(!check(q,b,p));++negatives;}
    for(unsigned i=0;i<18;++i){
        auto q=d;auto bound=b;auto pr=p;bool idle=true,indexed=false;
        if(i==0)idle=false;if(i==1)indexed=true;if(i==2)q.textures[0].source.pop_back();
        if(i==3)q.textures[0].fetch[1]^=1;if(i==4)q.vertices[0]^=1;if(i==5)pr.vertexIdentity[0]^=1;
        if(i==6)pr.abiVersion=38;if(i==7)bound.surfaces[0].header->at(24)^=1;
        if(i==8)bound.surfaces[1].object=0x1000;if(i==9)q.vertexShader^=4;
        if(i==10)q.textures.push_back(q.textures[0]);if(i==11)q.scissorRegisters.reset();
        if(i==12)q.textures[0].fetch[3]|=1<<19;if(i==13)pr.interpolators=1;
        if(i==14)pr.pixelIdentity[0]^=1;if(i==15)q.textures[0].fetch[2]=0x6003;
        if(i==16)pr.screenSpaceRectangle=true;if(i==17)q.textures[0].literalZero=true;
        assert(!check(q,bound,pr,idle,indexed));++negatives;
    }
    // Coherent wrong format in register AND header must still be refused.
    for(unsigned format:{6u,14u}){auto q=d;auto bound=b;
        q.deviceState[*gta5DrawRegisterOffset(0x2001)+1]=format;
        bound.surfaces[0].header->at(29)=format;assert(!check(q,bound,p));++negatives;}
    std::printf("EXPOSURE_PLAN_PASS actual_state=1 position_only=1 target=1x1 format=7 physical_base=720 footprint_tiles=2 numeric_r32=1 negatives=%u gpu=0 gameplay=0\n",negatives);
}
