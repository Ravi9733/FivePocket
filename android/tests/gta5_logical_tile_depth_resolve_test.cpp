#include "gta5_logical_tile_depth_resolve.h"
#include "gta5_logical_tile_resolve_fixture.h"
#include <cstdio>
#include <limits>
using namespace recomp::gpu;using namespace recomp::gpu::test;
static AuthoredTileResolve fixture() {
    auto f=authoredTileResolve();f.request.flags=0x14;f.request.clearColor=0;f.arguments.color.reset();
    auto words=f.destination.fetch;words[1]=(words[1]&~63u)|23u;words[3]|=1;
    f.destination=*XenosTextureDescription::decode(words);(*f.surfaces.state)[11071]|=2;return f;
}
static auto plan(const AuthoredTileResolve& f) {
    return planGta5LogicalTileDepthResolve(f.surfaces,f.request,f.arguments,f.destination);
}
int main() {
    unsigned cases=0,negative=0;
    for(unsigned flags:{0u,0x40u,0x80u,0xC0u}) {
        auto f=fixture();(*f.surfaces.state)[11072]|=flags;const auto before=*f.surfaces.state;
        const auto p=planGta5LogicalTileDepthResolve(f.surfaces,f.request,f.arguments,f.destination,
            Gta5ResolveSynchronization::FencedNativeImages);
        assert(p && p->geometry.requiredCacheMaintenance==flags && *f.surfaces.state==before);
        assert(flags?!plan(f):bool(plan(f)));
        (*f.surfaces.state)[11071]|=0x20;
        assert(!planGta5LogicalTileDepthResolve(f.surfaces,f.request,f.arguments,f.destination,
            Gta5ResolveSynchronization::FencedNativeImages));
    }
    std::puts("LOGICAL_TILE_DEPTH_SYNC_PASS profiles=4 default_guard_retained=1 query_guard_retained=1 captured_flags_unchanged=1");
    for(unsigned endian=0;endian<4;++endian)for(unsigned shape=0;shape<3;++shape) {
        auto f=fixture();auto fetch=f.destination.fetch;fetch[1]|=endian<<6;f.destination=*XenosTextureDescription::decode(fetch);
        const unsigned top=shape?384:0;tileWord(*f.arguments.rectangle,4,top);tileWord(*f.arguments.rectangle,12,top+16);
        tileWord(*f.arguments.rectangle,0,32);tileWord(*f.arguments.rectangle,8,48);
        tileWord(*f.arguments.point,0,64);tileWord(*f.arguments.point,4,shape==2?0:top);
        tileWord(*f.surfaces.state,13084,shape?12:3);
        const auto p=plan(f);assert(p && p->geometry.slot==4 && p->geometry.regions.size()==1);
        assert(!tileResolvePlan(f)); // Never admit depth through the color API.
        assert(!planXenosTexture2D(f.destination)); // Legacy RGBA sampling stays strict.
        const auto& r=p->geometry.regions[0];assert(r.source[1]==top && r.destination[1]==(shape==2?0:top));
        std::vector<float> depth(256);std::vector<std::uint8_t> stencil(256);
        for(unsigned i=0;i<256;++i){depth[i]=*decodeXenosFloat24((i*65537u)%0xF00001u);stencil[i]=i;}
        const auto& storage=p->geometry.storage;std::vector<std::uint8_t> before(storage.sourceBytes,0xA5);
        const auto packed=packGta5LogicalTileDepthResolveRegion(*p,r,depth,stencil,before);assert(packed);
        auto full=*linearizeXenosTexture2D(storage.description,before);
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
            const auto i=y*16+x,word=(((i*65537u)%0xF00001u)<<8)|stencil[i];
            for(unsigned b=0;b<4;++b)full[((r.destination[1]+y)*storage.description.width+r.destination[0]+x)*4+b]=word>>(b*8);
        }
        // Compare every destination/padding byte, not only the copied rectangle.
        assert(packed==storeXenosTexture2D(storage.description,full,before));
        const auto decoded=decodeXenosDepth24FloatTexture(f.destination,*packed);assert(decoded);
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
            float value;std::memcpy(&value,decoded->data()+((r.destination[1]+y)*storage.description.width+r.destination[0]+x)*4,4);
            assert(value==depth[y*16+x]);
        }
        auto shortSource=*packed;shortSource.pop_back();assert(!decodeXenosDepth24FloatTexture(f.destination,shortSource));
        const auto originalDepth=depth[0];
        for(float bad:{0.1f,-1.f,1.1f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
            depth[0]=bad;assert(!packGta5LogicalTileDepthResolveRegion(*p,r,depth,stencil,before));++negative;
        }
        depth[0]=originalDepth;stencil.pop_back();assert(!packGta5LogicalTileDepthResolveRegion(*p,r,depth,stencil,before));
        ++cases;
    }
    for(unsigned fault=0;fault<20;++fault) {
        auto f=fixture();auto words=f.destination.fetch;
        switch(fault) {
        case 0:f.request.flags=4;break;case 1:f.request.flags|=0x200;break;
        case 2:f.request.flags=0x10;break;case 3:f.request.mip=1;break;case 4:f.request.slice=1;break;
        case 5:words[1]=(words[1]&~63u)|22u;break;case 6:words[0]&=~0x80000000u;break;
        case 7:words[5]|=0x800;break;case 8:words[5]|=0x1000000;break;case 9:words[4]|=64;break;
        case 10:words[0]|=4;break;case 11:words[3]^=4;break;case 12:words[3]|=1u<<13;break;
        case 13:f.surfaces.surfaces[4].header.reset();break;case 14:tileWord(*f.surfaces.state,13088,1);break;
        case 15:(*f.surfaces.state)[11071]|=0x20;break;case 16:f.arguments.stack92.reset();break;
        case 17:tileWord(*f.arguments.stack100,0,1);break;case 18:f.surfaces.surfaces[4].tileCache+=64;break;
        case 19:tileWord(*f.arguments.rectangle,12,383);break;
        }
        f.destination=*XenosTextureDescription::decode(words);assert(!plan(f));++negative;
    }
    std::printf("LOGICAL_TILE_DEPTH_RESOLVE_PASS cases=%u negatives=%u exact_depth_stencil=1 all_endians=1 neighboring_bytes_preserved=1 color_guard_unchanged=1 publication=0\n",cases,negative);
}
