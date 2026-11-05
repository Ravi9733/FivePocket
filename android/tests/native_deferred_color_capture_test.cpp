#include "native_deferred_color_capture.h"
#include "native_color_batch_fixture.h"
#include "native_post_output_swap_fixture.h"
#include <cstdio>
#include <type_traits>
using namespace recomp::gpu;
using Words=std::vector<unsigned>;
static std::optional<NativeDeferredSwapSubmission> frame(const Words& words){
    const auto command=NativeSwapCommand::make({3,0x3000+unsigned(words.size())*4+40,0x2000000,73});assert(command);
    std::vector<std::uint8_t> bytes;
    for(auto w:words)for(unsigned i=0;i<4;++i)bytes.push_back(w>>(24-i*8));
    const auto tail=test::postOutputSwapBytes(*command);bytes.insert(bytes.end(),tail.begin(),tail.end());
    return NativeDeferredSwapSubmission::parse(*command,0x3000,bytes.data(),bytes.size());
}
static std::array<unsigned,9> offsets(const NativeDeferredColorSyncRequests& v){
    return {v.leadingIdleOffset,v.legacySetupOffset,v.depthStatusOffset,v.depthRangeOffset,v.depthWaitOffset,
        v.copyIdleOffset,v.cacheFlushOffset,v.invalidationOffsets[0],v.invalidationOffsets[1]};
}
int main(){
    static_assert(!std::is_copy_constructible_v<NativeDeferredColorCapture>);
    static_assert(std::is_nothrow_move_constructible_v<NativeDeferredColorCapture>);
    static_assert(!std::is_convertible_v<NativeDeferredColorCapture,NativeColorBatchCapture>);
    unsigned syncRejected=0,framingRejected=0;
    for(bool constantZero:{false,true}){
        test::NativeColorBatchFixture fixture(0x800000,0x2000000,0x7000000,0x7000004,constantZero,true);
        Words words=fixture.words;words.resize(words.size()-14); // Remove old completion, don't add fake events.
        words.insert(words.end(),{0x2007,0,0x2318,0,0x231B,0});
        auto submitted=frame(words);assert(submitted);
        unsigned reads=0;
        const Pm4PhysicalReader reader=[&](auto address,auto size)->std::optional<std::vector<std::uint8_t>>{
            ++reads;
            for(unsigned i=0;i<8;++i)if(address==fixture.addresses[i] && size==fixture.raw[i].size())return fixture.raw[i];
            assert(false);return std::nullopt;
        };
        auto captured=NativeDeferredColorCapture::create(*submitted,{},*fixture.nativeProgram,reader);
        assert(captured && reads==(constantZero?7u:8u) && captured->commands().draws.size()==4);
        assert(captured->color().sourceRanges().constantZeroTextures==unsigned(constantZero));
        assert(captured->copy().copy.destinationBase==submitted->swap().command().identity().imagePhysical);
        NativeColorChainBindings bindings;assert(captured->color().bindings(bindings));
        const auto count=captured->sourceBytes();assert(count>1400000 && count<2*1024*1024);
        auto moved=std::move(*captured);assert(moved.color().bindings(bindings) && moved.submission().sameRequest(*submitted));
        const auto plan=moved.commands();NativeDeferredColorSyncRequests sync;
        assert(planNativeDeferredColorSync(*submitted,plan,sync));const auto saved=offsets(sync);
        const auto rejectPlan=[&](const auto& p){assert(!planNativeDeferredColorSync(*submitted,p,sync) && offsets(sync)==saved);++syncRejected;};
        auto altered=plan;altered.actions.pop_back();rejectPlan(altered);
        altered=plan;altered.draws.pop_back();rejectPlan(altered);
        altered=plan;altered.waits.clear();rejectPlan(altered);
        for(auto reg:{0xA31u,0xA2Fu,0xA30u,0x200Du}){
            altered=plan;altered.waits[0].registers.erase(reg);rejectPlan(altered);
            for(unsigned bit=0;bit<32;++bit){altered=plan;altered.waits[0].registers[reg].bits.known^=1u<<bit;rejectPlan(altered);}
        }
        for(unsigned i=0;i<plan.actions.size();++i){altered=plan;altered.actions[i].priorDraws^=1;rejectPlan(altered);}
        const auto rejectWords=[&](const Words& bad){
            const auto changed=frame(bad);if(!changed){++framingRejected;return;}
            DrawCommandPlan p;DrawCommandError error;
            if(!planDrawCommandCandidates(changed->prefix(),{},reader,p,error)){++framingRejected;return;}
            assert(!planNativeDeferredColorSync(*changed,p,sync) && offsets(sync)==saved);++syncRejected;
        };
        for(unsigned at:saved){
            const auto& p=*std::find_if(plan.commands.packets().begin(),plan.commands.packets().end(),[&](const auto& packet){return packet.offsetWords==at;});
            for(unsigned n=0;n<=p.payloadWords;++n)for(unsigned bit=0;bit<32;++bit){auto bad=words;bad[at+n]^=1u<<bit;rejectWords(bad);}
            auto bad=words;bad.erase(bad.begin()+at,bad.begin()+at+p.payloadWords+1);rejectWords(bad);
            bad=words;bad.insert(bad.begin()+at,words.begin()+at,words.begin()+at+p.payloadWords+1);rejectWords(bad);
        }
        for(const Words& extra:std::vector<Words>{{0x5C8,0x20000},{0xC0003B00,0x7FFF},{0xC0025800,3,0x6002,1},
                {0xC0004600,6},{0x123,0},{0xC0022100,0x5C8,0xFFFFFFFF,0}}){
            auto bad=words;bad.insert(bad.end(),extra.begin(),extra.end());rejectWords(bad);
        }
        // Its sync requests cannot be borrowed by another byte snapshot.
        auto other=words;other.back()=1;const auto different=frame(other);assert(different);
        assert(!planNativeDeferredColorSync(*different,plan,sync));
        const auto before=reads;
        assert(!NativeDeferredColorCapture::create(*submitted,{},*fixture.nativeProgram,{}));
        DrawBinState invalid;invalid.knownWords=16;
        assert(!NativeDeferredColorCapture::create(*submitted,invalid,*fixture.nativeProgram,reader) && reads==before);
        const auto vertexAt=plan.draws[3].registers.at(0x4800).lastAssignmentOffset+1;
        for(auto address:{0x2000000u,0x3000u,0x30000u,0x8000u,0x9000u,0x9FFCu}){
            auto bad=words;bad[vertexAt]=address|3;const auto changed=frame(bad);assert(changed);
            assert(!NativeDeferredColorCapture::create(*changed,{},*fixture.nativeProgram,reader) && reads==before);
        }
        auto wrongTarget=words;wrongTarget[fixture.copyAddressWord]+=0x400000;const auto wrong=frame(wrongTarget);assert(wrong);
        assert(!NativeDeferredColorCapture::create(*wrong,{},*fixture.nativeProgram,reader));
        for(unsigned fault=0;fault<4;++fault){
            const Pm4PhysicalReader failed=[&](auto,auto size)->std::optional<std::vector<std::uint8_t>>{
                if(fault==0)return std::nullopt;if(fault==1)throw 1;return std::vector<std::uint8_t>(size+(fault==2?1:-1));};
            assert(!NativeDeferredColorCapture::create(*submitted,{},*fixture.nativeProgram,failed));
        }
        fixture.raw[0][0]^=1;
        assert(!NativeDeferredColorCapture::create(*submitted,{},*fixture.nativeProgram,reader));
        fixture.raw[0][0]^=1;
        assert(NativeDeferredColorCapture::create(*submitted,{},*fixture.nativeProgram,reader));
        std::printf("DEFERRED_COLOR_INPUTS: constant_zero=%u source_bytes=%u exact_profile/geometry/copy/source_alias/atomic_failure PASS; GPU_EXECUTED=0\n",constantZero,count);
    }
    std::printf("DEFERRED_COLOR_SYNC: %u request refusals,%u framing refusals; no old final events or retirement PASS\n",syncRejected,framingRejected);
}
