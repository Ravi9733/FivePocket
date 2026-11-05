#include "gta5_tile_replay.h"
#include "gta5_tile_replay_schedule.h"
#include "gta5_logical_tile_clear_fixture.h"
#include <cstdio>
int main() {
    using namespace recomp::gpu;using namespace recomp::gpu::test;
    unsigned cases=0;
    for(unsigned flags=0;flags<8;++flags)for(unsigned bin=0;bin<16;++bin) {
        unsigned expected=flags&4?0x2AAAAAAAu:flags&2?0x15555555u:UINT32_MAX;
        if(flags&1) {expected&=3u<<(bin*2);if(!bin && !(flags&4))expected|=0x80000000u;}
        assert(gta5DriverTileReplayMask(flags<<29,bin)==expected);
        assert(gta5DriverTileReplayMask((flags<<29)|0x1FFFFFFFu,bin)==expected);++cases;
    }
    assert(!gta5DriverTileReplayMask(0,16) && !gta5DriverTileReplayMask(0,UINT32_MAX));
    auto capture=authoredLogicalClear();auto p=planGta5LogicalTileCarry(capture,0,1);assert(p);
    const auto both=planGta5LogicalTileClear(capture);assert(both && both->tiles==3);
    for(unsigned bin=0;bin<2;++bin) {
        const auto selected=planGta5LogicalTileClearForReplay(capture,bin);
        assert(selected && selected->tiles==(1u<<bin) && selected->aspects==both->aspects &&
            selected->colors==both->colors && selected->rectangle==both->rectangle &&
            selected->depth==both->depth && selected->stencil==both->stencil);
    }
    auto firstOnly=capture;tileWord(*firstOnly.state,13084,3);
    const auto excluded=planGta5LogicalTileClearForReplay(firstOnly,1);assert(excluded && excluded->empty());
    for(unsigned bin:{2u,32u,UINT32_MAX})assert(!planGta5LogicalTileClearForReplay(capture,bin));
    auto invalidClear=capture;invalidClear.surfaces[0].header.reset();
    assert(!planGta5LogicalTileClearForReplay(invalidClear,0));
    assert((p->source==std::array<unsigned,4>{0,0,1280,336}));
    assert((p->destination==std::array<unsigned,4>{0,384,1280,720}));
    (*capture.state)[11071]|=2;
    const auto prepared=planGta5LogicalTileCarry(capture,0,1);
    assert(prepared && prepared->source==p->source && prepared->destination==p->destination);
    auto clear=capture;clear.call.flags=0;assert(!planGta5LogicalTileClear(clear));
    for(const auto [offset,bit]:std::array<std::array<unsigned,2>,6>{{{11068,0x10},{11068,0x40},{11068,0x80},{11071,0x20},{11072,0x40},{11072,0x80}}}) {
        auto bad=capture;(*bad.state)[offset]|=bit;assert(!planGta5LogicalTileCarry(bad,0,1));
    }
    (*capture.state)[11071]&=~2u;
    assert(!planGta5LogicalTileCarry(capture,0,0));assert(!planGta5LogicalTileCarry(capture,1,0));
    assert(!planGta5LogicalTileCarry(capture,0,2));assert(!planGta5LogicalTileCarry(capture,UINT32_MAX,1));
    auto bad=capture;bad.surfaces[0].header.reset();assert(!planGta5LogicalTileCarry(bad,0,1));
    bad=capture;tileWord(*bad.state,10372,0x190);assert(!planGta5LogicalTileCarry(bad,0,1));
    // A valid layout can have a smaller first tile. Its unknown physical
    // padding cannot initialize the next larger tile via a logical image copy.
    auto taller=capture;tileWord(*taller.state,13140,352);tileWord(*taller.state,13148,352);
    tileWord(*taller.state,13380,352);
    auto validation=taller;validation.call.flags=0;assert(planGta5LogicalTileClear(validation));
    assert(!planGta5LogicalTileCarry(taller,0,1));
    // The complete schedule is planned before any GPU effect. Malformed or
    // unsupported late records cannot leak a successful prefix to the caller.
    constexpr unsigned start=0xC0010000;
    std::vector<unsigned> words(63);words[0]=0x80000000;words[1]=1;words[2]=2;
    words.insert(words.end(),{0x86000000,0x81000003,0x07108000,0x8F000005,0x07109000,0x87000000,0xC0000000});
    const auto schedule=[&](const auto& source) {
        return planGta5TileReplaySchedule(start,[&](unsigned address,unsigned size)->std::optional<std::vector<std::uint8_t>> {
            if(address<start || std::uint64_t{address}+size>std::uint64_t{start}+source.size()*4)return std::nullopt;
            std::vector<std::uint8_t> bytes(size);
            for(unsigned i=0;i<size;i+=4)tileWord(bytes,i,source[(address-start+i)/4]);
            return bytes;
        });
    };
    const auto planned=schedule(words);assert(planned && planned->events.size()==7);
    assert(planned->events[0].mask==0x80000003 && planned->events[3].mask==12 &&
        planned->events[6].mask==UINT32_MAX && planned->terminalAddress==start+69*4);
    unsigned scheduleNegatives=0;
    for(unsigned at:{0u,1u,2u,63u,68u,69u}) {
        auto broken=words;broken[at]=at==2?0:0x12345678;
        assert(!schedule(broken));++scheduleNegatives;
    }
    for(unsigned unsupported:{0x82000000u,0x88000000u,0x89000000u,0x8A000000u,0x8C000000u,0x8D000000u,0x8E000000u,0x90000000u}) {
        auto broken=words;broken.insert(broken.begin()+66,unsupported);
        assert(!schedule(broken));++scheduleNegatives;
    }
    for(unsigned truncate:{0u,62u,63u,65u,69u}) {
        auto broken=words;broken.resize(truncate);assert(!schedule(broken));++scheduleNegatives;
    }
    auto broken=words;broken[2]=16;assert(!schedule(broken));++scheduleNegatives;
    broken=words;broken[63]=0x83000000;assert(!schedule(broken));++scheduleNegatives;
    broken=words;broken[64]=start+63*4; // Link to its own instruction, bounded failure.
    assert(!schedule(broken));++scheduleNegatives;
    broken=words;broken[64]=0xC0100000;assert(!schedule(broken));++scheduleNegatives;
    unsigned reads=0;
    for(unsigned invalid:{0u,3u,0xFFFFFF04u,0xFFFFFFFCu}) {
        assert(!planGta5TileReplaySchedule(invalid,[&](auto,auto)->std::optional<std::vector<std::uint8_t>> {++reads;return std::nullopt;}));
        ++scheduleNegatives;
    }
    assert(!reads);
    std::printf("GTA5_TILE_SCHEDULE_PASS ordered_two_bins=1 late_unknown_rejects_whole_plan=1 bounded_cycle=1 negatives=%u gpu_execution=0\n",scheduleNegatives);
    std::printf("GTA5_TILE_REPLAY_PASS mask_cases=%u carry_local_coordinates=1 unknown_padding_rejected=1 gpu_execution=0\n",cases);
}
