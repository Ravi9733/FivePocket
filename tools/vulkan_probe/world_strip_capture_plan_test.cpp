// Original saved-input state/samplers -> strict ABI29 plan. No GPU execution.
#include "native_world_strip_capture.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#ifndef RECOMP_PRIVATE_WORLD_STRIP_POSITION_PACK
#error Exact private shader identities required
#endif
#include RECOMP_PRIVATE_WORLD_STRIP_POSITION_PACK
using namespace recomp::gpu;
int main(int argc,char** argv) {
    assert(argc==2);const std::string folder=argv[1];std::setvbuf(stdout,nullptr,_IONBF,0);
    const auto read=[&](const std::string& name,unsigned size) {
        std::ifstream f(folder+"/"+name,std::ios::binary|std::ios::ate);assert(f && f.tellg()==size);
        f.seekg(0);std::vector<std::uint8_t> b(size);assert(f.read(reinterpret_cast<char*>(b.data()),size));return b;
    };
    const auto be=[](const auto& b,unsigned at){return (std::uint32_t{b[at]}<<24)|
        (std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];};
    const auto put=[](auto& b,unsigned at,unsigned w){for(unsigned c=0;c<4;++c)b[at+c]=w>>(24-8*c);};
    Gta5InlineDraw draw;draw.primitive=6;draw.vertexCount=4;draw.stride=36;
    draw.vertices=read("vertices.bin",144);draw.deviceState=read("device-state.bin",13860);
    draw.vertexShader=be(draw.deviceState,13072);draw.pixelShader=be(draw.deviceState,13068);
    const auto scissor=read("scissor.bin",12);
    draw.scissorRegisters=std::array<unsigned,3>{be(scissor,0),be(scissor,4),be(scissor,8)};
    Gta5BoundSurfaceCapture bindings;
    for(unsigned i=0;i<5;++i)bindings.surfaces[i].object=be(draw.deviceState,12816+4*i);
    bindings.surfaces[0].header=read("surface-0-argument.bin",40);
    bindings.surfaces[4].header=read("surface-4-argument.bin",40);
    NativeWorldStripTextures textures;const auto meta=read("texture-meta.bin",144);
    constexpr unsigned slots[]{2,6,1,3};
    for(unsigned i=0;i<4;++i) {
        unsigned at=144;for(unsigned j=0;j<4;++j)if(be(meta,j*36)==slots[i])at=j*36;assert(at<144);
        auto& t=textures[i];for(unsigned j=0;j<6;++j)t.fetch[j]=be(meta,at+12+4*j);
        t.base=read("texture-"+std::to_string(slots[i])+".bin",be(meta,at+4));
        t.mips=read("texture-"+std::to_string(slots[i])+"-mips.bin",be(meta,at+8));
    }
    const auto vs=read("vertex-shader.bin",252),ps=read("pixel-shader.bin",912);
    unsigned reads=0,captureNegatives=0;
    const Gta5TextureFetchReader submitted=[&](unsigned slot)->std::optional<std::array<unsigned,6>> {
        for(unsigned i=0;i<4;++i)if(slots[i]==slot)return textures[i].fetch;
        return std::nullopt;
    };
    const Gta5ResourceReader graphics=[&](unsigned address,unsigned bytes)->std::optional<std::vector<std::uint8_t>> {
        ++reads;
        for(const auto& t:textures) {
            const auto d=*XenosTextureDescription::decode(t.fetch);
            if(address==d.baseAddress && bytes==t.base.size())return t.base;
            if(address==d.mipAddress && !t.mips.empty() && bytes==t.mips.size())return t.mips;
        }
        return std::nullopt;
    };
    const auto copied=captureNativeWorldStripTextures(submitted,graphics);assert(copied && reads==5);
    for(unsigned i=0;i<4;++i)assert((*copied)[i].fetch==textures[i].fetch &&
        (*copied)[i].base==textures[i].base && (*copied)[i].mips==textures[i].mips);
    for(unsigned missing:slots) {
        const Gta5TextureFetchReader reader=[&](unsigned slot){return slot==missing?std::optional<std::array<unsigned,6>>{}:submitted(slot);};
        assert(!captureNativeWorldStripTextures(reader,graphics));++captureNegatives;
    }
    for(unsigned failed=1;failed<=5;++failed)for(bool shortRead:{false,true}) {
        reads=0;
        const Gta5ResourceReader reader=[&](unsigned address,unsigned bytes)->std::optional<std::vector<std::uint8_t>> {
            auto result=graphics(address,bytes);
            if(reads==failed){if(!shortRead)return std::nullopt;assert(result);result->pop_back();}
            return result;
        };
        assert(!captureNativeWorldStripTextures(submitted,reader));++captureNegatives;
    }
    for(unsigned changed:slots) {
        const Gta5TextureFetchReader reader=[&](unsigned slot) {
            auto f=submitted(slot);if(slot==changed)(*f)[3]^=2;return f;
        };
        assert(!captureNativeWorldStripTextures(reader,graphics));++captureNegatives;
    }
    assert(!captureNativeWorldStripTextures({},graphics));++captureNegatives;
    assert(!captureNativeWorldStripTextures(submitted,{}));++captureNegatives;
    const auto original=textures[0].base[0];textures[0].base[0]^=1;
    assert((*copied)[0].base[0]==original);textures[0].base[0]=original;
    std::printf("WORLD_STRIP_OWNED_CAPTURE_PASS resources=4 planes=5 negatives=%u independent_copies=1\n",captureNegatives);
    const NativeWorldStripIdentity identity{worldStripPositions.vertexIdentity,worldStripPositions.pixelIdentity};
    const char* why=nullptr;const auto p=planNativeWorldStrip(draw,bindings,textures,identity,vs,ps,false,true,&why);
    if(!p)std::printf("WORLD_STRIP_PLAN_REFUSED stage=%s\n",why);
    assert(p && p->samplers[1]==(NativeTextureSamplerPlan{{2,2},1,1,0,0,2}) && p->samplers[2].maxLevel==3);
    unsigned negatives=0;
    const auto refuses=[&](const auto& d,const auto& b,const auto& t) {
        assert(!planNativeWorldStrip(d,b,t,identity,vs,ps,false,true));++negatives;
    };
    for(unsigned reg:{0x2000u,0x2001u,0x2002u,0x2104u,0x210Du,0x2113u,0x2114u,
        0x2180u,0x2181u,0x2182u,0x2183u,0x2184u,0x2200u,0x2201u,0x2202u,0x2203u,
        0x2204u,0x2205u,0x2206u,0x2293u,0x2300u,0x2301u,0x2302u,0x2307u,0x2308u,
        0x2312u,0x231Cu,0x2324u,0x2325u})for(unsigned bit=0;bit<32;++bit) {
        auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(reg),*gta5DrawRegister(d,reg)^(1u<<bit));refuses(d,bindings,textures);
    }
    for(unsigned field=0;field<10;++field) {
        auto d=draw;
        if(field==0)d.primitive=4;if(field==1)d.vertexCount=5;if(field==2)d.stride=32;
        if(field==3)d.vertices.pop_back();if(field==4)d.deviceState.pop_back();
        if(field==5)d.vertexShader^=4;if(field==6)d.pixelShader=0;if(field==7)d.scissorRegisters.reset();
        if(field==8)put(d.deviceState,12408,1);if(field==9)d.deviceState[11068]|=0x10;
        refuses(d,bindings,textures);
    }
    for(unsigned slot=0;slot<4;++slot) {
        auto t=textures;t[slot].base.pop_back();refuses(draw,bindings,t);
        t=textures;t[slot].mips.push_back(0);refuses(draw,bindings,t);
        for(unsigned word=0;word<6;++word)for(unsigned bit=0;bit<32;++bit) {
            if((word==1 || (word==5 && slots[slot]==1)) && bit>=12)continue;
            t=textures;t[slot].fetch[word]^=1u<<bit;refuses(draw,bindings,t);
        }
    }
    for(unsigned slot:{0u,4u}) {
        auto b=bindings;b.surfaces[slot].header.reset();refuses(draw,b,textures);
        b=bindings;b.surfaces[slot].object^=4;refuses(draw,b,textures);
        b=bindings;put(*b.surfaces[slot].header,28,be(*b.surfaces[slot].header,28)^0x10000);refuses(draw,b,textures);
    }
    assert(!planNativeWorldStrip(draw,bindings,textures,identity,vs,ps,true,true));++negatives;
    assert(!planNativeWorldStrip(draw,bindings,textures,identity,vs,ps,false,false));++negatives;
    auto wrong=ps;wrong[0]^=1;assert(!planNativeWorldStrip(draw,bindings,textures,identity,vs,wrong,false,true));++negatives;
    std::printf("WORLD_STRIP_CAPTURE_PLAN_PASS textures=4 constant_depth=1 negatives=%u native_execution=0\n",negatives);
}
