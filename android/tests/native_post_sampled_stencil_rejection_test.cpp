// Authored identities and physical words test the contract, NOT title execution.
#include "native_post_sampled_stencil_rejection.h"
#include <cassert>
#include <cstdio>
#include <string_view>
using namespace recomp::gpu;
int main() {
    const auto put=[](auto& b,unsigned at,unsigned value){for(unsigned i=0;i<4;++i)b[at+i]=value>>(24-8*i);};
    Gta5InlineDraw d;d.primitive=8;d.vertexCount=3;d.stride=24;d.vertices.resize(72);
    d.deviceState.resize(13860);d.vertexShader=0x1000;d.pixelShader=0x2000;
    put(d.deviceState,13072,d.vertexShader);put(d.deviceState,13068,d.pixelShader);
    d.scissorRegisters=std::array<std::uint32_t,3>{0,0,(384u<<16)|1280};
    constexpr unsigned registers[][2]{
        {0x2000,0x14000500},{0x2001,0x302D0},{0x2002,0x10000},{0x2104,15},
        {0x210D,0x1010},{0x210E,0},
        {0x210F,0x44200000},{0x2110,0x44200000},{0x2111,0xC3B40000},
        {0x2112,0x43B40000},{0x2113,0xBF800000},{0x2114,0x3F800000},
        {0x2180,0x10050701},{0x2181,0x106},{0x2182,0x10001},{0x2183,0},{0x2184,0},
        {0x2200,0x00700571},{0x2201,0x10001},{0x2202,0xAA000003},{0x2203,0x1018},
        {0x2204,0x80000},{0x2205,0x18000},{0x2206,0x43F},
        {0x2293,0x14},{0x2302,5},{0x231C,0},{0x2324,2},{0x2325,0}};
    for(const auto& r:registers)put(d.deviceState,*gta5DrawRegisterOffset(r[0]),r[1]);
    Gta5BoundSurfaceCapture b;
    for(unsigned i:{0u,4u}) {
        auto& s=b.surfaces[i];s.object=0x3000+256*i;s.header=std::vector<std::uint8_t>(40);
        put(d.deviceState,12816+4*i,s.object);
        put(*s.header,24,0x14000500);put(*s.header,28,i==0?0x302D0:0x10000);
        put(*s.header,36,((1280-1)<<18)|((720-1)<<3));
    }
    const NativePostSampledStencilIdentity identity{std::vector<std::uint8_t>(96,17),std::vector<std::uint8_t>(324,19)};
    NativeEdram32Bands owner;NativePostSampledStencilProof proof;const char* why=nullptr;
    const auto check=[&](const auto& draw,const auto& bindings,bool indexed=false,bool idle=true) {
        return proveNativePostSampledStencilRejection(draw,bindings,identity,identity.vertex,identity.pixel,
            indexed,idle,owner,proof,&why);
    };
    assert(!check(d,b) && std::string_view(why)=="physical-stencil-unknown");
    std::vector<std::uint32_t> raw(1280*720),color(raw.size());
    for(unsigned i=0;i<raw.size();++i) {
        raw[i]=(i<<8)|((i&255)&~16u)|(i<1280*384?16u:0u);
        color[i]=0xCA000000u^i;
    }
    assert(owner.recordCompletedDepthWords(0,1280,1280,720,raw,owner.nextSerial()));
    std::vector<std::uint8_t> rgba(color.size()*4);
    for(unsigned i=0;i<color.size();++i)for(unsigned byte=0;byte<4;++byte)rgba[4*i+byte]=color[i]>>(8*byte);
    assert(owner.recordCompletedRawColorRows(720,1280,1280,720,rgba,owner.nextSerial()));
    assert(check(d,b) && proof.examined==491520 && proof.passing==0);
    assert(proof.ownerNextSerial==owner.nextSerial());
    const auto serial=owner.nextSerial();
    // Outside the scissor is deliberately passing and must remain untouched.
    for(unsigned at:{0u,1280u*192,1280u*384-1}) {
        auto one=raw;one[at]&=~16u;
        assert(owner.recordCompletedDepthWords(0,1280,1280,720,one,owner.nextSerial()));
        assert(!check(d,b) && proof.passing==1 && proof.examined==491520);
    }
    assert(owner.recordCompletedDepthWords(0,1280,1280,720,raw,owner.nextSerial()));
    assert(owner.nextSerial()>serial);
    for(unsigned tile:{0u,383u,384u,719u}) {
        auto unknown=owner;assert(unknown.invalidate(tile,1));
        assert(!proveNativePostSampledStencilRejection(d,b,identity,identity.vertex,identity.pixel,
            false,true,unknown,proof,&why) && std::string_view(why)=="physical-stencil-unknown");
    }
    // The original caller restores a different scissor before repeating the
    // pass. Arbitrary bounded scissor proof is stronger than a fixed first-run
    // rectangle: all current samples are scanned, never inferred from history.
    {auto full=d;(*full.scissorRegisters)[2]=(720u<<16)|1280;
        assert(!check(full,b) && proof.passing==430080 && proof.examined==921600);
        auto all=raw;for(auto& w:all)w|=16;
        assert(owner.recordCompletedDepthWords(0,1280,1280,720,all,owner.nextSerial()));
        assert(check(full,b) && proof.passing==0 && proof.examined==921600);
        assert(owner.recordCompletedDepthWords(0,1280,1280,720,raw,owner.nextSerial()));}
    {auto narrow=d;narrow.scissorRegisters=std::array<unsigned,3>{0,(3u<<16)|1,(383u<<16)|1279};
        assert(check(narrow,b) && proof.examined==1278*380 && !proof.passing);}
    for(const auto& r:registers) {
        auto bad=d;put(bad.deviceState,*gta5DrawRegisterOffset(r[0]),r[1]^1u);assert(!check(bad,b));
    }
    for(unsigned at:{12408u,12816u,12820u,12832u,13068u,13072u}) {
        auto bad=d;bad.deviceState[at+3]^=4;assert(!check(bad,b));
    }
    {auto bad=d;bad.scissorRegisters.reset();assert(!check(bad,b));}
    for(auto scissor:std::array<std::array<unsigned,3>,5>{{{1,0,25167104},{0,0,0},
            {0,0,(721u<<16)|1280},{0,0,(384u<<16)|1281},{0,5,4}}}) {
        auto bad=d;bad.scissorRegisters=scissor;assert(!check(bad,b));}
    {auto bad=d;bad.deviceState[11068]|=16;assert(!check(bad,b));}
    {auto bad=d;bad.textures.emplace_back();assert(!check(bad,b));}
    {auto bad=d;bad.vertices.pop_back();assert(!check(bad,b));}
    {auto bad=d;bad.deviceState.pop_back();assert(!check(bad,b));}
    {auto bad=d;bad.stride=36;assert(!check(bad,b));}
    {auto bad=d;bad.vertexCount=6;assert(!check(bad,b));}
    {auto bad=d;bad.primitive=4;assert(!check(bad,b));}
    assert(!check(d,b,true) && !check(d,b,false,false));
    for(unsigned i:{0u,4u}) {
        {auto bad=b;bad.surfaces[i].header.reset();assert(!check(d,bad));}
        {auto bad=b;bad.surfaces[i].header->pop_back();assert(!check(d,bad));}
        for(unsigned at:{24u,28u,36u}){auto bad=b;(*bad.surfaces[i].header)[at]^=1;assert(!check(d,bad));}
    }
    for(unsigned stage=0;stage<2;++stage){auto v=identity.vertex,p=identity.pixel;(stage?p:v)[0]^=1;
        assert(!proveNativePostSampledStencilRejection(d,b,identity,v,p,false,true,owner,proof,&why));}
    // Use the same transaction semantics as the production read-only path.
    const auto before=owner.nextSerial();bool display=true;
    {NativeEdram32Mutation mutation(owner);
        assert(finishNativeReadOnlyDraw(proveNativePostSampledStencilRejection(d,b,identity,identity.vertex,
            identity.pixel,false,true,mutation.candidate(),proof,&why),mutation,&display));}
    assert(display && owner.nextSerial()==before && owner.readDepthWords(0,1280,1280,720)==raw &&
        owner.readColorWords(720,1280,1280,720)==color);
    // The dispatcher invalidates display before entering any draw attempt;
    // finishNativeReadOnlyDraw restores it only on success, not on failure.
    display=false;
    {NativeEdram32Mutation mutation(owner);assert(!finishNativeReadOnlyDraw(false,mutation,&display));}
    assert(!display && owner.knownTiles()==0);
    std::puts("POST_SAMPLED_STENCIL_HOST_PASS authored_identity=1 examined=491520 passing=0 single_passing_refuses=1 query_unknown_refuses=1 outside_preserved=1 native_admission=0");
}
