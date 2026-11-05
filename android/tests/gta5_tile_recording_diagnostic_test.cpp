#include "gta5_tile_recording_diagnostic.h"
#include "gta5_logical_tile_clear_fixture.h"
#include <cstdio>
#include <map>
using namespace recomp::gpu;using namespace recomp::gpu::test;
int main() {
    auto seed=authoredLogicalClear();seed.call.flags=0;
    std::map<unsigned,std::vector<std::uint8_t>> blocks;
    for(unsigned i=0;i<7;++i) {
        const auto at=Gta5TileRecordingDiagnostic::DescriptorOffsets[i],head=0xA7100000+i*0x1080,used=i?4u:260u;
        for(unsigned c=0;c<6;++c)tileWord(*seed.state,at+c*4,std::array<unsigned,6>{head,head,4224,head+used,head+4220,seed.call.device}[c]);
        auto& b=blocks[head];b.resize(used,0xCD);tileWord(b,0,0xC7100004+i*0x1080);
    }
    const auto layout=*describeGta5DriverTiling(*seed.state);auto& header=blocks.begin()->second;
    tileWord(header,4,0x80000000);tileWord(header,8,1);tileWord(header,12,2);tileWord(header,256,0x86000000);
    for(unsigned i=0;i<2;++i)for(unsigned c=0;c<4;++c)tileWord(header,16+i*16+c*4,layout.tiles[i].logical[c]);
    unsigned reads=0;
    const auto read=[&](unsigned a,unsigned n)->std::optional<std::vector<std::uint8_t>> {
        ++reads;const auto it=blocks.find(a);if(it==blocks.end() || it->second.size()!=n)return std::nullopt;return it->second;
    };
    const std::array<unsigned,4> full{0,0,1280,720};
    for(unsigned flags:{0x30u,0x31u}) {
        Gta5TileRecordingDiagnostic d;assert(!d.pending() && d.device()==0);
        reads=0;assert(d.begin(seed,full,read) && reads==7 && d.pending() && d.device()==seed.call.device);
        assert(d.initialRecords()[0]==header && d.scissors().size()==1 && d.clears().empty());
        assert(!d.begin(seed,full,read));
        auto clear=seed;clear.call.flags=flags;tileWord(*clear.state,13084,3);
        auto bad=clear;bad.surfaces[2].header.reset();assert(!d.recordClear(bad) && d.clears().empty());
        bad=clear;bad.call.device+=4;assert(!d.recordClear(bad));
        bad=clear;tileWord(*bad.state,13084,12);assert(!d.recordClear(bad));
        assert(d.recordClear(clear) && d.clears().size()==1 && d.pending());
        assert(d.events().size()==2 && d.events()[0].kind==Gta5TileRecordingDiagnostic::Kind::Scissor &&
            d.events()[1].kind==Gta5TileRecordingDiagnostic::Kind::Clear && d.events()[1].index==0);
        for(unsigned i=0;i<7;++i)assert(d.recordScissor(seed,{i,i,1280-i,720-i}));
        assert(!d.recordScissor(seed,full) && d.scissors().size()==8 && d.pending());
        assert(d.events().size()==9 && d.events().back().kind==Gta5TileRecordingDiagnostic::Kind::Scissor && d.events().back().index==7);
        // Retained snapshots, not borrowed memory; no source payload mutations.
        clear.call.flags=0;assert(d.clears()[0].call.flags==flags && blocks.begin()->second==header);
    }
    unsigned sequenceCases=0;
    {
        Gta5TileRecordingDiagnostic d;assert(d.begin(seed,full,read));
        auto prepared=seed;(*prepared.state)[11068]=0x21;(*prepared.state)[11071]=3;
        assert(!d.recordIndexed(prepared,1)); // No preceding clear recorded.
        auto clear=seed;clear.call.flags=0x31;tileWord(*clear.state,13084,3);assert(d.recordClear(clear));
        assert(d.recordScissor(prepared,full)); // Shader flags do not change surfaces.
        for(unsigned sequence=1;sequence<=Gta5TileRecordingDiagnostic::MaxIndexed;++sequence) {
            auto bad=prepared;bad.surfaces[0].header.reset();assert(!d.recordIndexed(bad,sequence));
            assert(d.recordIndexed(prepared,sequence) && !d.recordIndexed(prepared,sequence));
            assert(d.events().back().kind==Gta5TileRecordingDiagnostic::Kind::Indexed && d.events().back().index==sequence);
        }
        assert(!d.recordIndexed(prepared,129) && d.indexedCount()==128 && d.pending());
        assert(!planGta5LogicalTileClear(prepared)); // No clear execution admission gained.
        auto resolve=prepared;(*resolve.state)[11068]=0x20;(*resolve.state)[11072]=0xC0;
        static_assert(Gta5TileRecordingDiagnostic::MaxResolves==10);
        for(unsigned i=0;i<Gta5TileRecordingDiagnostic::MaxResolves;++i) {
            auto bad=resolve;bad.surfaces[4].header.reset();assert(!d.recordResolve(bad,i));
            assert(!d.recordResolve(resolve,i+1));
            assert(d.recordResolve(resolve,i) && !d.recordResolve(resolve,i));
            assert(d.events().back().kind==Gta5TileRecordingDiagnostic::Kind::Resolve && d.events().back().index==i);
        }
        assert(!d.recordResolve(resolve,Gta5TileRecordingDiagnostic::MaxResolves) && d.pending());
        assert(!planGta5LogicalTileClear(resolve)); // Still no native cache-barrier bypass.
        std::puts("TILE_RESOLVE_EVENT_ORDER_PASS exact_indices=10 wrong_owner_rejected=1 pending_never_completed=1 gpu_execution=0");
    }
    // Original clear oracle covers aspect splitting and selected-bin effects.
    // These fixtures exercise bounded retention/order, not GPU execution.
    for(unsigned flags=1;flags<=0x3F;++flags)for(unsigned select:{3u,12u,UINT32_MAX}) {
        Gta5TileRecordingDiagnostic d;assert(d.begin(seed,full,read));
        auto initial=seed;initial.call.flags=0x31;tileWord(*initial.state,13084,3);
        assert(d.recordClear(initial));
        auto next=seed;next.call.flags=flags;tileWord(*next.state,13084,select);next.call.stencil=7;
        auto plan=planGta5LogicalTileClear(next);assert(plan && plan->aspects==flags && !plan->empty());
        assert(d.recordScissor(next,full) && d.recordClear(next));
        assert(d.clears().size()==2 && d.events().size()==4 && d.events().back().index==1);
        const auto savedEvents=d.events().size();
        const auto reject=[&](const Gta5ClearCapture& bad) {
            assert(!d.recordClear(bad) && d.clears().size()==2 && d.events().size()==savedEvents && d.pending());
        };
        auto bad=next;bad.call.flags|=0x40;reject(bad);
        bad=next;bad.call.flags=0;reject(bad);
        bad=next;bad.call.device+=4;reject(bad);
        bad=next;bad.state.reset();reject(bad);
        bad=next;bad.state->resize(13859);reject(bad);
        bad=next;tileWord(*bad.state,13088,1);reject(bad);
        bad=next;bad.rectangle.reset();reject(bad);
        bad=next;clearRectangle(bad,{0,720,1280,721});reject(bad);
        for(unsigned i=0;i<5;++i) {
            bad=next;bad.surfaces[i].header.reset();reject(bad);
            bad=next;bad.surfaces[i].tileCache+=4;reject(bad);
            bad=next;(*bad.surfaces[i].header)[0]^=1;reject(bad);
        }
        if(flags&0x20){bad=next;bad.call.stencil=256;reject(bad);}
        if(flags&0x10){bad=next;bad.call.depthBits=0x7FF8000000000000ULL;reject(bad);}
        if(flags&15){bad=next;bad.color.reset();reject(bad);}
        for(unsigned i=2;i<Gta5TileRecordingDiagnostic::MaxClears;++i) {
            assert(d.recordClear(next) && d.events().back().index==i);
        }
        assert(!d.recordClear(next) && d.clears().size()==Gta5TileRecordingDiagnostic::MaxClears && d.pending());
        next.call.flags=0;assert(d.clears()[1].call.flags==flags && d.clears()[1].call.stencil==7);
        ++sequenceCases;
    }
    unsigned negatives=0;
    for(unsigned descriptor:Gta5TileRecordingDiagnostic::DescriptorOffsets)for(unsigned lane=0;lane<6;++lane) {
        auto bad=seed;tileWord(*bad.state,descriptor+lane*4,tileWord(*bad.state,descriptor+lane*4)^4u);
        Gta5TileRecordingDiagnostic d;assert(!d.begin(bad,full,read) && !d.pending() && d.scissors().empty());++negatives;
    }
    for(unsigned at:{0u,4u,8u,12u,16u,20u,24u,28u,32u,36u,40u,44u,256u}) {
        const auto original=tileWord(header,at);tileWord(header,at,original^4);
        Gta5TileRecordingDiagnostic d;assert(!d.begin(seed,full,read) && !d.pending());tileWord(header,at,original);++negatives;
    }
    for(unsigned at=13784;at<13808;at+=4) {
        auto bad=seed;tileWord(*bad.state,at,4);Gta5TileRecordingDiagnostic d;
        assert(!d.begin(bad,full,read) && !d.pending());++negatives;
    }
    Gta5TileRecordingDiagnostic d;
    assert(!d.begin(seed,{1,0,1280,720},read));
    const auto absent=[](auto,auto)->std::optional<std::vector<std::uint8_t>>{return std::nullopt;};
    assert(!d.begin(seed,full,absent));
    auto bad=seed;tileWord(*bad.state,13564,3);assert(!d.begin(bad,full,read));
    bad=seed;bad.state->resize(13859);assert(!d.begin(bad,full,read));
    std::printf("TILE_RECORDING_DIAGNOSTIC_PASS seeds=7 initial_clear_flags=2 negative_seeds=%u sequence_cases=%u clear_budget=%u source_unmodified=1 pending_never_completed=1 gpu_execution=0\n",negatives,sequenceCases,Gta5TileRecordingDiagnostic::MaxClears);
}
