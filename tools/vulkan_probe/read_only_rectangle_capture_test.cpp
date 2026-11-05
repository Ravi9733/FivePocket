// Exact saved no-pixel rectangle -> no-output proof -> physical owner preservation.
// HOST ONLY. Private identity is optional for authored cases, mandatory for the
// actual-capture marker. No query, draw, installation or gameplay claim.
#include "native_read_only_rectangle_plan.h"
#include "native_edram32_bands.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <string_view>
#ifdef RECOMP_PRIVATE_SCREEN_PACK
#include RECOMP_PRIVATE_SCREEN_PACK
#endif
using namespace recomp::gpu;
namespace {
void put(std::vector<std::uint8_t>& b,unsigned at,std::uint32_t value) {
    assert(at+4<=b.size());for(unsigned n=0;n<4;++n)b[at+n]=value>>(24-8*n);
}
void reg(Gta5InlineDraw& d,unsigned r,unsigned v){put(d.deviceState,*gta5DrawRegisterOffset(r),v);}
Gta5InlineDraw authored(bool notEqualSeven=false) {
    Gta5InlineDraw d;d.primitive=8;d.vertexCount=3;d.stride=24;d.deviceState.resize(13860);
    constexpr unsigned values[][2]{{0x2000,0x0A020280},{0x2002,0x10000},{0x2104,0},
        {0x210D,0x00FF0701},{0x210F,0x43A00000},{0x2110,0x43A00000},
        {0x2111,0xC3340000},{0x2112,0x43340000},{0x2113,0xBF800000},{0x2114,0x3F800000},
        {0x2180,0x10000},{0x2181,0},{0x2182,0},{0x2183,0},{0x2184,0},
        {0x2200,0x00700275},{0x2202,0xAA000005},{0x2203,0x134},{0x2204,0x80000},
        {0x2205,0x18002},{0x2206,0x43F},{0x2293,0x10},{0x2302,4},
        {0x231C,0},{0x2324,2},{0x2325,0}};
    for(const auto& v:values)reg(d,v[0],v[1]);
    if(notEqualSeven) {
        reg(d,0x2200,0x00700575);reg(d,0x210D,0x00FF0707);
        reg(d,0x2182,0x00030003);reg(d,0x2203,0x714);
    }
    d.scissorRegisters=std::array<std::uint32_t,3>{0,0,(360u<<16)|640};return d;
}
NativeTranslatedDrawProgram authoredProgram() {
    NativeTranslatedDrawProgram p;p.abiVersion=26;p.screenSpaceRectangle=true;p.compositeRectangle=true;
    p.stride=24;p.textureSlot=2;p.textureSlots={2};return p;
}
void check(const Gta5InlineDraw& draw,const NativeTranslatedDrawProgram& program,bool captured) {
    const auto initial=draw.deviceState;const char* why=nullptr;
    const auto dc=*gta5DrawRegister(draw,0x2200),ref=*gta5DrawRegister(draw,0x210D);
    const bool notEqualSeven=dc==0x00700575;
    assert(planNativeReadOnlyScreenRectangle(draw,program,0,true,&why));
    assert(std::string_view(why)==(notEqualSeven?"read-only-depth-stencil-not-equal-seven":"read-only-depth-stencil"));
    assert(draw.deviceState==initial);
    assert(!planNativeReadOnlyScreenRectangle(draw,program,0,false));
    assert(!planNativeReadOnlyScreenRectangle(draw,program,0x1000,true));
    for(unsigned bit=0;bit<32;++bit) {
        auto d=draw;reg(d,0x2200,dc^(1u<<bit));
        assert(!planNativeReadOnlyScreenRectangle(d,program,0,true));
        d=draw;reg(d,0x210D,ref^(1u<<bit));
        assert(!planNativeReadOnlyScreenRectangle(d,program,0,true));
    }
    for(const auto r:{0x2104u,0x2180u,0x2181u,0x2182u,0x2183u,0x2184u,
            0x2202u,0x2203u,0x2293u,0x231Cu,0x2324u,0x2325u}) {
        auto d=draw;reg(d,r,*gta5DrawRegister(d,r)^1u);
        assert(!planNativeReadOnlyScreenRectangle(d,program,0,true));
    }
    for(unsigned at:{12816u,12820u,12824u,12828u,13068u}) {
        auto d=draw;put(d.deviceState,at,0x1000);
        assert(!planNativeReadOnlyScreenRectangle(d,program,0,true));
    }
    {auto d=draw;d.pixelShader=0x1000;assert(!planNativeReadOnlyScreenRectangle(d,program,0,true));}
    {auto d=draw;d.primitive=4;assert(!planNativeReadOnlyScreenRectangle(d,program,0,true));}
    {auto d=draw;d.vertexCount=6;assert(!planNativeReadOnlyScreenRectangle(d,program,0,true));}
    {auto p=program;p.abiVersion=20;assert(!planNativeReadOnlyScreenRectangle(draw,p,0,true));}
    {auto d=draw;d.textures.emplace_back();assert(!planNativeReadOnlyScreenRectangle(d,program,0,true));}
    {auto d=draw;d.scissorRegisters.reset();assert(!planNativeReadOnlyScreenRectangle(d,program,0,true));}
    // Changing any nonempty proper subset of the observed tuple is NOT another
    // supported profile, even if an isolated field would still be read-only.
    const auto other=authored(!notEqualSeven);
    constexpr unsigned profileRegs[]{0x2200,0x210D,0x2182,0x2203};
    for(unsigned mask=1;mask<15;++mask) {
        auto d=draw;
        for(unsigned i=0;i<4;++i)if(mask&(1u<<i))reg(d,profileRegs[i],*gta5DrawRegister(other,profileRegs[i]));
        assert(!planNativeReadOnlyScreenRectangle(d,program,0,true));
    }
    const auto state=decodeXenosDepthStencilState(dc,ref,0);assert(state);
    for(unsigned stencil=0;stencil<256;++stencil)for(bool back:{false,true})
        for(float incoming:{0.0f,0.25f,1.0f})for(float stored:{0.0f,0.75f,1.0f}) {
            const auto result=evaluateXenosDepthStencil(*state,incoming,stored,stencil,back);
            assert(!result.depthWritten && result.stencil==stencil);
            assert(result.colorPass==(notEqualSeven?((stencil&7)!=7):((stencil&7)==1)));
            // Test result exists; output writes do not.
        }
    NativeEdram32Bands owner;std::vector<std::uint32_t> words(80*16);
    for(unsigned n=0;n<words.size();++n)words[n]=(n*7919u)&0xFFFFFFFFu;
    assert(owner.recordCompletedDepthWords(0,80,80,16,words,1));
    const auto serial=owner.nextSerial();
    bool knownDisplay=true;knownDisplay=false; // Production dispatch entry.
    {NativeEdram32Mutation mutation(owner);assert(owner.knownTiles()==0);
     assert(finishNativeReadOnlyDraw(planNativeReadOnlyScreenRectangle(draw,program,0,true),mutation,&knownDisplay));}
    assert(knownDisplay);
    assert(owner.knownTiles()==1 && owner.nextSerial()==serial && owner.readDepthWords(0,80,80,16)==words);
    // No valid-on-entry display receipt: success must not invent one.
    bool unknownDisplay=false;
    {NativeEdram32Mutation mutation(owner);assert(finishNativeReadOnlyDraw(true,mutation));}
    assert(!unknownDisplay && owner.knownTiles()==1);
    // A failed admission must not accidentally restore owner validity.
    knownDisplay=false;
    {NativeEdram32Mutation mutation(owner);
     assert(!finishNativeReadOnlyDraw(planNativeReadOnlyScreenRectangle(draw,program,0,false),mutation,&knownDisplay));}
    assert(!knownDisplay);
    assert(owner.knownTiles()==0 && !owner.readDepthWords(0,80,80,16));
    std::printf("READ_ONLY_RECTANGLE_PROOF_PASS captured=%u stencil_values=256 both_faces=1 depth_pairs=9 "
        "query_owner_guard=1 mutations_reject=1 physical_owner_preserved=1 display_owner_preserved=1 "
        "gpu=0 gameplay=0 not_equal_seven=%u mixed_profiles_reject=14\n",unsigned(captured),unsigned(notEqualSeven));
}
}
int main(int argc,char** argv) {
    assert(argc==1 || argc==2);check(authored(),authoredProgram(),false);
    check(authored(true),authoredProgram(),false);
    if(argc==2) {
#ifdef RECOMP_PRIVATE_SCREEN_PACK
        const std::string folder=argv[1];
        const auto read=[&](const char* name,unsigned size) {
            std::ifstream f(folder+"/"+name,std::ios::binary|std::ios::ate);assert(f && f.tellg()==size);
            f.seekg(0);std::vector<std::uint8_t> b(size);
            if(size)assert(f.read(reinterpret_cast<char*>(b.data()),size));return b;
        };
        auto draw=authored();draw.deviceState=read("device-state.bin",13860);draw.vertices=read("vertices.bin",72);
        const auto meta=read("replay-metadata.bin",32);
        const auto word=[&](unsigned at){return (std::uint32_t{meta[at]}<<24)|(std::uint32_t{meta[at+1]}<<16)|
            (std::uint32_t{meta[at+2]}<<8)|meta[at+3];};
        draw.primitive=word(0);draw.vertexCount=word(4);draw.stride=word(8);
        assert(word(12)==0 && word(16)==0);draw.pixelShader=word(16);
        draw.scissorRegisters=std::array<std::uint32_t,3>{word(20),word(24),word(28)};
        assert(read("vertex-shader.bin",72)==nativeScreenSpaceProgram.draw.vertexIdentity);
        assert(read("pixel-shader.bin",0).empty());
        check(draw,nativeScreenSpaceProgram.draw,true);
#else
        (void)argv;assert(false && "private analysed identity is required for captured-state evidence");
#endif
    }
}
