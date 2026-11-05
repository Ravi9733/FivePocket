#include "gta5_driver_scissor.h"
#include <cassert>
#include <cstdio>

int main() {
    using namespace recomp::gpu;
    std::vector<std::uint8_t> state(13860);
    const auto set=[&](unsigned at,std::uint32_t value) {
        for(unsigned b=0;b<4;++b)state[at+b]=value>>(24-b*8);
    };
    const std::array<std::uint32_t,4> rectangle{16,32,1200,700};
    auto plan=planGta5DriverScissor(state,rectangle);assert(plan && !plan->binned);
    assert((plan->words==std::vector<std::uint32_t>{0x22080,0,0x200010,0x2BC04B0}));
    const auto prefix=[&](unsigned count){return std::vector<std::uint8_t>(state.begin(),state.begin()+count);};
    assert(!planGta5DriverScissor(prefix(13127),rectangle));
    // Every flag combination, with all matching, null, and one differing target.
    unsigned cases=2;
    for(unsigned flags=0;flags<256;++flags)for(unsigned slot=0;slot<5;++slot) {
        state[11068]=flags;set(12816+slot*4,7);set(13104+slot*4,7);
        plan=planGta5DriverScissor(state,rectangle);
        assert(plan && plan->binned==bool(flags&0x30));++cases;
        set(13104+slot*4,8);plan=planGta5DriverScissor(state,rectangle);
        assert(plan && plan->binned==bool(flags&0x10));++cases;
        set(12816+slot*4,0);plan=planGta5DriverScissor(state,rectangle);
        assert(plan && plan->binned==bool(flags&0x30));++cases;
        set(13104+slot*4,0);
    }
    state[11068]=0x10;set(13124,2);set(13084,0x89ABCDEF);set(13088,0x76543210);
    for(unsigned c=0;c<4;++c){set(13128+c*4,std::array<unsigned,4>{0,0,640,720}[c]);
        set(13144+c*4,std::array<unsigned,4>{640,0,1280,720}[c]);}
    set(13368,0);set(13372,0);set(13376,640);set(13380,0);
    plan=planGta5DriverScissor(state,rectangle);assert(plan && plan->tiles.size()==2);
    assert((plan->tiles[0].registers==std::array<unsigned,3>{0,0x200010,0x2BC0280}));
    assert((plan->tiles[1].registers==std::array<unsigned,3>{0x7D80,0x200280,0x2BC04B0}));
    assert(plan->tiles[0].selectLow==3 && plan->tiles[1].selectLow==12);
    assert((plan->words==std::vector<std::uint32_t>{0xC0006100,0,
        0xC0006000,3,0xC0035501,0x2080,0,0x200010,0x2BC0280,
        0xC0006000,12,0xC0035501,0x2080,0x7D80,0x200280,0x2BC04B0,
        0xC0006000,0x89ABCDEF,0xC0006100,0x76543210}));++cases;
    auto clipped=planGta5DriverScissor(state,{650,32,700,40});assert(clipped);
    assert(clipped->tiles[0].registers[1]==0 && clipped->tiles[0].registers[2]==0);++cases;
    auto negative=planGta5DriverScissor(state,{0xFFFFFFFF,0xFFFFFFFE,16,32});assert(negative);
    assert(negative->tiles[0].registers[1]==0 && negative->tiles[0].registers[2]==0x200010);++cases;
    state[11068]=0x50;state[11071]=0x20;
    plan=planGta5DriverScissor(state,rectangle);assert(plan && plan->auxiliaryRectangle && plan->words.size()==26);
    assert(plan->words[16]==0xC0006000 && plan->words[17]==1 && plan->words[18]==0xC0025501);++cases;
    set(13124,16);assert(!planGta5DriverScissor(state,rectangle));++cases;
    set(13124,15);plan=planGta5DriverScissor(state,rectangle);
    assert(plan && plan->tiles.size()==15 && plan->tiles.back().selectLow==0x30000000);++cases;
    assert(!planGta5DriverScissor(prefix(13487),rectangle));++cases;
    set(13124,0);plan=planGta5DriverScissor(prefix(13128),rectangle);
    assert(plan && plan->tiles.empty() && plan->words.size()==12);++cases;
    std::printf("GTA5_DRIVER_SCISSOR_PLAN_PASS cases=%u native_binned_execution=0\n",cases);
}
