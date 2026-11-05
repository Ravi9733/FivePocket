#include "postfx_upsample_fixture.h"
#include "postfx_blur_fixture.h"
#include "expanded16_copy_fixture.h"
#include "native_rgb7e3_replacement.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <limits>
using namespace recomp::gpu;
int main(int argc,char** argv) {
    assert(argc>=2 && argc<=5); // pass83 capture [, ABI41 row0 (T1) capture [, ABI42 pass81 capture [, pass83 320x180 capture]]]
    const auto read=[](const std::string& p,unsigned n){
        std::ifstream file(p,std::ios::binary|std::ios::ate);assert(file && file.tellg()==n);file.seekg(0);
        std::vector<std::uint8_t> b(n);assert(file.read(reinterpret_cast<char*>(b.data()),n));return b;
    };
    auto f=loadPostfxUpsampleFixture(argv[1],read);auto& d=f.draw;auto& b=f.bound;auto& p=f.program;
    p.vertexCode=p.pixelCode={0x07230203,0,0,0,0}; // Metadata test, no GPU module.
    assert(validateNativePostfxUpsampleProgram(p) && !validateNativeExpanded16CopyProgram(p) &&
        !validateNativePostfxBlurFamilyProgram(p));
    // Same 96/60-byte sizes as ABI41's copy: the metadata differs only by its label.
    {auto q=p;q.abiVersion=41;assert(validateNativeExpanded16CopyProgram(q) && !validateNativePostfxUpsampleProgram(q));}
    const auto check=[&](const auto& draw,const auto& bound,const auto& program,bool idle=true,bool indexed=false){
        return planNativePostfxUpsampleDraw(draw,bound,program,p.vertexIdentity,p.pixelIdentity,indexed,idle);
    };
    // Captured words (8hvjz4qw renderer-first-unsupported), literal, independent of the planner.
    const auto be32=[](const auto& b,unsigned at){return (unsigned{b[at]}<<24)|(unsigned{b[at+1]}<<16)|(unsigned{b[at+2]}<<8)|b[at+3];};
    for(const auto& [r,v]:std::initializer_list<std::pair<unsigned,unsigned>>{
            {0x2000,0x028000A0},{0x2001,0x00030300},{0x2104,0x0000000F},{0x210D,0x00FFFF00},{0x210F,0x42A00000},
            {0x2110,0x42A00000},{0x2111,0xC2340000},{0x2112,0x42340000},{0x2180,0x10010101},{0x2200,0x00700770},
            {0x2201,0x01010101},{0x2209,0x01010101},{0x220A,0x01010101},{0x220B,0x01010101}})
        assert(*gta5DrawRegister(d,r)==v);
    // Every other checked band word equals the shared table literally.
    std::size_t tableWords=0;
    for(auto r:NativeFilterWeightsRegisters)switch(r[0]) {
        case 0x2000:case 0x2001:case 0x210D:case 0x210F:case 0x2110:case 0x2111:case 0x2112:
        case 0x2180:case 0x2200:case 0x2201:case 0x2209:case 0x220A:case 0x220B:break;
        default:assert(*gta5DrawRegister(d,r[0])==r[1]);++tableWords;
    }
    assert(tableWords==std::size(NativeFilterWeightsRegisters)-13);
    // RB_BLENDCONTROL0: colour src ONE/fn ADD/dst ONE, alpha src ONE/fn ADD/dst ONE.
    {const auto w=*gta5DrawRegister(d,0x2201);
     assert((w&31)==1 && ((w>>5)&7)==0 && ((w>>8)&31)==1 && ((w>>16)&31)==1 && ((w>>21)&7)==0 && ((w>>24)&31)==1);}
    assert((d.scissorRegisters==std::optional{std::array<unsigned,3>{0,0,0x005A00A0}}));
    assert(d.textures[0].fetch==(std::array<std::uint32_t,6>{0x80C04956,0x0654605D,0x0005804F,0x00A80D11,3,0x210}));
    assert(b.surfaces[0].object==0x102711D0 && be32(*b.surfaces[0].header,24)==0x028000A0 &&
        be32(*b.surfaces[0].header,28)==0x00030300 && be32(*b.surfaces[0].header,36)==0x027C02C8);
    for(unsigned i=1;i<5;++i)assert(!b.surfaces[i].object);
    assert(d.deviceState[11068]==0 && be32(d.deviceState,12408)==0);
    const char* why=nullptr;auto plan=planNativePostfxUpsampleDraw(d,b,p,p.vertexIdentity,p.pixelIdentity,false,true,&why);
    if(!plan)std::printf("POSTFX_UPSAMPLE_PLAN_REFUSED %s\n",why);
    assert(plan && plan->target.width==160 && plan->target.height==90 && plan->target.pitch==160 &&
        plan->target.baseTile==768 && plan->target.tileCount==12 && plan->target.format==3 &&
        plan->target.object==0x102711D0 && nativePostfxUpsampleInstanceTarget(plan->target) &&
        // The 160x90 base768 target is also the second blur row's target (T4/T1
        // share base768); the planners stay distinct by program and source size.
        nativePostfxBlurInstanceTarget(plan->target) &&
        plan->submitted.width==80 && plan->submitted.height==45 && plan->submitted.pitchTexels==96 &&
        plan->submitted.baseAddress==0x06546000 && plan->storage.sourceBytes==49152 &&
        plan->storage.description.format==32);
    assert((160/80)*((90+15)/16)==12); // 5 full tile rows + rows 80..89 of the 6th
    assert(std::string(why)=="postfx-upsample-state-resources-planned-not-executed");
    assert(plan->viewport==*nativeTranslatedViewport(d));
    // The shader reads no constants: neither bank is admission state.
    {auto q=d;q.pixelConstants[189*4]=0;q.pixelConstants[255*4+3]^=1;q.vertexConstants[7]^=1;assert(check(q,b,p));}
    unsigned negatives=0;
    for(auto r:NativeFilterWeightsRegisters){auto q=d;q.deviceState[*gta5DrawRegisterOffset(r[0])+3]^=1;assert(!check(q,b,p));++negatives;}
    for(unsigned i=0;i<44;++i){
        auto q=d;auto bound=b;auto pr=p;bool idle=true,indexed=false;
        if(i==0)idle=false;if(i==1)indexed=true;if(i==2)q.textures[0].source.pop_back();
        if(i==3)q.textures[0].fetch[1]^=1;if(i==4)q.textures[0].fetch[3]&=~(1u<<19);
        if(i==5)q.textures[0].fetch[3]&=~(1u<<21);if(i==6)q.textures[0].slot=4;
        if(i==7)q.textures.push_back(q.textures[0]);if(i==8)q.vertices[0]^=1;
        if(i==9)pr.vertexIdentity[0]^=1;if(i==10)pr.pixelIdentity[0]^=1;
        if(i==11)pr.abiVersion=41;if(i==12)pr.abiVersion=42;if(i==13)pr.abiVersion=43;if(i==14)pr.abiVersion=45;
        if(i==15)pr.interpolators=0;if(i==16)bound.surfaces[0].header->at(24)^=1;
        if(i==17)bound.surfaces[0].header->at(36)^=1;if(i==18)bound.surfaces[1].object=0x1000;
        if(i==19)q.vertexShader^=4;if(i==20)q.pixelShader^=4;if(i==21)q.scissorRegisters.reset();
        if(i==22)(*q.scissorRegisters)[2]^=1;if(i==23)q.deviceState[11068]|=0x10;
        if(i==24)q.deviceState[12411]=1;                        // surface-mode word 12408
        if(i==25)q.textures[0].fetch[0]=0x81404956;             // pitch160 (the T1 texture word)
        if(i==26)q.textures[0].fetch[0]=0x80804956;             // pitch64 < width
        if(i==27)q.textures[0].fetch[0]=0x80C00156;             // T2 resolve destination word, not the sampled one
        if(i==28)q.textures[0].fetch[2]=0x000B209F;             // 160x90 extent (same size, not an upsample)
        if(i==29)q.textures[0].fetch[2]=0x0005A04F;             // 80x46 extent
        if(i==30)q.textures[0].literalZero=true;if(i==31)q.textures[0].fetch[4]^=1;if(i==32)q.textures[0].fetch[5]^=1;
        if(i==33)pr.textureSlot=4;if(i==34)pr.textureSlots={5,5};
        if(i==35){auto extra=q.textures[0];extra.slot=1;q.textures.push_back(extra);}
        if(i==36)pr.pixelIdentity.pop_back();if(i==37)pr.stride=36;
        if(i==38)bound.surfaces[0].header.reset();if(i==39)q.deviceState.pop_back();
        if(i==40)q.vertexCount=4;if(i==41)q.primitive=4;
        if(i==42)q.textures[0].fetch[1]=(q.textures[0].fetch[1]&0xFFFu)|0x1FFFF000u; // storage past 512MB
        if(i==43)bound.surfaces[0].object=0x10271250;           // T2's object, not the bound one
        assert(!check(q,bound,pr,idle,indexed));++negatives;
    }
    // The current pixel shader must be exactly this program's; the same-size ABI41
    // copy (and any other 60-byte program) is refused by identity, not size.
    {auto other=p.pixelIdentity;other[59]^=0x80;
     assert(!planNativePostfxUpsampleDraw(d,b,p,p.vertexIdentity,other,false,true,&why));
     assert(std::string(why)=="shader-identity-interface");++negatives;}
    for(unsigned format:{6u,7u,14u}){auto q=d;auto bound=b;
        q.deviceState[*gta5DrawRegisterOffset(0x2001)+1]=format;
        bound.surfaces[0].header->at(29)=format;assert(!check(q,bound,p));++negatives;}
    const auto be=[](auto& b,unsigned at,unsigned v){for(unsigned i=0;i<4;++i)b[at+i]=v>>(24-8*i);};
    const auto reg=[&](auto& q,unsigned r,unsigned v){be(q.deviceState,*gta5DrawRegisterOffset(r),v);};
    // Replace blending (ABI41/42's 0x00010001) on every control, and on each one alone.
    unsigned blendNegatives=0;
    {auto q=d;for(unsigned r:{0x2201u,0x2209u,0x220Au,0x220Bu})reg(q,r,0x00010001);
     assert(!check(q,b,p) && !planNativePostfxUpsampleDraw(q,b,p,p.vertexIdentity,p.pixelIdentity,false,true,&why));
     assert(std::string(why)=="additive-blend-profile");++blendNegatives;}
    for(unsigned r:{0x2201u,0x2209u,0x220Au,0x220Bu}) {
        for(unsigned v:{0x00010001u,0x00010101u,0x01010001u,0x01000101u,0x01010102u,0x01010121u,0x05040504u}) {
            auto q=d;reg(q,r,v);assert(!planNativePostfxUpsampleDraw(q,b,p,p.vertexIdentity,p.pixelIdentity,false,true,&why));
            assert(std::string(why)=="additive-blend-profile");++blendNegatives;
        }
    }
    // ABI41's and ABI42's band words on the ABI44 capture.
    {auto q=d;reg(q,0x210D,0x00FFFFFE);assert(!check(q,b,p));++negatives;}         // ABI41 stencil ref 0xFE
    {auto q=d;reg(q,0x2180,0x10010501);assert(!check(q,b,p));++negatives;}         // ABI42 six PS GPRs
    {auto q=d;reg(q,0x2200,0x00700760);assert(!check(q,b,p));++negatives;}         // ABI35 depth compare
    {auto q=d;reg(q,0x2104,0x00000007);assert(!check(q,b,p));++negatives;}         // RGB-only colour mask
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
    const Shape observed{80,45,160,90,768,0x028000A0,0x00030300,0x027C02C8,0x42A00000,0xC2340000,0x42340000,0x80C04956,0x0005804F,49152};
    {auto [q,bound]=make(observed);q.textures[0].source=d.textures[0].source;
     assert(q.deviceState==d.deviceState && *bound.surfaces[0].header==*b.surfaces[0].header &&
         q.scissorRegisters==d.scissorRegisters && q.textures[0].fetch==d.textures[0].fetch && check(q,bound,p));}
    unsigned rowNegatives=0;
    const auto refuse=[&](const Shape& z){auto [q,bound]=make(z);assert(!check(q,bound,p));++rowNegatives;};
    // ABI41 row0 (A 320x180 -> T1 160x90, halving) is not an upsample.
    refuse({320,180,160,90,768,0x028000A0,0x00030300,0x027C02C8,0x42A00000,0xC2340000,0x42340000,0x82804956,0x0016613F,491520});
    // Same-size 160x90 -> 160x90 (a predicted pass81 row).
    refuse({160,90,160,90,768,0x028000A0,0x00030300,0x027C02C8,0x42A00000,0xC2340000,0x42340000,0x81404956,0x000B209F,122880});
    // ABI42's observed row (T2 80x45 -> T3 80x45 base780).
    refuse({80,45,80,45,780,0x01800050,0x0003030C,0x013C0160,0x42200000,0xC1B40000,0x41B40000,0x80C04956,0x0005804F,49152});
    // Predicted later pass83 row (T4 160x90 -> A 320x180), self-consistent words: NOT admitted until captured.
    // Row1 (160x90 -> 320x180 base720, observed 1if67slu): authoring it from the 80x45 capture plans.
    {auto [q,bound]=make({160,90,320,180,720,0x05000140,0x000302D0,0x04FC0598,0x43200000,0xC2B40000,0x42B40000,0x81404956,0x000B209F,122880});
     const auto r1=check(q,bound,p);assert(r1 && r1->target.width==320 && r1->target.baseTile==720 && r1->target.tileCount==48);}
    auto z=observed;z.base=780;z.color=0x0003030C;refuse(z);                 // row at T2's base780
    z=observed;z.info=0x018000A0;refuse(z);                                  // hiz pitch96 (not the observed160)
    z=observed;z.info=0x02800050;refuse(z);                                  // surface pitch80
    z=observed;z.ys=0x42340000;refuse(z);                                    // Y not flipped
    z=observed;z.xs=0x42200000;refuse(z);                                    // 80-wide viewport on the 160x90 target
    z=observed;z.dims=0x013C0160;refuse(z);                                  // 80x45 surface dims
    z=observed;z.h=96;z.dims=0x027C02F8;z.ys=0xC2400000;z.yo=0x42400000;refuse(z); // 160x96 target
    z=observed;z.bytes=49151;refuse(z);                                      // truncated owned source
    z=observed;z.f0=0x80004956;refuse(z);                                    // pitch0 source
    z=observed;z.f0=0x81004956;refuse(z);                                    // pitch128 source
    {auto [q,bound]=make(observed);(*q.scissorRegisters)[2]=(90<<16)|80;assert(!check(q,bound,p));++rowNegatives;}
    // Neighbouring planners cannot take the upsample (forced identities; state refuses).
    unsigned crossBand=0;
    {auto p41=p;p41.abiVersion=41;assert(validateNativeExpanded16CopyProgram(p41));
     assert(!planNativeExpanded16CopyDraw(d,b,p41,p41.vertexIdentity,p41.pixelIdentity,false,true,&why));
     assert(std::string(why)=="raster-profile");++crossBand;}
    {auto p42=p;p42.abiVersion=42;p42.pixelIdentity.resize(192);assert(validateNativePostfxBlurProgram(p42));
     assert(!planNativePostfxBlurDraw(d,b,p42,p42.vertexIdentity,p42.pixelIdentity,false,true,&why));
     assert(std::string(why)=="raster-profile");++crossBand;}
    if(argc>=3) {
        // ABI41 row0 (bloom-downsample v572): the same T1 object/header/scissor/viewport.
        auto g=loadExpanded16CopyFixture(argv[2],read);auto& d1=g.draw;auto& b1=g.bound;auto& p1=g.program;
        p1.vertexCode=p1.pixelCode=p.vertexCode;
        assert(p1.vertexIdentity==p.vertexIdentity && p1.pixelIdentity!=p.pixelIdentity &&
            p1.pixelIdentity.size()==p.pixelIdentity.size());      // Shared post VS; same-size PS.
        assert(b1.surfaces[0].object==b.surfaces[0].object && *b1.surfaces[0].header==*b.surfaces[0].header);
        assert(planNativeExpanded16CopyDraw(d1,b1,p1,p1.vertexIdentity,p1.pixelIdentity,false,true));
        // ABI44 planner on the ABI41 capture: identity, then (identity forced) its stencil
        // word, then (stencil forced) its replace blend, then (blend forced) its 320x180 source.
        assert(!planNativePostfxUpsampleDraw(d1,b1,p,p.vertexIdentity,p1.pixelIdentity,false,true,&why));
        assert(std::string(why)=="shader-identity-interface");++crossBand;
        auto q=d1;assert(!planNativePostfxUpsampleDraw(q,b1,p,p.vertexIdentity,p.pixelIdentity,false,true,&why));
        assert(std::string(why)=="raster-profile");++crossBand;
        reg(q,0x210D,0x00FFFF00);assert(!planNativePostfxUpsampleDraw(q,b1,p,p.vertexIdentity,p.pixelIdentity,false,true,&why));
        assert(std::string(why)=="additive-blend-profile");++crossBand;
        for(unsigned r:{0x2201u,0x2209u,0x220Au,0x220Bu})reg(q,r,0x01010101);
        assert(!planNativePostfxUpsampleDraw(q,b1,p,p.vertexIdentity,p.pixelIdentity,false,true,&why));
        assert(std::string(why)=="submitted-linear-profile");++crossBand;
        // ABI41's planner on the ABI44 capture with ABI41's stencil word: still refused (blend).
        auto p41=p1;auto u=d;reg(u,0x210D,0x00FFFFFE);
        assert(!planNativeExpanded16CopyDraw(u,b,p41,p41.vertexIdentity,p41.pixelIdentity,false,true,&why));++crossBand;
    }
    if(argc>=4) {
        // ABI42 pass81 (postfx-blur v612): T3 at base780, pitch80 -> no ABI44 row.
        auto g=loadPostfxBlurFixture(argv[3],read);auto& d2=g.draw;auto& b2=g.bound;
        assert(g.program.vertexIdentity==p.vertexIdentity && g.program.pixelIdentity.size()==192);
        assert(!planNativePostfxUpsampleDraw(d2,b2,p,p.vertexIdentity,g.program.pixelIdentity,false,true,&why));
        assert(std::string(why)=="shader-identity-interface");++crossBand;
        assert(!planNativePostfxUpsampleDraw(d2,b2,p,p.vertexIdentity,p.pixelIdentity,false,true,&why));
        assert(std::string(why)=="raster-profile");++crossBand;
    }
    // CPU ONE/ONE boundary (recordCompletedNativeRgb7e3AdditiveBlend) on the planned target.
    const auto t=plan->target;
    std::vector<float> source(160*90*4);
    for(unsigned y=0;y<90;++y)for(unsigned x=0;x<160;++x){auto* s=&source[(y*160+x)*4];
        s[0]=float(x+1)/64;s[1]=float(y+1)/32;s[2]=float((x*7+y*3)%97)/8;s[3]=0;}
    const auto words=[&](const std::vector<std::uint32_t>& w){std::vector<std::uint8_t> r(w.size()*4);
        for(std::size_t i=0;i<w.size();++i)for(unsigned c=0;c<4;++c)r[i*4+c]=w[i]>>(c*8);return r;};
    const auto sum=[&](std::uint32_t dst,const float* s){ // Independent: exact double sum of exact decodes.
        std::array<std::uint32_t,4> bits{};
        for(unsigned c=0;c<3;++c) {
            const unsigned code=(dst>>(10*c))&1023,e=code>>7,m=code&127;
            const double value=e?std::ldexp(double(128+m),int(e)-10):std::ldexp(double(m),-9);
            bits[c]=colorBits(float(value+double(s[c])));
        }
        bits[3]=colorBits(float(dst>>30)/3+s[3]);
        unsigned packed=0;assert(packColor7e3(bits,packed,Color7e3Rounding::XeniaShiftRound));return packed;
    };
    unsigned blendChecks=0;
    {   // T1 after its resolve's recorded clear: 160x96, all 12 tiles known C0000000.
        NativeEdram32Bands owner;
        assert(owner.recordCompletedRawColorRows(768,160,160,96,words(std::vector<std::uint32_t>(160*96,0xC0000000u)),1));
        {NativeEdram32Mutation tx(owner);
         assert(recordCompletedNativeRgb7e3AdditiveBlend(tx,t,source,Color7e3Rounding::XeniaShiftRound,&why));tx.commit();}
        const auto after=owner.readKnownColorRows(768,160,160,96);assert(after && owner.knownTiles()==12);
        for(unsigned y=0;y<96;++y)for(unsigned x=0;x<160;++x) {
            const auto w=(*after)[y*160+x];
            if(y>=90){assert(w==0xC0000000u);continue;}                 // rows 90..95 untouched, still known
            const auto* s=&source[(y*160+x)*4];
            unsigned alone=0;assert(packColor7e3({colorBits(s[0]),colorBits(s[1]),colorBits(s[2]),colorBits(1.0f)},alone,Color7e3Rounding::XeniaShiftRound));
            assert(w==alone && w>>30==3 && w==sum(0xC0000000u,s));      // rgb = source, alpha = 1.0
        }
        ++blendChecks;
    }
    {   // Nonzero destination with only rows 0..89 known (ABI41's own footprint).
        NativeEdram32Bands owner;std::vector<std::uint32_t> prior(160*90);
        for(unsigned i=0;i<prior.size();++i)prior[i]=((i*2654435761u)&0x3FFFFFFFu)|(unsigned(i%4)<<30);
        prior[0]=1023|(1023u<<10)|(1023u<<20)|(3u<<30);                  // saturated destination
        assert(owner.recordCompletedRawColorRows(768,160,160,90,words(prior),1) && owner.knownTiles()==10);
        {NativeEdram32Mutation tx(owner);
         assert(recordCompletedNativeRgb7e3AdditiveBlend(tx,t,source,Color7e3Rounding::XeniaShiftRound,&why));tx.commit();}
        const auto after=owner.readKnownColorRows(768,160,160,90);assert(after);
        for(unsigned i=0;i<prior.size();++i)assert((*after)[i]==sum(prior[i],&source[i*4]));
        assert((*after)[0]==prior[0]);                                  // 31.875 + x saturates to 1023
        assert(owner.knownTiles()==10 && !owner.readKnownColorRows(768,160,160,91)); // rows 90..95 stay unknown
        ++blendChecks;
    }
    unsigned blendRefusals=0;
    const auto refuseBlend=[&](NativeEdram32Bands owner,const NativeOffscreenDrawPlan& target,const std::vector<float>& rgba,const char* reason){
        const auto serial=owner.nextSerial();
        {NativeEdram32Mutation tx(owner);
         assert(!recordCompletedNativeRgb7e3AdditiveBlend(tx,target,rgba,Color7e3Rounding::XeniaShiftRound,&why));
         assert(std::string(why)==reason && tx.candidate().nextSerial()==serial);}
        assert(!owner.knownTiles());++blendRefusals;                    // uncommitted -> unknown
    };
    NativeEdram32Bands cleared;
    assert(cleared.recordCompletedRawColorRows(768,160,160,96,words(std::vector<std::uint32_t>(160*96,0xC0000000u)),1));
    {auto hole=cleared;assert(hole.invalidate(779,1));refuseBlend(hole,t,source,"destination-colour-unknown");}
    {auto hole=cleared;assert(hole.invalidate(768,1));refuseBlend(hole,t,source,"destination-colour-unknown");}
    {NativeEdram32Bands partial;                                      // one row short of the rectangle
     assert(partial.recordCompletedRawColorRows(768,160,160,89,words(std::vector<std::uint32_t>(160*89,0xC0000000u)),1));
     refuseBlend(partial,t,source,"destination-colour-unknown");}
    refuseBlend(NativeEdram32Bands{},t,source,"destination-colour-unknown");
    {auto bad=source;bad[5*4+1]=std::numeric_limits<float>::infinity();refuseBlend(cleared,t,bad,"nonfinite-additive-blend");}
    {auto bad=source;bad[7*4+2]=std::numeric_limits<float>::quiet_NaN();refuseBlend(cleared,t,bad,"nonfinite-additive-blend");}
    {auto bad=source;bad.pop_back();refuseBlend(cleared,t,bad,"additive-blend-input");}
    {auto wide=t;wide.format=7;refuseBlend(cleared,wide,source,"additive-blend-input");}
    {auto off=t;off.tileCount=11;refuseBlend(cleared,off,source,"additive-blend-input");}
    std::printf("POSTFX_UPSAMPLE_PLAN_PASS actual_state=1 abi=44 target=160x90 format=3 physical_base=768 tiles=12 hiz=160 "
                "source=80x45_linear_pitch96 upsample=2x blend=add blend_control=01010101 negatives=%u "
                "blend_negatives=%u gpu=0 gameplay=0\n",negatives,blendNegatives);
    std::printf("POSTFX_UPSAMPLE_ROW_TABLE_PASS rows=2 observed_row_authored_equals_captured=1 refused_rows=%u "
                "cross_band_refusals=%u gpu=0 gameplay=0\n",rowNegatives,crossBand);
    std::printf("POSTFX_UPSAMPLE_ADDITIVE_BOUNDARY_PASS cases=%u refusals=%u onto_recorded_clear=1 partial_tile_rows_preserved=1 "
                "unknown_destination_refused=1 nonfinite_refused=1 saturation=1 gpu=0 gameplay=0\n",blendChecks,blendRefusals);
    if(argc==5) {
        // Row1 OBSERVED: pass83 at 160x90 -> 320x180 (1if67slu) plans exactly with the ABI44 program.
        auto g=loadPostfxUpsampleFixture(argv[4],read);auto& d5=g.draw;auto& b5=g.bound;auto& p5=g.program;
        p5.vertexCode=p5.pixelCode=p.vertexCode;
        assert(p5.vertexIdentity==p.vertexIdentity && p5.pixelIdentity==p.pixelIdentity);
        const auto r=planNativePostfxUpsampleDraw(d5,b5,p5,p5.vertexIdentity,p5.pixelIdentity,false,true,&why);
        if(!r)std::printf("POSTFX_UPSAMPLE_ROW1_REFUSED %s\n",why);
        assert(r && r->target.width==320 && r->target.height==180 && r->target.pitch==320 && r->target.baseTile==720 &&
            r->target.tileCount==48 && r->target.object==0x10271110 && r->submitted.baseAddress==0x0650A000 &&
            r->submitted.width==160 && r->submitted.pitchTexels==160 && r->storage.sourceBytes==122880);
        unsigned row1=0;
        const auto refuse5=[&](auto q,auto bound){assert(!planNativePostfxUpsampleDraw(q,bound,p5,p5.vertexIdentity,p5.pixelIdentity,false,true));++row1;};
        for(auto reg:NativeFilterWeightsRegisters){auto q=d5;q.deviceState[*gta5DrawRegisterOffset(reg[0])+3]^=1;refuse5(q,b5);}
        {auto q=d5;q.textures[0].source.pop_back();refuse5(q,b5);}
        {auto q=d5;q.textures[0].fetch[0]=0x80C04956;refuse5(q,b5);}      // 80x45 source pitch
        {auto bound=b5;bound.surfaces[0].header->at(36)^=1;refuse5(d5,bound);}
        {auto q=d5;(*q.scissorRegisters)[2]=0x005A00A0;refuse5(q,b5);}     // 160x90 scissor
        std::printf("POSTFX_UPSAMPLE_ROW1_CAPTURE_PASS actual_state=1 target=320x180 physical_base=720 tiles=48 source=T1_0650A000_pitch160 "
                    "negatives=%u gpu=0 gameplay=0\n",row1);
    }
}