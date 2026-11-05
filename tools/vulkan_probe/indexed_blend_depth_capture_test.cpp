#include "indexed_blend_depth_fixture.h"
#include <cstdio>
using namespace recomp::gpu;
using namespace recomp::gpu::test;
int main(int argc,char** argv) {
    assert(argc==2);IndexedBlendDepthFixture f(argv[1]);unsigned negatives=0;
    const auto check=[&](const auto& p,const auto& d,const auto& indices,const auto& b,const auto& t,bool idle=true) {
        return planNativeIndexedBlendDepth(p,d,indices,b,t,f.program.vertexIdentity,f.program.pixelIdentity,idle);
    };
    const char* why=nullptr;
    const auto plan=planNativeIndexedBlendDepth(f.program,f.draw,f.indices,f.bound,f.textures,
        f.program.vertexIdentity,f.program.pixelIdentity,true,&why);
    if(!plan)std::printf("INDEXED_BLEND_DEPTH_PLAN_REFUSED stage=%s\n",why);
    assert(plan && f.draw.vertexCount==615 && f.indices.size()==3360);
    const auto put=[](auto& b,unsigned at,unsigned word){for(unsigned i=0;i<4;++i)b[at+i]=word>>(24-8*i);};
    for(unsigned r:{0x2000u,0x2001u,0x2002u,0x2104u,0x210Du,0x210Fu,0x2110u,0x2111u,0x2112u,0x2113u,0x2114u,
        0x2180u,0x2181u,0x2182u,0x2183u,0x2184u,0x2200u,0x2201u,0x2202u,0x2203u,0x2204u,0x2205u,0x2206u,
        0x2207u,0x2208u,0x2284u,0x2292u,0x2293u,0x2300u,0x2301u,0x2302u,0x2307u,0x2308u,0x2312u,0x231Cu,0x2324u,0x2325u}) {
        for(unsigned bit=0;bit<32;++bit) {
            auto d=f.draw;put(d.deviceState,*gta5DrawRegisterOffset(r),*gta5DrawRegister(d,r)^(1u<<bit));
            assert(!check(f.program,d,f.indices,f.bound,f.textures));++negatives;
        }
    }
    for(unsigned k=0;k<12;++k) {
        auto d=f.draw;
        if(k==0)d.primitive=6;if(k==1)d.stride=20;if(k==2)d.vertexCount=2;
        if(k==3)d.vertices.pop_back();if(k==4)d.deviceState.pop_back();
        if(k==5)d.vertexShader^=4;if(k==6)d.pixelShader=0;
        if(k==7)d.scissorRegisters.reset();if(k==8)put(d.deviceState,12408,0);
        if(k>=9)d.deviceState[k==9?11068:k==10?12498:12563]=1;
        assert(!check(f.program,d,f.indices,f.bound,f.textures));++negatives;
    }
    for(unsigned k=0;k<4;++k) {
        auto indices=f.indices;
        if(k==0)indices.clear();if(k==1)indices.pop_back();if(k==2)indices[0]=f.draw.vertexCount;
        if(k==3)indices.resize(65538);
        assert(!check(f.program,f.draw,indices,f.bound,f.textures));++negatives;
    }
    assert(!check(f.program,f.draw,f.indices,f.bound,f.textures,false));++negatives;
    for(unsigned k=0;k<9;++k) {
        auto p=f.program;if(k==0)p.pixelIdentity[0]^=1;if(k==1)p.vertexIdentity[0]^=1;
        if(k==2)p.abiVersion=13;if(k==3)p.geometryPixelEffects=true;if(k==4)p.geometryDepth20e4=true;
        if(k==5)p.pixelCode.clear();if(k==6)p.textureSlots={2,3,11};if(k==7)p.interpolators=9;if(k==8)p.indexed=false;
        assert(!check(p,f.draw,f.indices,f.bound,f.textures));++negatives;
    }
    for(unsigned slot=0;slot<3;++slot)for(unsigned k=0;k<8;++k) {
        auto t=f.textures;if(k<6)t[slot].fetch[k]^=1;
        if(k==6)t[slot].base.pop_back();if(k==7)t[slot].mips.push_back(0);
        assert(!check(f.program,f.draw,f.indices,f.bound,t));++negatives;
    }
    for(unsigned slot:{0u,4u})for(unsigned k=0;k<5;++k) {
        auto b=f.bound;
        if(k==0)b.surfaces[slot].header.reset();if(k==1)b.surfaces[slot].object^=4;
        if(k>=2)put(*b.surfaces[slot].header,k==2?24:k==3?28:36,0);
        assert(!check(f.program,f.draw,f.indices,b,f.textures));++negatives;
    }
    const Gta5TextureFetchReader submitted=[&](unsigned slot)->std::optional<std::array<unsigned,6>> {
        for(unsigned i=0;i<3;++i)if(f.program.textureSlots[i]==slot)return f.textures[i].fetch;return std::nullopt;
    };
    unsigned reads=0;
    const Gta5ResourceReader reader=[&](unsigned address,unsigned size)->std::optional<std::vector<std::uint8_t>> {
        ++reads;for(const auto& t:f.textures) {
            const auto d=XenosTextureDescription::decode(t.fetch);
            if(d && d->baseAddress==address && size==t.base.size())return t.base;
        }
        return std::nullopt;
    };
    const auto capture=captureNativeIndexedBlendDepthTextures(submitted,reader);assert(capture && reads==3);
    for(unsigned i=0;i<3;++i)assert((*capture)[i].base==f.textures[i].base && (*capture)[i].fetch==f.textures[i].fetch);
    for(unsigned failSlot:{3u,2u,11u}) {
        assert(!captureNativeIndexedBlendDepthTextures([&](unsigned slot){return slot==failSlot?std::nullopt:submitted(slot);},reader));++negatives;
        const auto failAddress=XenosTextureDescription::decode(*submitted(failSlot))->baseAddress;
        assert(!captureNativeIndexedBlendDepthTextures(submitted,[&](unsigned a,unsigned n) {
            auto b=reader(a,n);if(a==failAddress && b)b->pop_back();return b;
        }));++negatives;
    }
    std::printf("INDEXED_BLEND_DEPTH_CAPTURE_HOST_PASS vertices=615 indices=3360 textures=3 negatives=%u original_storage_connected=1 gpu=0\n",negatives);
}
