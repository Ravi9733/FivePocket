#include "gta5_logical_tile_clear_fixture.h"
#include <cstdio>
#include <limits>
int main() {
    using namespace recomp::gpu;using namespace recomp::gpu::test;
    auto c=authoredLogicalClear();auto p=planGta5LogicalTileClear(c);assert(p && p->aspects==63 && p->tiles==3);
    unsigned cases=0;
    for(unsigned flags=0;flags<64;++flags)for(unsigned select:{0u,1u,2u,3u,4u,8u,12u,15u,UINT32_MAX}) {
        c=authoredLogicalClear();c.call.flags=flags;tileWord(*c.state,13084,select);
        p=planGta5LogicalTileClear(c);assert(p && p->aspects==flags);
        const unsigned expected=(select&3?1u:0u)|(select&12?2u:0u);
        assert(p->tiles==(flags?expected:0u));++cases;
    }
    c=authoredLogicalClear();c.call.flags=0x30;c.color.reset();tileWord(*c.state,13084,3);
    p=planGta5LogicalTileClear(c);assert(p && p->aspects==0x30 && p->tiles==1 && p->depth==0.5f && p->stencil==37);
    c.call.flags=0x20;c.call.depthBits=UINT64_MAX;assert(planGta5LogicalTileClear(c)); // Unused depth is not read.
    c=authoredLogicalClear();c.call.flags=1;c.call.stencil=UINT32_MAX;c.call.depthBits=UINT64_MAX;
    p=planGta5LogicalTileClear(c);assert(p && p->colors[0]==(std::array<std::uint8_t,4>{64,128,191,255}));
    tileWord(*c.state,10372,0x10180);tileWord(*c.surfaces[0].header,28,0x10180);
    p=planGta5LogicalTileClear(c);assert(p && p->colors[0]==(std::array<std::uint8_t,4>{128,192,223,255}));
    // Shader-prepared flags do not change surface identity. They still reject
    // actual clear admission, even for a zero-aspect clear.
    for(unsigned flag=0;flag<256;++flag) {
        auto prepared=authoredLogicalClear();(*prepared.state)[11071]=flag;
        const auto original=*prepared.state;
        const auto s=describeGta5LogicalTileSurfaces(prepared);assert(s && s->boundAspects==63);
        assert(s->layout.flags3==flag && *prepared.state==original);
        for(unsigned aspects:{0u,1u,48u,63u}) {
            prepared.call.flags=aspects;
            assert(bool(planGta5LogicalTileClear(prepared))==!(flag&0x22));
        }
        prepared.surfaces[2].header.reset();assert(!describeGta5LogicalTileSurfaces(prepared));
    }
    c=authoredLogicalClear();clearRectangle(c,{0,370,1280,400});
    tileFloat(*c.state,13024,9.75f);tileFloat(*c.state,13032,1000.9f);
    tileWord(*c.state,12288,1);for(unsigned i=0;i<4;++i)tileWord(*c.state,13052+i*4,std::array<unsigned,4>{13,373,1007,390}[i]);
    p=planGta5LogicalTileClear(c);assert(p && p->tiles==3 && p->rectangle==(std::array<unsigned,4>{13,373,1007,390}));
    clearRectangle(c,{0,720,1280,721});p=planGta5LogicalTileClear(c);assert(p && p->empty());
    c=authoredLogicalClear();tileWord(*c.state,12828,0);tileWord(*c.state,13116,0);c.surfaces[3]={};
    c.call.flags=0x38;p=planGta5LogicalTileClear(c);assert(p && p->empty()); // Requested absent color suppresses depth loop.
    c.call.flags=0x30;p=planGta5LogicalTileClear(c);assert(p && p->aspects==0x30);
    unsigned rejected=0;
    const auto reject=[&](const auto& mutate) {auto bad=authoredLogicalClear();mutate(bad);assert(!planGta5LogicalTileClear(bad));++rejected;};
    reject([](auto& b){b.state.reset();});reject([](auto& b){b.state->resize(13128);});
    reject([](auto& b){b.call.flags=0x40;});reject([](auto& b){(*b.state)[11068]|=0x40;});
    reject([](auto& b){(*b.state)[11071]|=0x20;});reject([](auto& b){(*b.state)[11072]|=0x80;});
    reject([](auto& b){tileWord(*b.state,13088,1);});reject([](auto& b){b.rectangle.reset();});
    reject([](auto& b){b.color.reset();});reject([](auto& b){b.surfaces[3].header.reset();});
    reject([](auto& b){b.surfaces[2].object+=4;});reject([](auto& b){tileWord(*b.surfaces[4].header,28,0);});
    reject([](auto& b){tileWord(*b.surfaces[1].header,24,1280);});
    reject([](auto& b){tileWord(*b.surfaces[0].header,36,(1279u<<18)|(335u<<3));});
    reject([](auto& b){tileWord(*b.surfaces[1].header,28,384);tileWord(*b.state,10380,384);});
    reject([](auto& b){tileWord(*b.state,13556,1279);});
    reject([](auto& b){tileFloat(*b.color,0,std::numeric_limits<float>::quiet_NaN());});
    reject([](auto& b){tileFloat(*b.state,13024,std::numeric_limits<float>::infinity());});
    reject([](auto& b){b.call.depthBits=UINT64_MAX;});reject([](auto& b){b.call.stencil=256;});
    std::printf("GTA5_LOGICAL_TILE_CLEAR_PASS combinations=%u rejected=%u initial_depth_only_bin0=1 clipped_seam=1\n",cases,rejected);
}
