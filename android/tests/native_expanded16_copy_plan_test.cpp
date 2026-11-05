#include "expanded16_copy_fixture.h"
#include <cassert>
#include <cstdio>
#include <fstream>
using namespace recomp::gpu;
int main(int argc,char** argv) {
    assert(argc==2 || argc==3); // row0 capture [, row1 capture]
    const auto read=[](const std::string& p,unsigned n){
        std::ifstream file(p,std::ios::binary|std::ios::ate);assert(file && file.tellg()==n);file.seekg(0);
        std::vector<std::uint8_t> b(n);assert(file.read(reinterpret_cast<char*>(b.data()),n));return b;
    };
    auto f=loadExpanded16CopyFixture(argv[1],read);auto& d=f.draw;auto& b=f.bound;auto& p=f.program;
    p.vertexCode=p.pixelCode={0x07230203,0,0,0,0}; // Metadata test, no GPU module.
    assert(validateNativeExpanded16CopyProgram(p));
    const auto check=[&](const auto& draw,const auto& bound,const auto& program,bool idle=true,bool indexed=false){
        return planNativeExpanded16CopyDraw(draw,bound,program,p.vertexIdentity,p.pixelIdentity,indexed,idle);
    };
    const char* why=nullptr;auto plan=planNativeExpanded16CopyDraw(d,b,p,p.vertexIdentity,p.pixelIdentity,false,true,&why);
    if(!plan)std::printf("EXPANDED16_COPY_PLAN_REFUSED %s\n",why);
    assert(plan && plan->target.width==160 && plan->target.height==90 && plan->target.pitch==160 &&
        plan->target.baseTile==768 && plan->target.tileCount==12 && plan->target.format==3 &&
        plan->submitted.width==320 && plan->submitted.height==180 && plan->storage.sourceBytes==491520 &&
        plan->storage.description.format==32);
    assert((160/80)*((90+15)/16)==12); // recordCompletedNativeRgb7e3Replacement footprint
    assert(std::string(why)=="expanded16-copy-state-resources-planned-not-executed");
    unsigned negatives=0;
    for(auto r:NativeFilterWeightsRegisters){auto q=d;q.deviceState[*gta5DrawRegisterOffset(r[0])+3]^=1;assert(!check(q,b,p));++negatives;}
    for(unsigned i=0;i<22;++i){
        auto q=d;auto bound=b;auto pr=p;bool idle=true,indexed=false;
        if(i==0)idle=false;if(i==1)indexed=true;if(i==2)q.textures[0].source.pop_back();
        if(i==3)q.textures[0].fetch[1]^=1;if(i==4)q.textures[0].fetch[3]&=~(1u<<19);
        if(i==5)q.textures[0].fetch[3]&=~(1u<<21);if(i==6)q.textures[0].slot=4;
        if(i==7)q.textures.push_back(q.textures[0]);if(i==8)q.vertices[0]^=1;
        if(i==9)pr.vertexIdentity[0]^=1;if(i==10)pr.pixelIdentity[0]^=1;if(i==11)pr.abiVersion=40;
        if(i==12)pr.interpolators=0;if(i==13)bound.surfaces[0].header->at(24)^=1;
        if(i==14)bound.surfaces[0].header->at(36)^=1;if(i==15)bound.surfaces[1].object=0x1000;
        if(i==16)q.vertexShader^=4;if(i==17)q.scissorRegisters.reset();
        if(i==18)q.textures[0].fetch[2]=0x2CE27F;if(i==19)q.textures[0].literalZero=true;
        if(i==20)(*q.scissorRegisters)[2]^=1;if(i==21)q.deviceState[11068]|=0x10;
        assert(!check(q,bound,pr,idle,indexed));++negatives;
    }
    for(unsigned format:{6u,7u,14u}){auto q=d;auto bound=b;
        q.deviceState[*gta5DrawRegisterOffset(0x2001)+1]=format;
        bound.surfaces[0].header->at(29)=format;assert(!check(q,bound,p));++negatives;}
    // Second pass71 instance: T1 160x90 -> T2 80x45 base780, hiz pitch96. Authored
    // from the captured first instance by rewriting exactly the size/base words;
    // with argv[2] the authored words must equal the captured 77745p73 row1.
    const auto be=[](auto& b,unsigned at,unsigned v){for(unsigned i=0;i<4;++i)b[at+i]=v>>(24-8*i);};
    const auto reg=[&](auto& q,unsigned r,unsigned v){be(q.deviceState,*gta5DrawRegisterOffset(r),v);};
    struct Shape {unsigned sw,sh,w,h,base,info,color,dims,xs,ys,yo,f0,f2,bytes;};
    const auto make=[&](const Shape& z) {
        auto q=d;auto bound=b;
        reg(q,0x2000,z.info);reg(q,0x2001,z.color);reg(q,0x210F,z.xs);reg(q,0x2110,z.xs);reg(q,0x2111,z.ys);reg(q,0x2112,z.yo);
        (*q.scissorRegisters)[2]=(z.h<<16)|z.w;
        be(*bound.surfaces[0].header,24,z.info);be(*bound.surfaces[0].header,28,z.color);be(*bound.surfaces[0].header,36,z.dims);
        q.textures[0].fetch[0]=z.f0;q.textures[0].fetch[2]=z.f2;q.textures[0].source.assign(z.bytes,0);
        return std::pair{q,bound};
    };
    // Literal words, independent of the planner's formulas.
    const Shape second{160,90,80,45,780,0x01800050,0x0003030C,0x013C0160,0x42200000,0xC1B40000,0x41B40000,0x81404956,0x000B209F,122880};
    {auto [q,bound]=make(second);const auto p2=planNativeExpanded16CopyDraw(q,bound,p,p.vertexIdentity,p.pixelIdentity,false,true,&why);
     if(!p2)std::printf("EXPANDED16_COPY_SECOND_REFUSED %s\n",why);
     assert(p2 && p2->target.width==80 && p2->target.height==45 && p2->target.pitch==80 && p2->target.baseTile==780 &&
         p2->target.tileCount==3 && p2->target.format==3 && p2->submitted.width==160 && p2->submitted.height==90 &&
         p2->storage.sourceBytes==122880 && p2->storage.description.format==32 && nativeExpanded16CopyInstanceTarget(p2->target));
     assert(plan->target.baseTile+plan->target.tileCount==p2->target.baseTile);} // T2 directly after T1
    unsigned secondNegatives=0;
    const auto refuse=[&](const Shape& z){auto [q,bound]=make(z);assert(!check(q,bound,p));++secondNegatives;};
    auto z=second;z.base=768;z.color=0x00030300;refuse(z);                 // row1 at row0's base
    z=second;z.sw=320;z.sh=180;z.f0=0x82804956;z.f2=0x16613F;z.bytes=491520;refuse(z); // 4:1, not halving
    z=second;z.h=46;z.dims=0x013C0168;z.ys=0xC1B80000;z.yo=0x41B80000;refuse(z);        // 80x46: odd halving
    z=second;z.ys=0x41B40000;refuse(z);                                     // Y not flipped
    z=second;z.xs=0x42A00000;refuse(z);                                     // row0 viewport on row1 target
    z=second;z.dims=0x027C02C8;refuse(z);                                   // row0 surface dims
    z=second;z.f2=0x000B409F;refuse(z);                                     // 160x91 source
    z=second;z.f0=0x81804956;refuse(z);                                     // padded source pitch192
    z=second;z.bytes=122879;refuse(z);                                      // truncated owned source
    z=second;z.info=0x01400050;refuse(z);                                   // hiz pitch80 (not the observed96)
    z=second;z.info=0x014000A0;refuse(z);                                   // pitch160 on row1
    {auto [q,bound]=make(second);(*q.scissorRegisters)[2]=(45<<16)|160;assert(!check(q,bound,p));++secondNegatives;}
    {auto [q,bound]=make(second);q.textures[0].fetch[3]&=~(1u<<19);assert(!check(q,bound,p));++secondNegatives;}
    // Row0 base/format on a row1 viewport: the base is part of the row.
    {Shape w{320,180,160,90,780,0x028000A0,0x0003030C,0x027C02C8,0x42A00000,0xC2340000,0x42340000,0x82804956,0x16613F,491520};refuse(w);}
    // A third, not admitted halving (T2 80x45 -> 40x23 would not be exact anyway).
    {Shape w{80,45,40,22,783,0x00A00028,0x0003030F,0x009C00A8,0x41A00000,0xC1300000,0x41300000,0x80C04956,0x0005804F,49152};refuse(w);}
    std::printf("EXPANDED16_COPY_PLAN_PASS actual_state=1 target=160x90 format=3 physical_base=768 tiles=12 "
                "source=320x180_linear negatives=%u gpu=0 gameplay=0\n",negatives);
    std::printf("EXPANDED16_COPY_SECOND_INSTANCE_PASS authored_from_capture=1 target=80x45 physical_base=780 hiz=96 tiles=3 "
                "source=160x90_linear halving=exact negatives=%u gpu=0 gameplay=0\n",secondNegatives);
    if(argc==3) {
        auto g=loadExpanded16CopyFixture(argv[2],read);auto& d2=g.draw;auto& b2=g.bound;auto& p2=g.program;
        p2.vertexCode=p2.pixelCode=p.vertexCode;
        assert(p2.vertexIdentity==p.vertexIdentity && p2.pixelIdentity==p.pixelIdentity); // Same original pair.
        const auto live=planNativeExpanded16CopyDraw(d2,b2,p2,p2.vertexIdentity,p2.pixelIdentity,false,true,&why);
        if(!live)std::printf("EXPANDED16_COPY_SECOND_CAPTURE_REFUSED %s\n",why);
        assert(live && live->target.width==80 && live->target.height==45 && live->target.pitch==80 &&
            live->target.baseTile==780 && live->target.tileCount==3 && live->target.format==3 &&
            live->submitted.width==160 && live->submitted.height==90 && live->storage.sourceBytes==122880 &&
            live->submitted.baseAddress==0x0650A000 && live->target.object==b2.surfaces[0].object);
        // The authored transform of row0 reproduces the captured row1 words it rewrites.
        auto [q,bound]=make(second);
        for(unsigned r:{0x2000u,0x2001u,0x210Fu,0x2110u,0x2111u,0x2112u})
            assert(*gta5DrawRegister(q,r)==*gta5DrawRegister(d2,r));
        assert(*q.scissorRegisters==*d2.scissorRegisters && q.textures[0].fetch[0]==d2.textures[0].fetch[0] &&
            q.textures[0].fetch[2]==d2.textures[0].fetch[2]);
        for(unsigned at:{24u,28u,36u})for(unsigned i=0;i<4;++i)assert(bound.surfaces[0].header->at(at+i)==b2.surfaces[0].header->at(at+i));
        unsigned captured=0;
        const auto refuse2=[&](auto q2,auto bnd){assert(!planNativeExpanded16CopyDraw(q2,bnd,p2,p2.vertexIdentity,p2.pixelIdentity,false,true));++captured;};
        for(auto r:NativeFilterWeightsRegisters){auto q2=d2;q2.deviceState[*gta5DrawRegisterOffset(r[0])+3]^=1;refuse2(q2,b2);}
        {auto q2=d2;be(q2.deviceState,*gta5DrawRegisterOffset(0x2000),0x01400050);auto bnd=b2;be(*bnd.surfaces[0].header,24,0x01400050);refuse2(q2,bnd);}
        {auto q2=d2;be(q2.deviceState,*gta5DrawRegisterOffset(0x2001),0x00030300);auto bnd=b2;be(*bnd.surfaces[0].header,28,0x00030300);refuse2(q2,bnd);}
        {auto bnd=b2;be(*bnd.surfaces[0].header,36,0x027C02C8);refuse2(d2,bnd);}
        {auto q2=d2;q2.textures[0].source.pop_back();refuse2(q2,b2);}
        {auto q2=d2;q2.textures[0].fetch[0]=0x81804956;refuse2(q2,b2);}
        {auto q2=d2;(*q2.scissorRegisters)[2]=(45<<16)|160;refuse2(q2,b2);}
        {auto q2=d2;q2.textures[0].fetch[3]&=~(1u<<19);refuse2(q2,b2);}
        {auto q2=d2;q2.deviceState[11068]|=0x10;refuse2(q2,b2);}
        {auto bnd=b2;bnd.surfaces[1].object=0x1000;refuse2(d2,bnd);}
        std::printf("EXPANDED16_COPY_SECOND_CAPTURE_PASS actual_state=1 target=80x45 physical_base=780 hiz=96 tiles=3 "
                    "source=160x90_linear authored_equals_captured=1 negatives=%u gpu=0 gameplay=0\n",captured);
    }
}
