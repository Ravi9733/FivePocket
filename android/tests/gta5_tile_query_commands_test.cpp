#include "gta5_tile_query_commands.h"
#include "native_visibility_queries.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
using Words=std::vector<unsigned>;
static Pm4Snapshot snapshot(const Words& words) {
    std::vector<std::uint8_t> bytes;
    for(auto word:words)for(unsigned lane=0;lane<4;++lane)bytes.push_back(word>>(24-8*lane));
    Pm4Snapshot out;Pm4Error error;assert(decodePm4(bytes.data(),bytes.size(),{},out,error));return out;
}
int main() {
    const Words source{0x2293,17,0xC0002300,8,0xC0032201,0,0x60004,0x1000,0x40000006,
        0xC0002300,0x108,0xC0032201,0,0x30004,0x2000,0x40000003,0xC0012201,0,0x84};
    const std::vector<Gta5TileQueryDrawBinding> draws{{0x1000,6,17},{0x2000,3,16}};
    Gta5TileQueryCommands out;unsigned cases=0;
    assert(matchGta5TileQueryCommands(snapshot(source),draws,out));++cases;
    assert(out.commands.words()==source && out.actions.size()==6 && out.matchedDraws==2 &&
        out.queryCommands==2 && out.unimplementedPackets==2);
    const auto reject=[&](Words words,std::vector<Gta5TileQueryDrawBinding> bindings) {
        const auto before=out.commands.words();const auto count=out.actions.size();
        assert(!matchGta5TileQueryCommands(snapshot(words),bindings,out));
        assert(out.commands.words()==before && out.actions.size()==count);++cases;
    };
    auto words=source;words[2]|=1;reject(words,draws);
    words=source;words[3]=0x108;reject(words,draws);
    words=source;words[10]=8;reject(words,draws);
    words=source;words[10]=0x109;reject(words,draws);
    words=source;words[3]|=0x80;reject(words,draws);
    words=source;words[10]|=0x200;reject(words,draws);
    words=source;words[5]=0x108;reject(words,draws);
    words=source;words[7]=0x1004;reject(words,draws);
    words=source;words[8]=0x40000003;reject(words,draws);
    words=source;words[2]=0xC0004400;reject(words,draws);
    words=source;words.insert(words.begin()+4,{0xC0013F00,0x3000,5});reject(words,draws);
    words=source;words.insert(words.begin()+4,{0xC0003601,0x30088});reject(words,draws);
    words=source;words.insert(words.begin()+4,{0xC0012201,0,0x84});reject(words,draws);
    auto bindings=draws;bindings[0].visibilityControl=19;reject(source,bindings);
    bindings=draws;bindings[1].visibilityControl=17;reject(source,bindings);
    bindings=draws;bindings[0].visibilityControl|=0x80;reject(source,bindings);
    bindings=draws;bindings[0].indexCount=3;reject(source,bindings);
    bindings=draws;bindings.pop_back();reject(source,bindings);
    bindings=draws;bindings.push_back(draws.back());reject(source,bindings);
    reject(source,{});
    words=source;words.insert(words.end(),{0xC0002300,9});reject(words,draws);
    // The original End updates the CPU shadow and dirty mask, not an immediate
    // 2293 PM4 write. Bind the provided draw-time shadow, never infer 17 here.
    NativeVisibilityQueries queries;NativeVisibilityQueries::Journal journal;unsigned executed=0,opaque=0;
    for(const auto& action:out.actions) {
        using Kind=Gta5TileQueryCommands::Kind;
        if(action.kind==Kind::Unimplemented){++opaque;continue;} // Host test only, not a stream executor.
        if(action.kind==Kind::Indexed) {
            assert(queries.draw(action.payload,[&]()->std::optional<NativeVisibilityQueries::Measurement> {
                ++executed;return NativeVisibilityQueries::Measurement{false,
                    (action.payload&1)?NativeVisibilityQueries::Stage::PotentialVisibility:NativeVisibilityQueries::Stage::FinalSamples};
            }));
        } else assert(queries.command(action.payload,journal));
    }
    assert(executed==2 && opaque==2 && queries.idle() && queries.condition(0x108)==false &&
        journal.at(0xC44).first==0 && journal.at(0xC44).second==256);++cases;
    // No fixed query8 or single bank assumption.
    for(unsigned id:{0u,31u,32u,63u}) {
        words=source;words[1]=id*2+1;words[3]=id;words[10]=0x100|id;
        bindings=draws;bindings[0].visibilityControl=id*2+1;bindings[1].visibilityControl=id*2;
        assert(matchGta5TileQueryCommands(snapshot(words),bindings,out));++cases;
    }
    std::printf("TILE_QUERY_COMMAND_CONNECTION_PASS cases=%u raw_dma_identity=1 ordered_lifecycle=1 cpu_shadow_distinct=1 opaque_retained=1 failure_atomic=1 gpu_execution=0\n",cases);
}
