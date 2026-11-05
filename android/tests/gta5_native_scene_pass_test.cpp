#include "gta5_native_scene_pass.h"
#include "gta5_native_tile_first_scene.h"
#include "gta5_native_tile_final_scene.h"
#include "native_graphics_state.h"
#include "native_visibility_queries.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
namespace {
using Kind=Gta5NativeSceneCommands::Kind;
struct Engine {
    NativeGraphicsState state{};
    NativeTileRegisterState::Journal journal;
    NativeVisibilityQueries queries;
    unsigned zero=0;
    Engine(){state.binMask=state.binSelect=1;state.knownBinWords=15;}
    const auto& stateBatchSnapshot()const{return state;}
    bool logicalVisibilityIdle()const{return queries.idle();}
    bool executeLogicalZeroRaster(){if(!queries.idle())return false;++zero;return true;}
    bool executeLogicalRegisterState(const NativeTileRegisterState& c){c.apply(journal);return true;}
    bool executeLogicalCacheBarrier(const NativeTileCacheBarrier&,int&,unsigned*){return false;}
    bool executeLogicalStateInvalidation(const NativeTileStateInvalidation&,int&,unsigned*){return false;}
    bool executeLogicalInlineShaderLoad(const NativeInlineShaderLoad&){return false;}
    bool executeLogicalBinCommand(const NativeTileBinCommand& c){return c.apply(state);}
    bool executeLogicalVisibilityCommand(unsigned p){return queries.command(p,journal);}
    std::optional<unsigned> retainedRegister(unsigned reg)const {
        const auto found=journal.find(reg);return found==journal.end()?std::nullopt:std::optional(found->second.first);
    }
};
using Bytes=std::vector<std::uint8_t>;
Bytes words(std::initializer_list<unsigned> input){Bytes b;for(auto v:input)for(unsigned i=0;i<4;++i)b.push_back(v>>(24-i*8));return b;}
void put(Bytes& b,unsigned at,unsigned v){for(unsigned i=0;i<4;++i)b.at(at+i)=v>>(24-i*8);}
void append(Gta5NativeSceneCommands& p,Kind kind,unsigned bytes,unsigned index=0,bool predicated=false,unsigned value=0) {
    Gta5NativeSceneCommands::Command c;c.kind=kind;c.offsetBytes=p.commands.empty()?0:p.commands.back().endBytes;
    c.endBytes=c.offsetBytes+bytes;c.index=index;c.predicated=predicated;c.value=value;p.commands.push_back(c);
}
const auto noCopy=[](auto...){assert(false);return false;};
const auto noWrite=[](unsigned,const auto&){assert(false);return false;};
const auto current=[]{return true;};
}
int main() {
    int target=0;
    // State -> real adapter result -> selected write: never a second opaque walker.
    Gta5NativeSceneCommands p;
    append(p,Kind::Raw,8);p.commands.back().operation=*NativeTileRegisterState::parse(words({0x2319,0x12345678}));
    append(p,Kind::Indexed,20,0,true,16);
    append(p,Kind::RegisterWriteback,12,0,true);
    p.commands.back().operation=*NativeTileRegisterWriteback::parse(words({0xC0015501,0x2319,0x4000}));
    {Engine e;unsigned draws=0,writes=0;
        const auto r=Gta5NativeScenePass::execute(p,e,target,current,[&](unsigned i,unsigned control){assert(i==0 && control==16 && !writes);++draws;return true;},noCopy,
            [&](unsigned at,const auto& b){assert(draws==1 && at==0x4000 && (b==std::array<std::uint8_t,4>{0x78,0x56,0x34,0x12}));++writes;return true;});
        assert(r.prefixExecuted() && draws==1 && writes==1 && r.counts().commands==3 && !r.extentsPublished());}
    {Engine e;const auto r=Gta5NativeScenePass::execute(p,e,target,current,[](unsigned,unsigned){return false;},noCopy,noWrite);
        assert(!r.prefixExecuted() && r.failedOffset()==8 && r.counts().commands==1);}
    for(unsigned failure=0;failure<4;++failure) {
        Engine e;auto r=Gta5NativeScenePass::execute(p,e,target,current,[](unsigned,unsigned){return true;},noCopy,
            [](unsigned,const auto&){return true;});assert(r.prefixExecuted());
        Gta5NativeSceneResolves destinations;destinations.destinations.push_back({0xA2000000,0xA2100000,4,{}});
        Gta5NativeSceneResolveStorage storage{{words({0})},{words({1})}};unsigned publications=0;
        if(failure==0){destinations.destinations.clear();storage={};}
        if(failure==1)storage.current[0].pop_back();
        const bool done=r.publishResources(destinations,storage,[&](const auto&,const auto&){++publications;return failure==3;});
        assert(done==(failure==3) && r.resourcesPublished()==done && publications==(failure>=2?1u:0u));
        assert(!r.publishResources(destinations,storage,[](const auto&,const auto&){assert(false);return true;}));
    }
    std::puts("NATIVE_SCENE_RESOURCE_PUBLICATION_CONTROLS_PASS missing_or_wrong_storage_rejected=1 failed_publish_no_completion=1 current_storage_required=1 single_publication=1 authored_host_outputs=1 gpu_execution=0");
    for(bool invalid:{false,true}) {Engine e;e.state.binSelect=0;e.state.knownBinWords=invalid?0x10u:3u;
        const auto r=Gta5NativeScenePass::execute(p,e,target,current,noCopy,noCopy,noWrite);assert(!r.prefixExecuted());}
    {Engine e;e.state.binSelect=0;const auto r=Gta5NativeScenePass::execute(p,e,target,current,noCopy,noCopy,noWrite);
        assert(r.prefixExecuted() && r.counts().skipped==2 && !r.counts().indexed && !r.counts().registerWrites);}
    {Engine e;unsigned reads=0;const auto r=Gta5NativeScenePass::execute(p,e,target,[&]{return !reads++;},[](unsigned,unsigned){return true;},noCopy,
        [](unsigned,const auto&){return true;});assert(!r.prefixExecuted());}
    {Engine e;auto malformed=p;malformed.commands[1].offsetBytes+=4;
        const auto r=Gta5NativeScenePass::execute(malformed,e,target,current,noCopy,noCopy,noWrite);assert(!r.prefixExecuted());}
    // Original-shaped zero DMA's EXT may publish only with its execution witness.
    constexpr unsigned commandAddress=0xA1000000,tableAddress=0xA2000000,tablePhysical=0x2000000;
    const auto bytes=words({0xC0004600,0x19,0xC0006000,UINT32_MAX,0xC0012201,0,0x84,
        0xC0006000,0x80000000,0xC0015A01,0x1A,tablePhysical+9,0xC0004601,0x19});
    Bytes table(24);put(table,0,Gta5TileRecordingDiagnostic::originalLink(tableAddress+20));
    put(table,4,commandAddress+4);put(table,20,0xC0000000);
    const std::array<std::array<unsigned,4>,1> tiles{{{0,0,1280,720}}};
    const auto prepare=[&]{return Gta5TileExtentPublication::prepare({commandAddress,0x1000000,bytes},{tableAddress,tablePhysical,table},tiles);};
    Gta5NativeSceneCommands z;z.sourceBytes=bytes;
    append(z,Kind::DeferredExtentReset,8);
    append(z,Kind::Bin,8);z.commands.back().operation=*NativeTileBinCommand::parse(words({0xC0006000,UINT32_MAX}));
    append(z,Kind::ZeroRaster,12,0,true);
    append(z,Kind::Bin,8);z.commands.back().operation=*NativeTileBinCommand::parse(words({0xC0006000,0x80000000}));
    append(z,Kind::DeferredExtentWrite,12,0,true,tablePhysical+9);
    append(z,Kind::DeferredExtentReset,8,0,true);
    {Engine e;e.state.binSelect=0x80000001;auto extent=prepare();assert(extent);
        auto r=Gta5NativeScenePass::execute(z,e,target,current,noCopy,noCopy,noWrite);
        assert(r.prefixExecuted() && e.zero==1 && r.extents().size()==1 && !r.extentsPublished());unsigned fences=0,writes=0;
        const auto publish=[&](auto& bound){return bound.publish([&](unsigned at,unsigned size)->std::optional<Bytes>{
            if(at==commandAddress && size==bytes.size())return bytes;if(at==tableAddress && size==table.size())return table;return std::nullopt;
        },[&]{++fences;return true;},[&](unsigned at,const auto& before,const auto& after){assert(at==tableAddress && before==table);table=after;++writes;return true;});};
        assert(r.publishExtents(*extent,publish) && r.extentsPublished() && fences==1 && writes==1);
        assert(!r.publishExtents(*extent,publish));
        assert(r.matchesExecutedBytes(bytes));auto other=bytes;other[0]^=1;assert(!r.matchesExecutedBytes(other));
        // Actual submission -> exact executed prefix/bounds -> continuation.
        // Synthetic host memory/IRQ callback; NOT GPU or registry retirement.
        constexpr unsigned device=0x10000000,ring=0xA0001000,fence=0xA0002000;
        constexpr unsigned scratch=0x3000,context=0xC0100000;
        auto ib=bytes;const auto tail=words({0xC0006000,UINT32_MAX,0xC0006000,0x80000000,0x5C8,0x20000,
            0x1057C,Gta5TileCommandInterrupt::Callback,context,0x578,4,
            0xC0043C01,0x13,scratch+2,4,UINT32_MAX,0x100,
            0xC0043C01,0x13,scratch+22,context,UINT32_MAX,0x100,
            0xC0043C01,0x13,scratch+18,Gta5TileCommandInterrupt::Callback,UINT32_MAX,0x100,
            0xC0005401,4,0x45E,4,0xC0043C01,0x13,scratch+2,0,UINT32_MAX,0x100,
            0x57C,0x0BADF00D,0xC0006000,UINT32_MAX});
        assert(tail.size()==Gta5TileSceneContinuation::Bytes);ib.insert(ib.end(),tail.begin(),tail.end());
        std::map<unsigned,Bytes> memory{{device,Bytes(15308)},{ring,Bytes(512)},{fence+60,Bytes(4)},{commandAddress,ib}};
        const Gta5ResourceReader read=[&](unsigned at,unsigned n)->std::optional<Bytes>{
            if(at<0x20000000 && at!=device)at+=0xA0000000;
            const auto found=memory.find(at);if(found==memory.end() || found->second.size()<n)return std::nullopt;
            return Bytes(found->second.begin(),found->second.begin()+n);
        };
        put(memory[device],15300,ring);put(memory[device],15304,127);put(memory[device],11024,fence);
        put(memory[device],11088,126);put(memory[fence+60],0,126);
        auto queued=Gta5NativeTileSubmissionCapture::begin(device,read,read);assert(queued);
        const unsigned rw[]{0xC0006200,0x80000001,0xC0013F00,commandAddress-0xA0000000,unsigned(ib.size()/4)};
        for(unsigned i=0;i<5;++i)put(memory[ring],((126+i)&127)*4,rw[i]);
        put(memory[device],11088,3);assert(queued->capture(3,read,read) && queued->seal(read,read));
        const DrawBinState bins{{UINT32_MAX,0,0x80000001,0},15};
        const auto scene=[&]{auto q=Gta5NativeTileFirstScene::prepare(*queued,bins);assert(q);return *q;};
        {auto badBins=bins;badBins.words[2]=0xC;assert(!Gta5NativeTileFirstScene::prepare(*queued,badBins));}
        unsigned tails=0;
        const auto execute=[&](unsigned d,const auto& t){assert(d==device && t.request().predicated());++tails;return true;};
        {auto q=scene();assert(r.extentsPublished() && !r.resourcesPublished());
            assert(!q.complete(r,read,read,execute) && !q.packetsCompleted() && !tails);}
        assert(r.publishResources({}, {}, [](const auto&,const auto&){return true;}) && r.resourcesPublished());
        {auto q=scene();Engine engine;engine.state.binSelect=0x80000001;
            const auto unpublished=Gta5NativeScenePass::execute(z,engine,target,current,noCopy,noCopy,noWrite);
            assert(unpublished.prefixExecuted() && !q.complete(unpublished,read,read,execute) && !q.packetsCompleted() && !tails);}
        {auto q=scene();memory[commandAddress][0]^=1;
            assert(!q.complete(r,read,read,execute) && !q.packetsCompleted() && !tails);memory[commandAddress][0]^=1;}
        {auto q=scene();assert(!q.complete(r,read,read,[](unsigned,const auto&){return false;}) && !q.packetsCompleted());
            assert(!q.complete(r,read,read,execute) && !tails);}
        {auto q=scene();assert(!q.complete(r,read,read,[&](unsigned,const auto&){memory[commandAddress][0]^=1;return true;}) && !q.packetsCompleted());
            memory[commandAddress][0]^=1;}
        {auto q=scene();assert(q.complete(r,read,read,execute) && q.packetsCompleted() && tails==1);
            assert(!q.complete(r,read,read,execute) && tails==1 && memory[fence+60]==words({126}));}
        std::puts("NATIVE_FIRST_SCENE_COMPLETION_CONTROLS_PASS exact_executed_bytes=1 published_bounds_required=1 failed_tail_no_completion=1 stale_before_and_after_rejected=1 single_use=1 read_pointer_unchanged=1 authored_host_tail=1 gpu_execution=0");
        // Second scene keeps its read pointer until its separately submitted
        // deferred tail really publishes BOTH final words. No new IRQ is faked.
        memory[device].resize(21944);memory[fence]=words({0,0});
        put(memory[fence+60],0,11);put(memory[device],11088,11);
        auto finalCapture=Gta5NativeTileSubmissionCapture::begin(device,read,read);assert(finalCapture);
        for(unsigned i=0;i<5;++i)put(memory[ring],(11+i)*4,i==1?0xCu:rw[i]);
        put(memory[device],11088,16);assert(finalCapture->capture(16,read,read));
        auto partial=*finalCapture;assert(partial.seal(read,read));
        const DrawBinState laterBins{{UINT32_MAX,0,0xC,0},15};
        const auto finalScene=[&]{auto q=Gta5NativeTileFinalScene::prepare(partial,laterBins);assert(q);return *q;};
        for(unsigned alias=0;alias<4;++alias) {
            auto& altered=memory[commandAddress];
            const unsigned newScratch=alias==0?commandAddress-0xA0000000:0x1000;
            const unsigned newContext=alias==2?commandAddress+0x20000000:fence+60+0x20000000;
            for(unsigned at=bytes.size();at<altered.size();at+=4) {
                const unsigned value=(unsigned(altered[at])<<24)|(unsigned(altered[at+1])<<16)|(unsigned(altered[at+2])<<8)|altered[at+3];
                if(alias<2 && (value==scratch+2 || value==scratch+22 || value==scratch+18))put(altered,at,newScratch+value-scratch);
                if(alias>=2 && value==context)put(altered,at,newContext);
            }
            assert(Gta5TileSceneContinuation::parse(commandAddress-0xA0000000+bytes.size(),
                std::span<const std::uint8_t>(altered).subspan(bytes.size())));
            put(memory[device],11088,11);auto bad=Gta5NativeTileSubmissionCapture::begin(device,read,read);assert(bad);
            put(memory[device],11088,16);assert(bad->capture(16,read,read) && bad->seal(read,read));
            assert(!Gta5NativeTileFinalScene::prepare(*bad,laterBins));altered=ib;
        }
        std::puts("NATIVE_FINAL_SCENE_ALIAS_CONTROLS_PASS scratch_and_context_aliases_rejected_before_execution=1 cases=4 gpu_execution=0");
        Engine laterEngine;laterEngine.state.binSelect=0xC;
        auto laterPass=Gta5NativeScenePass::execute(z,laterEngine,target,current,noCopy,noCopy,noWrite);
        assert(laterPass.prefixExecuted() && laterPass.extents().empty());
        auto final=finalScene();
        {auto q=finalScene();assert(!q.completeScene(laterPass,read,read,[](const auto&){assert(false);return true;}));}
        assert(laterPass.publishResources({}, {}, [](const auto&,const auto&){return true;}));
        {auto q=finalScene();assert(!q.completeScene(laterPass,read,read,[](const auto&){return false;}) && !q.packetsCompleted());}
        assert(final.completeScene(laterPass,read,read,[&](const auto& t){
            const auto selected=t.select(laterBins);return selected && !selected->interruptRequired && selected->operationCount==5;
        }));
        constexpr unsigned deferredAddress=0xA0005000,cursor=0xA1004000;
        memory[deferredAddress]=words({0xC0003B00,0x7FFF,0x5C8,0x20000,
            0xC0025800,3,0x2006,cursor,0xC0025800,3,0x2002,17});
        const unsigned dw[]{0xC0006200,UINT32_MAX,0xC0013F00,0x5000,12};
        for(unsigned i=0;i<5;++i)put(memory[ring],(16+i)*4,dw[i]);
        put(memory[device],11088,21);put(memory[device],11036,19);put(memory[device],13608,cursor);put(memory[device],13612,0);
        assert(finalCapture->capture(21,read,read) && finalCapture->seal(read,read));
        {auto stale=final;memory[commandAddress][0]^=1;
            assert(!stale.appendDeferred(*finalCapture,read,read));memory[commandAddress][0]^=1;}
        assert(final.appendDeferred(*finalCapture,read,read) && !final.packetsCompleted());
        {auto incomplete=final;assert(!incomplete.completeDeferred(read,read,[](const auto&){return true;}) && !incomplete.packetsCompleted());}
        {auto failed=final;assert(!failed.completeDeferred(read,read,[](const auto&){return false;}) && !failed.packetsCompleted());}
        {auto stale=final;memory[deferredAddress][0]^=1;
            assert(!stale.completeDeferred(read,read,[](const auto&){assert(false);return true;}));memory[deferredAddress][0]^=1;}
        assert(final.completeDeferred(read,read,[&](const auto& tail){
            put(memory[fence],0,tail.completionValue());put(memory[fence],4,tail.commandCursor());return true;
        }) && final.packetsCompleted());
        assert(!final.completeDeferred(read,read,[](const auto&){assert(false);return true;}) && memory[fence+60]==words({11}));
        std::puts("NATIVE_FINAL_SCENE_COMPLETION_CONTROLS_PASS two_original_shaped_submissions=1 skipped_irq_five_operations=1 resources_required=1 stale_or_failed_rejected=1 both_deferred_words_required=1 single_use=1 read_pointer_unchanged=1 authored_host_effects=1 gpu_execution=0");
    }
    {Engine e;e.state.binSelect=0x80000001;auto altered=z;altered.commands[4].value+=16;
        auto r=Gta5NativeScenePass::execute(altered,e,target,current,noCopy,noCopy,noWrite);auto extent=prepare();assert(extent);
        assert(r.prefixExecuted() && !r.publishExtents(*extent,[](auto&){assert(false);return true;}));}
    {Engine e;auto active=z;active.commands.clear();append(active,Kind::Query,8,0,false,1);append(active,Kind::ZeroRaster,12);
        auto r=Gta5NativeScenePass::execute(active,e,target,current,noCopy,noCopy,noWrite);assert(!r.prefixExecuted() && !e.zero);}
    {Engine e;Gta5NativeSceneCommands emptyQuery;
        append(emptyQuery,Kind::Query,8,0,false,8);append(emptyQuery,Kind::Query,8,0,false,0x108);append(emptyQuery,Kind::ZeroRaster,12);
        auto r=Gta5NativeScenePass::execute(emptyQuery,e,target,current,noCopy,noCopy,noWrite);
        assert(r.prefixExecuted() && e.zero==1 && e.queries.condition(0x108)==false);
        assert(e.journal.at(0xC44).first==0 && e.journal.at(0xC44).second==0x100);
        std::puts("NATIVE_SCENE_ENDED_HIDDEN_QUERY_PASS actual_visibility_lifecycle=1 keep_false=1 status_bit_known_zero=1 zero_draw_after_end=1");}
    std::puts("NATIVE_SCENE_ORDERED_CONTROLS_PASS producer_consumer_order=1 failed_draw_no_later_write=1 unknown_or_invalid_predicate_rejected=1 skipped_no_effects=1 stale_no_completion=1 extent_requires_executed_dma_and_exact_binding=1 active_query_not_bypassed=1 authored_host_callbacks=1 gpu_execution=0");
}
