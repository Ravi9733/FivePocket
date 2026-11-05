#include "native_expanded16_luma_plan.h"
#include <cassert>
#include <cstdio>
#include <fstream>
using namespace recomp::gpu;
std::vector<std::uint8_t> read(const std::string& p,unsigned n) {
    std::ifstream f(p,std::ios::binary|std::ios::ate);assert(f && f.tellg()==n);f.seekg(0);
    std::vector<std::uint8_t> b(n);assert(f.read(reinterpret_cast<char*>(b.data()),n));return b;
}
unsigned word(const auto& b,unsigned n){return (unsigned{b[n]}<<24)|(unsigned{b[n+1]}<<16)|(unsigned{b[n+2]}<<8)|b[n+3];}
int main(int argc,char** argv) {
    assert(argc==2);const std::string p=argv[1];Gta5InlineDraw d;
    d.primitive=8;d.vertexCount=3;d.stride=24;d.vertices=read(p+"/vertices.bin",72);
    d.deviceState=read(p+"/device-state.bin",13860);d.vertexShader=word(d.deviceState,13072);d.pixelShader=word(d.deviceState,13068);
    const auto meta=read(p+"/capture-meta.bin",36);
    d.scissorRegisters=std::array<unsigned,3>{word(meta,24),word(meta,28),word(meta,32)};
    Gta5InlineDraw::Texture t;t.slot=0;for(unsigned i=0;i<6;++i)t.fetch[i]=word(meta,i*4);
    t.source=read(p+"/source.bin",491520);d.textures={t};
    Gta5BoundSurfaceCapture b;for(unsigned i=0;i<5;++i)b.surfaces[i].object=word(d.deviceState,12816+i*4);
    b.surfaces[0].header=read(p+"/surface-0-resource.bin",40);
    NativeTranslatedDrawProgram program;program.abiVersion=37;program.stride=24;program.interpolators=1;
    program.textureSlot=0;program.textureSlots={0};program.vertexIdentity=read(p+"/vertex-shader.bin",96);
    program.pixelIdentity=read(p+"/pixel-shader.bin",324);program.vertexCode=program.pixelCode={0x07230203,0,0,0,0};
    const char* why=nullptr;
    const auto plan=planNativeExpanded16LumaDraw(d,b,program,program.vertexIdentity,program.pixelIdentity,false,true,&why);
    if(!plan)std::printf("LUMA_PLAN_REFUSED %s\n",why);
    assert(plan && plan->target.width==64 && plan->target.height==64 && plan->target.pitch==80 &&
        plan->target.baseTile==1440 && plan->target.tileCount==4 && plan->target.format==14 && plan->submitted.fetch==t.fetch);
    unsigned heapReads=0,vertexReads=0,textureReads=0,otherReads=0;
    const unsigned device=0x1004CD00,vertexAddress=word(d.deviceState,13848),base=t.fetch[1]&0xFFFFF000;
    const Gta5ResourceReader heap=[&](auto a,auto n)->std::optional<std::vector<std::uint8_t>> {
        if(a==device && n==13860){++heapReads;return d.deviceState;}++otherReads;return std::nullopt;
    };
    bool shortTexture=false;
    const Gta5ResourceReader data=[&](auto a,auto n)->std::optional<std::vector<std::uint8_t>> {
        if(a==vertexAddress && n==72){++vertexReads;return d.vertices;}
        if(a==base && n==t.source.size()){++textureReads;auto bytes=t.source;if(shortTexture)bytes.pop_back();return bytes;}
        ++otherReads;return std::nullopt;
    };
    const Gta5TextureFetchReader submitted=[&](auto slot)->std::optional<std::array<unsigned,6>> {
        assert(slot==0);return t.fetch;
    };
    auto captured=captureNativeExpanded16LumaDraw(device,8,3,24,heap,data,submitted);
    assert(captured && heapReads==1 && vertexReads==1 && textureReads==1 && !otherReads);
    assert(captured->textures.size()==1 && captured->textures[0].fetch==t.fetch && captured->textures[0].source==t.source);
    captured->scissorRegisters=d.scissorRegisters;
    assert(planNativeExpanded16LumaDraw(*captured,b,program,program.vertexIdentity,program.pixelIdentity,false,true));
    shortTexture=true;assert(!captureNativeExpanded16LumaDraw(device,8,3,24,heap,data,submitted));
    assert(!captureNativeExpanded16LumaDraw(device,8,3,24,heap,data,{}));
    assert(!captureNativeExpanded16LumaDraw(device,4,3,24,heap,data,submitted));
    unsigned negatives=0;
    for(auto r:NativeFilterWeightsRegisters) {
        auto bad=d;bad.deviceState[*gta5DrawRegisterOffset(r[0])+3]^=1;
        assert(!planNativeExpanded16LumaDraw(bad,b,program,program.vertexIdentity,program.pixelIdentity,false,true));++negatives;
    }
    for(unsigned i=0;i<14;++i) {
        auto bad=d;auto bound=b;auto pr=program;auto vs=program.vertexIdentity;bool idle=true,indexed=false;
        if(i==0)idle=false;if(i==1)indexed=true;if(i==2)bad.textures[0].source.pop_back();
        if(i==3)bad.textures[0].fetch[1]^=1;if(i==4)bad.vertices[0]^=1;if(i==5)vs[0]^=1;
        if(i==6)pr.abiVersion=36;if(i==7)bound.surfaces[0].header->at(24)^=1;
        if(i==8)bound.surfaces[1].object=0x1000;if(i==9)bad.vertexShader^=4;
        if(i==10)bad.textures.push_back(t);if(i==11)bad.scissorRegisters.reset();
        if(i==12)bad.textures[0].fetch[3]|=1<<19;if(i==13)pr.textureSlot=5;
        assert(!planNativeExpanded16LumaDraw(bad,bound,pr,vs,program.pixelIdentity,indexed,idle));++negatives;
    }
    std::puts("EXPANDED16_LUMA_CAPTURE_CONNECTION_PASS actual_state=1 captured_pixels=1 exact_owned_reads=1 short_read_refuses=1");
    std::printf("EXPANDED16_LUMA_PLAN_PASS current_state=1 captured_pixels=1 target=64x64 pitch=80 base=1440 r32=1 negatives=%u gpu=0 completion=0\n",negatives);
}
