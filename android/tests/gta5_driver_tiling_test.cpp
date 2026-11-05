#include "gta5_driver_tiling.h"
#include "gta5_driver_scissor.h"
#include <cassert>
#include <cstdio>

int main() {
    using namespace recomp::gpu;
    std::vector<std::uint8_t> state(13860);
    const auto put=[&](unsigned at,unsigned v) {
        for(unsigned i=0;i<4;++i)state[at+i]=v>>(24-8*i);
    };
    const auto tile=[&](unsigned i,std::array<unsigned,4> r) {
        for(unsigned c=0;c<4;++c)put(13128+i*16+c*4,r[c]);
        put(13368+i*8,r[0]&~31u);put(13372+i*8,r[1]&~31u);
    };
    state[11068]=0x21;put(13124,2);put(13556,1280);put(13560,720);put(13564,1);
    put(12816,0x10010000);put(13104,0x10010000);put(13100,0x10010000);
    tile(0,{0,0,1280,384});tile(1,{0,384,1280,720});
    auto plan=describeGta5DriverTiling(state);
    assert(plan && plan->disjointFullCoverage && plan->boundTargetsUseTiles);
    assert((plan->tiles[1].local==std::array<unsigned,4>{0,0,1280,336}));
    assert(plan->tiles[1].selectLow==12);
    auto scissor=planGta5DriverScissor(state,{0,0,1280,720});
    assert(scissor && scissor->binned && scissor->tiles.size()==2);
    assert(scissor->tiles[1].registers[0]==((0u-384u)&0x7FFFu)<<16);
    // Same tile session, unrelated rebound target: scissor must be direct.
    put(12816,0x10020000);plan=describeGta5DriverTiling(state);
    assert(plan && !plan->boundTargetsUseTiles && !planGta5DriverScissor(state,{0,0,1280,720})->binned);
    state[11068]|=0x10;assert(describeGta5DriverTiling(state)->boundTargetsUseTiles);
    state[11068]=0x21;put(12816,0x10010000);
    // Unaligned logical boundaries have an aligned origin plus local inset.
    tile(0,{0,0,1280,373});tile(1,{0,373,1280,720});
    plan=describeGta5DriverTiling(state);assert(plan && plan->disjointFullCoverage);
    assert((plan->tiles[1].origin==std::array<unsigned,2>{0,352}));
    assert((plan->tiles[1].local==std::array<unsigned,4>{0,21,1280,368}));
    tile(1,{0,374,1280,720});assert(!describeGta5DriverTiling(state)->disjointFullCoverage);
    tile(1,{0,372,1280,720});assert(!describeGta5DriverTiling(state)->disjointFullCoverage);
    tile(1,{0,373,1280,720});
    for(unsigned at:{13124u,13100u,13556u,13560u,13376u}) {
        const auto good=state;put(at,0xFFFFFFFF);assert(!describeGta5DriverTiling(state));state=good;
    }
    const auto good=state;
    put(13124,0);assert(!describeGta5DriverTiling(state));state=good;
    state.resize(13567);assert(!describeGta5DriverTiling(state));state=good;
    state[11068]=0;assert(!describeGta5DriverTiling(state));state=good;
    put(13128+16+8,0);assert(!describeGta5DriverTiling(state));state=good;
    put(12816,0);put(13104,0);put(12832,0x10030000);put(13120,0x10030000);put(13100,0x10030000);
    assert(describeGta5DriverTiling(state)->boundTargetsUseTiles);
    // Capacity and high bin bits are bounded independently of native support.
    put(13124,15);put(13560,480);
    for(unsigned i=0;i<15;++i)tile(i,{0,i*32,1280,(i+1)*32});
    plan=describeGta5DriverTiling(state);assert(plan && plan->disjointFullCoverage);
    assert(plan->tiles.back().selectLow==0x30000000);
    put(13124,16);assert(!describeGta5DriverTiling(state));
    std::puts("DRIVER_TILING_LAYOUT_PASS native_tiled_execution=0");
}
