#include "gta5_logical_tile_resolve_fixture.h"
#include <cstdio>

int main() {
    using namespace recomp::gpu;using namespace recomp::gpu::test;
    unsigned cases=0,negative=0;
    for(unsigned slot=0;slot<4;++slot)for(unsigned mode:{0u,0x10u})for(unsigned clear:{0u,0x100u})
    for(unsigned select:{0u,1u,2u,3u,4u,8u,12u,15u})for(unsigned shape=0;shape<3;++shape) {
        auto f=authoredTileResolve();f.request.flags=slot|mode|clear;
        tileWord(*f.surfaces.state,13084,select);
        const std::array<unsigned,4> rect=shape==0?std::array<unsigned,4>{0,0,1280,720}:
            shape==1?std::array<unsigned,4>{0,384,1280,720}:std::array<unsigned,4>{32,352,128,416};
        for(unsigned i=0;i<4;++i)tileWord(*f.arguments.rectangle,i*4,rect[i]);
        tileWord(*f.arguments.point,0,shape==2?64:rect[0]);
        tileWord(*f.arguments.point,4,shape==2?320:rect[1]);
        const auto p=tileResolvePlan(f);assert(p && p->slot==slot && bool(p->clearAfterCopy)==bool(clear));
        const unsigned expected=((select&3) && shape!=1?1u:0u)|(select&12?2u:0u);
        assert(p->tiles==expected);
        for(const auto& r:p->regions) {
            assert(r.destination[0]==r.source[0]+(shape==2?32u:0u));
            assert(r.destination[1]==r.source[1]-(shape==2?32u:0u));
            assert(r.source[1]>=p->layout.tiles[r.tile].logical[1]);
            assert(r.source[3]<=p->layout.tiles[r.tile].logical[3]);
        }
        ++cases;
    }
    // Bottom stripe copied to top-left: subtract the source origin, don't add
    // the bin origin twice. Null destination point has the same zero meaning.
    auto f=authoredTileResolve();tileWord(*f.surfaces.state,13084,12);
    tileWord(*f.arguments.rectangle,4,384);tileWord(*f.arguments.rectangle,12,720);
    f.request.destinationPoint=0;f.arguments.point.reset();
    auto p=tileResolvePlan(f);assert(p && p->regions.size()==1);
    (*f.surfaces.state)[11071]|=2;
    const auto prepared=tileResolvePlan(f);assert(prepared && prepared->rectangle==p->rectangle && prepared->tiles==p->tiles);
    auto clearValidation=f.surfaces;clearValidation.call.flags=0;
    assert(!planGta5LogicalTileClear(clearValidation)); // Clear admission remains unchanged.
    for(const auto [offset,bit]:std::array<std::array<unsigned,2>,6>{{{11068,0x10},{11068,0x40},{11068,0x80},{11071,0x20},{11072,0x40},{11072,0x80}}}) {
        auto bad=f;(*bad.surfaces.state)[offset]|=bit;assert(!tileResolvePlan(bad));
    }
    for(unsigned flags:{0u,0x40u,0x80u,0xC0u}) {
        auto dirty=f;(*dirty.surfaces.state)[11072]|=flags;const auto before=*dirty.surfaces.state;
        const auto synchronized=planGta5LogicalTileResolve(dirty.surfaces,dirty.request,dirty.arguments,dirty.destination,
            Gta5ResolveSynchronization::FencedNativeImages);
        assert(synchronized && synchronized->requiredCacheMaintenance==flags && synchronized->tiles==p->tiles);
        assert(*dirty.surfaces.state==before && (flags?!tileResolvePlan(dirty):bool(tileResolvePlan(dirty))));
        (*dirty.surfaces.state)[11071]|=0x20;
        assert(!planGta5LogicalTileResolve(dirty.surfaces,dirty.request,dirty.arguments,dirty.destination,
            Gta5ResolveSynchronization::FencedNativeImages));
    }
    std::puts("LOGICAL_TILE_RESOLVE_SYNC_PASS profiles=4 default_guard_retained=1 query_guard_retained=1 captured_flags_unchanged=1");
    assert((p->regions[0].source==std::array<unsigned,4>{0,384,1280,720}));
    assert((p->regions[0].destination==std::array<unsigned,4>{0,0,1280,336}));
    f=authoredTileResolve();f.request.rectangle=0;f.arguments.rectangle.reset();
    p=tileResolvePlan(f);assert(p && p->rectangle[3]==720 && p->regions.size()==1);
    // Empty selection: valid zero work, never initialize other bins.
    tileWord(*f.surfaces.state,13084,0);assert(tileResolvePlan(f)->regions.empty());
    // Each negative starts with an otherwise valid connected shape.
    for(unsigned fault=0;fault<24;++fault) {
        f=authoredTileResolve();
        switch(fault) {
        case 0:f.request.flags=4;break;case 1:f.request.flags=0x150;break;
        case 2:f.request.flags=0x310;break;case 3:f.request.mip=1;break;
        case 4:f.request.slice=1;break;case 5:f.arguments.stack100.reset();break;
        case 6:tileWord(*f.arguments.stack92,0,1);break;case 7:f.request.stack=UINT32_MAX-32;break;
        case 8:f.surfaces.surfaces[0].header.reset();break;case 9:tileWord(*f.surfaces.state,13088,1);break;
        case 10:tileWord(*f.arguments.rectangle,12,383);break;case 11:tileWord(*f.arguments.rectangle,8,1288);break;
        case 12:tileWord(*f.arguments.point,4,384);break;case 13:tileWord(*f.arguments.point,0,1);break;
        case 14:f.request.clearColor+=4;break;case 15:f.arguments.color->pop_back();break;
        case 16:tileWord(*f.arguments.color,0,0x7FC00000);break;case 17:f.arguments.rectangle.reset();break;
        case 18:f.arguments.point.reset();break;case 19:f.surfaces.surfaces[1].tileCache+=64;break;
        case 20:tileWord(*f.surfaces.state,12816,0);break;case 21:f.request.flags|=0x1000;break;
        case 22:f.request.rectangle=UINT32_MAX-7;break;case 23:f.request.destinationPoint=UINT32_MAX-3;break;
        }
        assert(!tileResolvePlan(f));++negative;
    }
    // Exact region bytes and all surrounding allocation/padding bytes, every
    // endian mode, identity/RB swap and sampling alpha-one swizzle.
    for(unsigned endian=0;endian<4;++endian)for(unsigned swizzle:{0xD10u,0xC14u,0x1510u}) {
        f=authoredTileResolve();auto fetch=f.destination.fetch;fetch[1]|=endian<<6;fetch[3]=swizzle;
        f.destination=*XenosTextureDescription::decode(fetch);
        tileWord(*f.arguments.rectangle,0,32);tileWord(*f.arguments.rectangle,4,32);
        tileWord(*f.arguments.rectangle,8,48);tileWord(*f.arguments.rectangle,12,48);
        tileWord(*f.arguments.point,0,64);tileWord(*f.arguments.point,4,64);
        p=tileResolvePlan(f);assert(p && p->regions.size()==1);const auto& d=p->storage.description;
        const std::vector<std::uint8_t> before(p->storage.sourceBytes,0xA5);
        std::vector<std::uint8_t> rgba(16*16*4);for(unsigned i=0;i<rgba.size();++i)rgba[i]=i%251;
        const auto packed=packGta5LogicalTileResolveRegion(*p,p->regions[0],rgba,before);assert(packed);
        auto expected=*linearizeXenosTexture2D(d,before);
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)for(unsigned c=0;c<4;++c) {
            const auto from=d.swizzle[0]==2 && (c==0 || c==2)?2-c:c;
            expected[((y+64)*d.width+x+64)*4+c]=rgba[(y*16+x)*4+from];
        }
        assert(packed==storeXenosTexture2D(d,expected,before));
        rgba.pop_back();assert(!packGta5LogicalTileResolveRegion(*p,p->regions[0],rgba,before));
    }
    std::printf("LOGICAL_TILE_RESOLVE_PLAN_PASS cases=%u rejected=%u packing=12 source_origin_once=1 publication=0\n",cases,negative);
}
