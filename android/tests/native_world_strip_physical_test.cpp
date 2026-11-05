// Authored physical-owner producer/consumer contract. No game/GPU claim.
#include "native_world_strip_physical.h"
#include <cassert>
#include <cstdio>
#include <limits>
using namespace recomp::gpu;
int main() {
    constexpr unsigned pixels=1280*720;
    std::vector<std::uint8_t> color(pixels*4);std::vector<unsigned> depth(pixels);
    const auto put=[](auto& b,unsigned at,unsigned v){for(unsigned c=0;c<4;++c)b[at+c]=v>>(8*c);};
    for(unsigned i=0;i<pixels;++i) {
        put(color,4*i,(i%1024)*0x100401u|((i%4)<<30));
        depth[i]=((i%3==0?0u:i%3==1?0x400u:0x401u)<<8)|(i%255+1);
    }
    NativeWorldStripPlan p;p.colorObject=0x1000;p.depthObject=0x2000;
    p.depthStencil=*decodeXenosDepthStencilState(0x708767,0xFFFF00,0);
    const auto seed=[&]{NativeEdram32Bands o;
        assert(o.recordCompletedRawColorRows(720,1280,1280,720,color,1));
        assert(o.recordCompletedDepthWords(0,1280,1280,720,depth,2));
        assert(o.recordCompletedRawColorRows(1500,80,80,16,std::vector<std::uint8_t>(5120,0xA5),3));return o;};
    auto owner=seed();const auto original=owner.readColorWords(720,1280,1280,720);
    const auto unrelated=owner.readColorWords(1500,80,80,16);
    for(unsigned mode=0;mode<2;++mode) {
        owner=seed();const auto serial=owner.nextSerial();
        NativeEdram32Mutation mutation(owner);
        assert(executeNativeWorldStripPhysical(mutation,p,[&](const auto& rgba,const auto& words) {
            assert(!owner.knownTiles());NativeWorldStripReadback out{rgba,words};
            if(mode)for(unsigned i=0;i<pixels;++i)if(i%3<2) {
                out.rgba[i*4]=5;out.rgba[i*4+1]=.25f;out.rgba[i*4+2]=-1;out.rgba[i*4+3]=1.5f;
                out.depthStencil[i]=0x40000;
            }
            return std::optional{out};
        }));
        const auto colors=owner.readColorWords(720,1280,1280,720),words=owner.readDepthWords(0,1280,1280,720);
        assert(colors && words && owner.nextSerial()==serial+2 && owner.readColorWords(1500,80,80,16)==unrelated);
        unsigned expected=0;assert(packColor7e3({colorBits(5),colorBits(.25f),colorBits(-1),colorBits(1.5f)},expected));
        for(unsigned i=0;i<pixels;++i) {
            assert((*colors)[i]==(mode && i%3<2?expected:(*original)[i]));
            assert((*words)[i]==(mode && i%3<2?0x40000:depth[i]));
        }
    }
    unsigned negatives=0;
    for(unsigned mode=0;mode<13;++mode) {
        owner=seed();auto plan=p;unsigned calls=0;
        if(mode==0)assert(owner.invalidate(0,1));
        if(mode==1)assert(owner.invalidate(720,1));
        if(mode==2)plan.depthObject=plan.colorObject;
        if(mode==3)plan.depthStencil.depthWrite=false;
        {NativeEdram32Mutation mutation(owner);
        assert(!executeNativeWorldStripPhysical(mutation,plan,[&](const auto& rgba,const auto& words)
                ->std::optional<NativeWorldStripReadback> {
            ++calls;NativeWorldStripReadback out{rgba,words};
            if(mode==4)return std::nullopt;
            if(mode==5)out.rgba.pop_back();if(mode==6)out.depthStencil.pop_back();
            if(mode==7)out.depthStencil[0]=0x40001; // Wrong stencil reference.
            if(mode==8)out.depthStencil[0]=0x50000; // Wrong encoded depth.
            if(mode==9)out.depthStencil[2]=0x40000; // GEQUAL must reject.
            if(mode==10)out.rgba[0]=3; // Colour changes without depth/stencil.
            if(mode==11)out.rgba[0]=std::numeric_limits<float>::quiet_NaN();
            if(mode==12)out.rgba[3]=std::numeric_limits<float>::infinity();
            return out;
        }));}
        assert(calls==(mode<4?0u:1u) && !owner.knownTiles());++negatives;
    }
    // Every original7e3/A2 code round-trips, including alpha1/3 and2/3.
    for(unsigned a=0;a<4;++a)for(unsigned code=0;code<1024;++code) {
        unsigned raw=code*0x100401u|(a<<30),back=0;
        assert(packColor7e3(unpackColor7e3(raw),back) && back==raw);
    }
    std::printf("WORLD_STRIP_PHYSICAL_HOST_PASS negatives=%u roundtrips=4096 pixels=%u gpu=0\n",negatives,pixels);
}
