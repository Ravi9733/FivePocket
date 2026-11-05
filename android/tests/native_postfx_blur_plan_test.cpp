#include "postfx_blur_fixture.h"
#include "expanded16_copy_fixture.h"
#include <cassert>
#include <cstdio>
#include <fstream>
using namespace recomp::gpu;
int main(int argc,char** argv) {
    assert(argc>=2 && argc<=6); // pass81 capture [, ABI41 T2 capture [, pass82 capture [, pass81 160x90 capture [, pass81 320x180 capture]]]]
    const auto read=[](const std::string& p,unsigned n){
        std::ifstream file(p,std::ios::binary|std::ios::ate);assert(file && file.tellg()==n);file.seekg(0);
        std::vector<std::uint8_t> b(n);assert(file.read(reinterpret_cast<char*>(b.data()),n));return b;
    };
    auto f=loadPostfxBlurFixture(argv[1],read);auto& d=f.draw;auto& b=f.bound;auto& p=f.program;
    p.vertexCode=p.pixelCode={0x07230203,0,0,0,0}; // Metadata test, no GPU module.
    assert(validateNativePostfxBlurProgram(p) && !validateNativeExpanded16CopyProgram(p));
    const auto check=[&](const auto& draw,const auto& bound,const auto& program,bool idle=true,bool indexed=false){
        return planNativePostfxBlurDraw(draw,bound,program,p.vertexIdentity,p.pixelIdentity,indexed,idle);
    };
    // Captured words (zepsefhf renderer-first-unsupported), literal, independent of the planner.
    const auto be32=[](const auto& b,unsigned at){return (unsigned{b[at]}<<24)|(unsigned{b[at+1]}<<16)|(unsigned{b[at+2]}<<8)|b[at+3];};
    for(const auto& [r,v]:std::initializer_list<std::pair<unsigned,unsigned>>{
            {0x2000,0x01800050},{0x2001,0x0003030C},{0x210D,0x00FFFF00},{0x210F,0x42200000},{0x2110,0x42200000},
            {0x2111,0xC1B40000},{0x2112,0x41B40000},{0x2180,0x10010501},{0x2200,0x00700770}})
        assert(*gta5DrawRegister(d,r)==v);
    assert((d.scissorRegisters==std::optional{std::array<unsigned,3>{0,0,0x002D0050}}));
    assert(d.textures[0].fetch==(std::array<std::uint32_t,6>{0x80C04956,0x0654605D,0x0005804F,0x00A80D11,3,0x210}));
    assert(b.surfaces[0].object==0x10271290 && be32(*b.surfaces[0].header,24)==0x01800050 &&
        be32(*b.surfaces[0].header,28)==0x0003030C && be32(*b.surfaces[0].header,36)==0x013C0160);
    // Runtime constants of this capture (TexelSize.x=1/80, 5-tap offsets/weights).
    const auto c=[&](unsigned r,unsigned i){return d.pixelConstants[r*4+i];};
    assert(c(189,0)==0x3C4CCCCD && c(254,0)==0x3EA1E796 && c(254,1)==0x3E687A8D && c(254,2)==0xC04EC4F0 &&
        c(255,0)==0x3FB13B3A && c(255,1)==0x404EC4F0 && c(255,2)==0xBFB13B3A && c(255,3)==0x3D8FE9B8);
    const char* why=nullptr;auto plan=planNativePostfxBlurDraw(d,b,p,p.vertexIdentity,p.pixelIdentity,false,true,&why);
    if(!plan)std::printf("POSTFX_BLUR_PLAN_REFUSED %s\n",why);
    assert(plan && plan->target.width==80 && plan->target.height==45 && plan->target.pitch==80 &&
        plan->target.baseTile==780 && plan->target.tileCount==3 && plan->target.format==3 &&
        plan->target.object==0x10271290 && nativePostfxBlurInstanceTarget(plan->target) &&
        plan->submitted.width==80 && plan->submitted.height==45 && plan->submitted.pitchTexels==96 &&
        plan->submitted.baseAddress==0x06546000 && plan->storage.sourceBytes==49152 &&
        plan->storage.description.format==32);
    assert((80/80)*((45+15)/16)==3); // recordCompletedNativeRgb7e3Replacement footprint
    assert(std::string(why)=="postfx-blur-state-resources-planned-not-executed");
    assert(plan->viewport==*nativeTranslatedViewport(d));
    // Constants are runtime shader inputs, not admission state.
    {auto q=d;q.pixelConstants[189*4]=0x3CCCCCCD;q.pixelConstants[255*4+3]=0;assert(check(q,b,p));}
    unsigned negatives=0;
    for(auto r:NativeFilterWeightsRegisters){auto q=d;q.deviceState[*gta5DrawRegisterOffset(r[0])+3]^=1;assert(!check(q,b,p));++negatives;}
    for(unsigned i=0;i<42;++i){
        auto q=d;auto bound=b;auto pr=p;bool idle=true,indexed=false;
        if(i==0)idle=false;if(i==1)indexed=true;if(i==2)q.textures[0].source.pop_back();
        if(i==3)q.textures[0].fetch[1]^=1;if(i==4)q.textures[0].fetch[3]&=~(1u<<19);
        if(i==5)q.textures[0].fetch[3]&=~(1u<<21);if(i==6)q.textures[0].slot=4;
        if(i==7)q.textures.push_back(q.textures[0]);if(i==8)q.vertices[0]^=1;
        if(i==9)pr.vertexIdentity[0]^=1;if(i==10)pr.pixelIdentity[0]^=1;if(i==11)pr.abiVersion=41;
        // i==12: ABI43 is the admitted vertical sibling (identity-exact); 44 is unknown.
        if(i==12)pr.abiVersion=44;if(i==13)pr.interpolators=0;if(i==14)bound.surfaces[0].header->at(24)^=1;
        if(i==15)bound.surfaces[0].header->at(36)^=1;if(i==16)bound.surfaces[1].object=0x1000;
        if(i==17)q.vertexShader^=4;if(i==18)q.pixelShader^=4;if(i==19)q.scissorRegisters.reset();
        if(i==20)(*q.scissorRegisters)[2]^=1;if(i==21)q.deviceState[11068]|=0x10;
        if(i==22)q.deviceState[12411]=1;                        // surface-mode word 12408
        if(i==23)q.textures[0].fetch[0]=0x81404956;             // pitch160 (ABI41 T1 source word)
        if(i==24)q.textures[0].fetch[0]=0x80804956;             // pitch64 < width
        if(i==25)q.textures[0].fetch[0]=0x80C00156;             // T2 resolve destination word, not the sampled one
        if(i==26)q.textures[0].fetch[2]=0x000B209F;             // 160x90 extent
        if(i==27)q.textures[0].fetch[2]=0x0005A04F;             // 80x46 extent
        if(i==28)q.textures[0].literalZero=true;if(i==29)q.textures[0].fetch[4]^=1;if(i==30)q.textures[0].fetch[5]^=1;
        if(i==31)pr.textureSlot=4;if(i==32)pr.textureSlots={5,5};
        if(i==33){auto extra=q.textures[0];extra.slot=1;q.textures.push_back(extra);}
        if(i==34)pr.pixelIdentity.pop_back();if(i==35)pr.stride=36;
        if(i==36)bound.surfaces[0].object=0x10271250;           // T2's object at the same base
        if(i==37)bound.surfaces[0].header.reset();if(i==38)q.deviceState.pop_back();
        if(i==39)q.vertexCount=4;if(i==40)q.primitive=4;
        if(i==41)q.textures[0].fetch[1]=(q.textures[0].fetch[1]&0xFFFu)|0x1FFFF000u; // storage past 512MB
        assert(!check(q,bound,pr,idle,indexed));++negatives;
    }
    // The current pixel shader must be exactly this program's (the ABI41 60-byte copy is not).
    {std::vector<std::uint8_t> other(p.pixelIdentity.begin(),p.pixelIdentity.begin()+60);
     assert(!planNativePostfxBlurDraw(d,b,p,p.vertexIdentity,other,false,true));++negatives;}
    for(unsigned format:{6u,7u,14u}){auto q=d;auto bound=b;
        q.deviceState[*gta5DrawRegisterOffset(0x2001)+1]=format;
        bound.surfaces[0].header->at(29)=format;assert(!check(q,bound,p));++negatives;}
    // ABI41's band constants on the ABI42 capture: its stencil reference and PS GPR count.
    const auto be=[](auto& b,unsigned at,unsigned v){for(unsigned i=0;i<4;++i)b[at+i]=v>>(24-8*i);};
    const auto reg=[&](auto& q,unsigned r,unsigned v){be(q.deviceState,*gta5DrawRegisterOffset(r),v);};
    {auto q=d;reg(q,0x210D,0x00FFFFFE);assert(!check(q,b,p));++negatives;}
    {auto q=d;reg(q,0x2180,0x10010101);assert(!check(q,b,p));++negatives;}
    // Row table: authored shapes rewrite exactly the size/base words of the capture.
    struct Shape {unsigned sw,sh,w,h,base,info,color,dims,xs,ys,yo,f0,f2,bytes;};
    const auto make=[&](const Shape& z) {
        auto q=d;auto bound=b;
        reg(q,0x2000,z.info);reg(q,0x2001,z.color);reg(q,0x210F,z.xs);reg(q,0x2110,z.xs);reg(q,0x2111,z.ys);reg(q,0x2112,z.yo);
        (*q.scissorRegisters)[2]=(z.h<<16)|z.w;
        be(*bound.surfaces[0].header,24,z.info);be(*bound.surfaces[0].header,28,z.color);be(*bound.surfaces[0].header,36,z.dims);
        q.textures[0].fetch[0]=z.f0;q.textures[0].fetch[2]=z.f2;q.textures[0].source.assign(z.bytes,0);
        return std::pair{q,bound};
    };
    // Literal observed row; authoring it reproduces the captured words and plans.
    const Shape observed{80,45,80,45,780,0x01800050,0x0003030C,0x013C0160,0x42200000,0xC1B40000,0x41B40000,0x80C04956,0x0005804F,49152};
    {auto [q,bound]=make(observed);assert(q.deviceState==d.deviceState && *bound.surfaces[0].header==*b.surfaces[0].header &&
         q.scissorRegisters==d.scissorRegisters && q.textures[0].fetch==d.textures[0].fetch && check(q,bound,p));}
    unsigned rowNegatives=0;
    const auto refuse=[&](const Shape& z){auto [q,bound]=make(z);assert(!check(q,bound,p));++rowNegatives;};
    // ABI41 row1 (T1 160x90 -> T2 80x45, halving) is not a same-size blur.
    refuse({160,90,80,45,780,0x01800050,0x0003030C,0x013C0160,0x42200000,0xC1B40000,0x41B40000,0x81404956,0x000B209F,122880});
    // Predicted later pass81 rows, self-consistent words: NOT admitted until captured.
    // Row1 (160x90, observed cnb086dj): authoring it from the 80x45 capture plans.
    {auto [q,bound]=make({160,90,160,90,768,0x028000A0,0x00030300,0x027C02C8,0x42A00000,0xC2340000,0x42340000,0x81404956,0x000B209F,122880});
     const auto r1=check(q,bound,p);assert(r1 && r1->target.width==160 && r1->target.baseTile==768 && r1->target.tileCount==12);}
    // Row2 (320x180 base720, observed 18rl488a): authoring it from the 80x45 capture plans.
    {auto [q,bound]=make({320,180,320,180,720,0x05000140,0x000302D0,0x04FC0598,0x43200000,0xC2B40000,0x42B40000,0x82804956,0x0016613F,491520});
     const auto r2=check(q,bound,p);assert(r2 && r2->target.width==320 && r2->target.baseTile==720 && r2->target.tileCount==48);}
    // 320x180 at the 160x90 row's base768 is not a row (only the observed base720).
    refuse({320,180,320,180,768,0x05000140,0x00030300,0x04FC0598,0x43200000,0xC2B40000,0x42B40000,0x82804956,0x0016613F,491520});
    // Predicted 640x360 row, self-consistent words: NOT admitted until captured.
    refuse({640,360,640,360,720,0x0A000280,0x000302D0,0x09FC0B38,0x43A00000,0xC3340000,0x43340000,0x85004956,0x002CE27F,1966080});
    auto z=observed;z.base=768;z.color=0x00030300;refuse(z);                 // row at T1's base
    z=observed;z.info=0x01400050;refuse(z);                                  // hiz pitch80 (not the observed96)
    z=observed;z.info=0x01800060;refuse(z);                                  // surface pitch96
    z=observed;z.ys=0x41B40000;refuse(z);                                    // Y not flipped
    z=observed;z.xs=0x42A00000;refuse(z);                                    // 160-wide viewport on the 80x45 target
    z=observed;z.dims=0x027C02C8;refuse(z);                                  // 160x90 surface dims
    z=observed;z.h=46;z.dims=0x013C0168;z.ys=0xC1B80000;z.yo=0x41B80000;refuse(z); // 80x46 target
    z=observed;z.bytes=49151;refuse(z);                                      // truncated owned source
    z=observed;z.f0=0x80004956;refuse(z);                                    // pitch0 source
    {auto [q,bound]=make(observed);(*q.scissorRegisters)[2]=(45<<16)|160;assert(!check(q,bound,p));++rowNegatives;}
    // ABI41's planner cannot take the blur: an ABI41-valid 60-byte program sees ABI42's band state.
    unsigned crossBand=0;
    {auto p41=p;p41.abiVersion=41;p41.pixelIdentity.resize(60);assert(validateNativeExpanded16CopyProgram(p41));
     assert(!planNativeExpanded16CopyDraw(d,b,p41,p41.vertexIdentity,p41.pixelIdentity,false,true,&why));
     assert(std::string(why)=="raster-profile");++crossBand;}
    if(argc>=3) {
        auto g=loadExpanded16CopyFixture(argv[2],read);auto& d2=g.draw;auto& b2=g.bound;auto& p2=g.program;
        p2.vertexCode=p2.pixelCode=p.vertexCode;
        assert(p2.vertexIdentity==p.vertexIdentity && p2.pixelIdentity!=p.pixelIdentity); // Shared post VS only.
        assert(planNativeExpanded16CopyDraw(d2,b2,p2,p2.vertexIdentity,p2.pixelIdentity,false,true));
        // ABI42 planner on the ABI41 T2 capture: identity, then (identity forced) its state.
        assert(!planNativePostfxBlurDraw(d2,b2,p,p.vertexIdentity,p2.pixelIdentity,false,true));++crossBand;
        assert(!planNativePostfxBlurDraw(d2,b2,p,p.vertexIdentity,p.pixelIdentity,false,true,&why));
        assert(std::string(why)=="raster-profile");++crossBand;
    }
    std::printf("POSTFX_BLUR_PLAN_PASS actual_state=1 target=80x45 format=3 physical_base=780 tiles=3 hiz=96 "
                "source=80x45_linear_pitch96 same_size=1 runtime_constants=1 negatives=%u gpu=0 gameplay=0\n",negatives);
    std::printf("POSTFX_BLUR_ROW_TABLE_PASS rows=3 observed_row_authored_equals_captured=1 refused_rows=%u "
                "cross_band_refusals=%u gpu=0 gameplay=0\n",rowNegatives,crossBand);
    if(argc>=4) {
        // ABI43 pass82 (vertical mirror, 6ehacmfz): the same row with its own exact program.
        auto g=loadPostfxBlurFixture(argv[3],read);auto& d3=g.draw;auto& b3=g.bound;auto& p3=g.program;
        p3.abiVersion=43;p3.vertexCode=p3.pixelCode=p.vertexCode;
        assert(validateNativePostfxVerticalBlurProgram(p3) && !validateNativePostfxBlurProgram(p3) &&
            validateNativePostfxBlurFamilyProgram(p3) && p3.vertexIdentity==p.vertexIdentity && p3.pixelIdentity!=p.pixelIdentity);
        const auto v=planNativePostfxBlurDraw(d3,b3,p3,p3.vertexIdentity,p3.pixelIdentity,false,true,&why);
        if(!v)std::printf("POSTFX_VERTICAL_BLUR_PLAN_REFUSED %s\n",why);
        assert(v && v->target.width==80 && v->target.height==45 && v->target.baseTile==780 && v->target.tileCount==3 &&
            v->target.object==0x10271250 && v->submitted.baseAddress==0x06552000 && v->submitted.pitchTexels==96);
        // Every checked band register equals pass81's.
        for(auto r:NativeFilterWeightsRegisters)assert(*gta5DrawRegister(d3,r[0])==*gta5DrawRegister(d,r[0]));
        unsigned vertical=0;
        const auto refuse3=[&](const auto& q,const auto& bound,const auto& program,const auto& vertex,const auto& pixel){
            assert(!planNativePostfxBlurDraw(q,bound,program,vertex,pixel,false,true));++vertical;};
        refuse3(d3,b3,p,p.vertexIdentity,p3.pixelIdentity);   // horizontal program, vertical shaders bound
        refuse3(d,b,p3,p3.vertexIdentity,p.pixelIdentity);    // vertical program, horizontal shaders bound
        // Admission binds a program to the bound shader's exact identity; the ABI label
        // alone does not (the renderer only uses the two fixed packs).
        for(auto r:NativeFilterWeightsRegisters){auto q=d3;q.deviceState[*gta5DrawRegisterOffset(r[0])+3]^=1;refuse3(q,b3,p3,p3.vertexIdentity,p3.pixelIdentity);}
        {auto q=d3;q.textures[0].source.pop_back();refuse3(q,b3,p3,p3.vertexIdentity,p3.pixelIdentity);}
        {auto q=d3;q.textures[0].fetch[0]=0x81404956;refuse3(q,b3,p3,p3.vertexIdentity,p3.pixelIdentity);}
        {auto bound=b3;bound.surfaces[0].header->at(36)^=1;refuse3(d3,bound,p3,p3.vertexIdentity,p3.pixelIdentity);}
        std::printf("POSTFX_VERTICAL_BLUR_PLAN_PASS actual_state=1 abi=43 target=80x45 physical_base=780 tiles=3 "
                    "source=T3_06552000_pitch96 same_row_as_pass81=1 negatives=%u gpu=0 gameplay=0\n",vertical);
    }
    if(argc>=5) {
        // Row1 OBSERVED: pass81 at 160x90 (cnb086dj) plans exactly with the ABI42 program.
        auto g=loadPostfxBlurFixture(argv[4],read);auto& d4=g.draw;auto& b4=g.bound;auto& p4=g.program;
        p4.vertexCode=p4.pixelCode=p.vertexCode;
        assert(p4.vertexIdentity==p.vertexIdentity && p4.pixelIdentity==p.pixelIdentity);
        const auto r=planNativePostfxBlurDraw(d4,b4,p4,p4.vertexIdentity,p4.pixelIdentity,false,true,&why);
        if(!r)std::printf("POSTFX_BLUR_ROW1_REFUSED %s\n",why);
        assert(r && r->target.width==160 && r->target.height==90 && r->target.pitch==160 && r->target.baseTile==768 &&
            r->target.tileCount==12 && r->target.object==0x10271210 && r->submitted.baseAddress==0x0650A000 &&
            r->submitted.pitchTexels==160 && r->storage.sourceBytes==122880);
        unsigned row1=0;
        const auto refuse4=[&](auto q,auto bound){assert(!planNativePostfxBlurDraw(q,bound,p4,p4.vertexIdentity,p4.pixelIdentity,false,true));++row1;};
        for(auto reg:NativeFilterWeightsRegisters){auto q=d4;q.deviceState[*gta5DrawRegisterOffset(reg[0])+3]^=1;refuse4(q,b4);}
        {auto q=d4;q.textures[0].source.pop_back();refuse4(q,b4);}
        {auto q=d4;q.textures[0].fetch[0]=0x80C04956;refuse4(q,b4);}      // 80x45 row's source pitch
        {auto bound=b4;bound.surfaces[0].header->at(36)^=1;refuse4(d4,bound);}
        {auto q=d4;(*q.scissorRegisters)[2]=0x002D0050;refuse4(q,b4);}     // 80x45 scissor
        std::printf("POSTFX_BLUR_ROW1_CAPTURE_PASS actual_state=1 target=160x90 physical_base=768 tiles=12 source=T1_0650A000_pitch160 "
                    "negatives=%u gpu=0 gameplay=0\n",row1);
    }
    if(argc==6) {
        // Row2 OBSERVED: pass81 at 320x180 (18rl488a) plans exactly with the ABI42 program.
        auto g=loadPostfxBlurFixture(argv[5],read);auto& d5=g.draw;auto& b5=g.bound;auto& p5=g.program;
        p5.vertexCode=p5.pixelCode=p.vertexCode;
        assert(p5.vertexIdentity==p.vertexIdentity && p5.pixelIdentity==p.pixelIdentity);
        for(const auto& [r,v]:std::initializer_list<std::pair<unsigned,unsigned>>{
                {0x2000,0x05000140},{0x2001,0x000302D0},{0x210D,0x00FFFF00},{0x210F,0x43200000},{0x2110,0x43200000},
                {0x2111,0xC2B40000},{0x2112,0x42B40000},{0x2180,0x10010501},{0x2200,0x00700770}})
            assert(*gta5DrawRegister(d5,r)==v);
        assert(d5.textures[0].fetch==(std::array<std::uint32_t,6>{0x82804956,0x0641A05D,0x0016613F,0x00A80D11,3,0x210}));
        assert(b5.surfaces[0].object==0x10271110 && be32(*b5.surfaces[0].header,24)==0x05000140 &&
            be32(*b5.surfaces[0].header,28)==0x000302D0 && be32(*b5.surfaces[0].header,36)==0x04FC0598);
        const auto r=planNativePostfxBlurDraw(d5,b5,p5,p5.vertexIdentity,p5.pixelIdentity,false,true,&why);
        if(!r)std::printf("POSTFX_BLUR_ROW2_REFUSED %s\n",why);
        assert(r && r->target.width==320 && r->target.height==180 && r->target.pitch==320 && r->target.baseTile==720 &&
            r->target.tileCount==48 && r->target.object==0x10271110 && nativePostfxBlurInstanceTarget(r->target) &&
            r->submitted.baseAddress==0x0641A000 && r->submitted.pitchTexels==320 && r->storage.sourceBytes==491520);
        unsigned row2=0;
        const auto refuse5=[&](auto q,auto bound){assert(!planNativePostfxBlurDraw(q,bound,p5,p5.vertexIdentity,p5.pixelIdentity,false,true));++row2;};
        for(auto reg:NativeFilterWeightsRegisters){auto q=d5;q.deviceState[*gta5DrawRegisterOffset(reg[0])+3]^=1;refuse5(q,b5);}
        {auto q=d5;q.textures[0].source.pop_back();refuse5(q,b5);}
        {auto q=d5;q.textures[0].fetch[0]=0x81404956;refuse5(q,b5);}      // 160x90 row's source pitch
        {auto bound=b5;bound.surfaces[0].header->at(36)^=1;refuse5(d5,bound);}
        {auto q=d5;(*q.scissorRegisters)[2]=0x005A00A0;refuse5(q,b5);}     // 160x90 scissor
        std::printf("POSTFX_BLUR_ROW2_CAPTURE_PASS actual_state=1 target=320x180 physical_base=720 tiles=48 source=A_0641A000_pitch320 "
                    "negatives=%u gpu=0 gameplay=0\n",row2);
    }
}