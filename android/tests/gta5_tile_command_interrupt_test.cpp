#include "gta5_tile_command_interrupt.h"
#include "gta5_tile_scene_continuation.h"
#include "gta5_tile_deferred_tail.h"
#include "native_serialized_resource_batch.h"
#include "native_tile_command_interrupt.h"
#include "native_tile_deferred_tail.h"
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace recomp::gpu;
int main() {
    using Plan=Gta5TileCommandInterrupt;using Sequence=Gta5TileCommandInterruptSequence;
    constexpr unsigned Child=0x3000,Scratch=0x1000,Context=0xC0100000;
    const std::array<unsigned,35> words{0x1057C,Plan::Callback,Context,0x578,4,
        0xC0043C00,0x13,Scratch+2,4,UINT32_MAX,0x100,
        0xC0043C00,0x13,Scratch+22,Context,UINT32_MAX,0x100,
        0xC0043C00,0x13,Scratch+18,Plan::Callback,UINT32_MAX,0x100,
        0xC0005400,4,0x45E,4,0xC0043C00,0x13,Scratch+2,0,UINT32_MAX,0x100,
        0x57C,0x0BADF00D};
    std::vector<std::uint8_t> bytes(Plan::Bytes);
    const auto put=[&](unsigned index,unsigned value) {
        for(unsigned i=0;i<4;++i)bytes[index*4+i]=value>>(24-i*8);
    };
    for(unsigned i=0;i<words.size();++i)put(i,words[i]);
    const auto plan=Plan::parse(Child,bytes);assert(plan);
    assert(plan->callbackContext()==Context && plan->scratchPhysical()==Scratch);
    assert(plan->scratchStateMatches(Scratch,0x20031));
    assert(plan->scratchStateMatches(Scratch,0x200FF));
    assert(!plan->scratchStateMatches(Scratch,0x20030));
    assert(!plan->scratchStateMatches(Scratch,0x31));
    assert(!plan->scratchStateMatches(Scratch+4,0x20031));
    unsigned negatives=0;
    for(unsigned i=0;i<words.size();++i) {
        put(i,words[i]^1);assert(!Plan::parse(Child,bytes));++negatives;put(i,words[i]);
    }
    for(unsigned n=0;n<Plan::Bytes;++n) {
        assert(!Plan::parse(Child,std::span(bytes).first(n)));++negatives;
    }
    bytes.push_back(0);assert(!Plan::parse(Child,bytes));bytes.pop_back();++negatives;
    for(unsigned child:{Child+1,Scratch,Scratch+28,0x1FFFFF80u,0x100000u}) {
        assert(!Plan::parse(child,bytes));++negatives;
    }
    for(unsigned context:{0u,0xA0100000u,0xC0000000u,0xC0001000u,0xC0003000u,0xDFFFFFFCu}) {
        put(2,context);put(14,context);assert(!Plan::parse(Child,bytes));++negatives;
    }
    put(2,Context);put(14,Context);
    assert(Plan::parse(Child,bytes)==plan);
    unsigned phaseCases=0;
    for(int failAt=-1;failAt<10;++failAt) {
        if(failAt==6)continue; // IRQ cannot be executed by the action provider.
        Sequence s(*plan);unsigned invoked=0;
        const auto action=[&](const auto& op) {
            assert(op.kind!=Plan::Kind::Interrupt);
            const auto at=s.nextOperation();assert(op==plan->operations()[at]);++invoked;
            return int(at)!=failAt;
        };
        assert(!s.acknowledge(action) && !s.enterGuest() && !s.guestReturned());
        const bool prefix=s.prefix(Scratch,0x20031,action);
        if(failAt>=0 && failAt<6) {
            assert(!prefix && s.phase()==Sequence::Phase::Failed && invoked==unsigned(failAt+1));
        } else {
            assert(prefix && invoked==6 && !s.acknowledge(action));
            assert(s.enterGuest() && !s.enterGuest() && !s.acknowledge(action));
            assert(s.guestReturned() && !s.guestReturned());
            const auto ok=s.acknowledge(action);
            assert(ok==(failAt<0));
            assert(s.phase()==(ok?Sequence::Phase::Complete:Sequence::Phase::Failed));
            if(ok)assert(invoked==9);
        }
        const auto before=invoked;
        assert(!s.prefix(Scratch,0x20031,action) && !s.acknowledge(action) && invoked==before);
        ++phaseCases;
    }
    Sequence exception(*plan);
    try{exception.prefix(Scratch,0x20031,[](const auto&)->bool{throw std::runtime_error("injected");});assert(false);}
    catch(const std::runtime_error&){assert(exception.phase()==Sequence::Phase::Failed);}
    using Tail=Gta5TileSceneContinuation;
    std::vector<std::uint8_t> tail(Tail::Bytes);
    const auto tailPut=[&](unsigned index,unsigned value) {
        for(unsigned i=0;i<4;++i)tail[index*4+i]=value>>(24-i*8);
    };
    tailPut(0,0xC0006000);tailPut(1,UINT32_MAX);tailPut(2,0xC0006000);
    tailPut(3,0x80000000);tailPut(4,0x5C8);tailPut(5,0x20000);
    for(unsigned i=0;i<words.size();++i)tailPut(i+6,words[i]);
    for(unsigned i:{5u,11u,17u,23u,27u})tailPut(i+6,words[i]|1);
    tailPut(41,0xC0006000);tailPut(42,UINT32_MAX);
    const auto continuation=Tail::parse(Child,tail);assert(continuation);
    assert(continuation->request().predicated());
    assert(!Plan::parse(Child+24,std::span(tail).subspan(24,140)));
    Sequence wrongSequence(continuation->request());unsigned wrongCalls=0;
    assert(!wrongSequence.prefix(Scratch,0x20031,[&](const auto&){++wrongCalls;return true;}) && !wrongCalls);
    unsigned tailNegatives=0;
    for(unsigned i=0;i<tail.size();++i) {
        tail[i]^=1;assert(!Tail::parse(Child,tail));++tailNegatives;tail[i]^=1;
    }
    for(unsigned n=0;n<Tail::Bytes;++n){assert(!Tail::parse(Child,std::span(tail).first(n)));++tailNegatives;}
    tail.push_back(0);assert(!Tail::parse(Child,tail));tail.pop_back();++tailNegatives;
    for(unsigned physical:{Child+1,0x1FFFFF80u,0x100000u,Scratch}) {
        assert(!Tail::parse(physical,tail));++tailNegatives;
    }
    const auto first=continuation->select({{UINT32_MAX,0,0x80000003,0},15});
    assert(first && first->interruptRequired && first->operationCount==10);
    for(unsigned i=0;i<10;++i)assert(first->operationIndices[i]==i);
    const auto second=continuation->select({{0,0,0xC,0},15});
    assert(second && !second->interruptRequired && second->operationCount==5);
    const std::array<unsigned,5> unconditional{0,1,2,7,9};
    for(unsigned i=0;i<5;++i)assert(second->operationIndices[i]==unconditional[i]);
    assert(second->finalBins.words[0]==UINT32_MAX && second->finalBins.words[2]==0xC);
    // A high-word intersection also satisfies the real 64-bit predicate.
    const auto high=continuation->select({{0,4,0xC,4},15});assert(high && high->interruptRequired);
    assert(!continuation->select({}));
    assert(!continuation->select({{0,0,0xC,0},5})); // Unknown high words cannot prove Skip.
    assert(!continuation->select({{1,0,0,0},0})); // Inconsistent unknown word.
    assert(!continuation->select({{0,0,0,0},16}));
    {
        using Native=NativeTileCommandInterrupt;
        unsigned nativeCases=0;
        // Connected producer -> original-handler boundary -> scratch consumer.
        // Host fences here are injected contracts, not claimed Vulkan evidence.
        for(unsigned profile=0;profile<3;++profile)for(int fault=-1;fault<14;++fault) {
            Native native=profile?Native(*continuation):Native(*plan);
            NativeGraphicsState state;state.binMask=0;state.binSelect=profile==2?0xC:0x80000003;state.knownBinWords=15;
            Native::Journal journal;std::array<unsigned,8> scratch{};scratch[2]=0xABCD;
            unsigned fenced=0,guests=0,memoryCalls=0;bool ackRead=false;
            const auto memory=[&](const auto& op) {
                assert(fenced==1 && op.target<8 && op.kind!=Plan::Kind::Interrupt && op.kind!=Plan::Kind::CallbackAcknowledge);
                const auto call=memoryCalls++;
                if(fault==int(call))return false;
                if(fault==11 && !call)throw std::runtime_error("memory failure");
                if(op.kind==Plan::Kind::ScratchWrite)scratch[op.target]=op.value;
                else {if(!op.value)ackRead=true;return scratch[op.target]==op.value;}
                return true;
            };
            const auto guest=[&](const auto& request) {
                ++guests;assert(fenced==1 && scratch[0]==4 && scratch[4]==Plan::Callback && scratch[5]==Context);
                assert(request.callbackContext()==Context && scratch[2]==0xABCD);
                if(fault==9)return false;
                if(fault==12)throw std::runtime_error("guest failure");
                if(fault==13)assert(!native.execute(Scratch,0x20031,state,journal,[]{return true;},memory,{}));
                if(fault!=10)scratch[0]=0; // Only guest handler supplies the zero.
                return true;
            };
            bool ok=false,threw=false;
            try {ok=native.execute(Scratch,0x20031,state,journal,[&]{++fenced;return fault!=8;},memory,guest);}
            catch(const std::runtime_error&){threw=true;}
            const bool skipped=profile==2;
            const bool expected=fault<0 || (skipped && ((fault>=4 && fault<=7) || fault==9 || fault==10 || fault==12 || fault==13));
            assert(ok==expected && native.phase()==(ok?Native::Phase::Complete:Native::Phase::Failed));
            assert(threw==(fault==11 || (fault==12 && !skipped)));
            if(ok) {
                assert(fenced==1 && guests==unsigned(!skipped) && ackRead==!skipped);
                assert(scratch[0]==(skipped?4u:0u) && scratch[2]==0xABCD && scratch[4]==0x0BADF00D && scratch[5]==Context);
                assert(journal.at(0x578).first==4 && journal.at(0x45E).first==4 && journal.at(0x57C).first==0x0BADF00D);
                assert(native.completedOperations()==(skipped?5u:10u));
                if(profile)assert(state.binMask==UINT32_MAX && journal.at(0x5C8).first==0x20000);
            }
            if(fault==8)assert(!memoryCalls && !guests && scratch[0]==0);
            if(fault==9 && !skipped)assert(scratch[0]==4 && scratch[4]==Plan::Callback && !ackRead);
            if(fault==10 && !skipped)assert(scratch[0]==4 && scratch[4]==Plan::Callback && ackRead);
            const auto saved=scratch;
            assert(!native.execute(Scratch,0x20031,state,journal,[]{return true;},memory,guest) && scratch==saved);
            ++nativeCases;
        }
        for(unsigned fault=0;fault<4;++fault) {
            Native n(fault==3?Native(continuation->request()):Native(*continuation));
            NativeGraphicsState s;Native::Journal j;unsigned actions=0;
            if(fault!=2){s.knownBinWords=15;s.binSelect=0x80000003;}
            assert(!n.execute(Scratch+(fault==0?4:0),fault==1?0x20030:0x20031,s,j,
                [&]{++actions;return true;},[&](const auto&){++actions;return true;},[&](const auto&){++actions;return true;}));
            assert(!actions && j.empty());++nativeCases;
        }
        std::printf("NATIVE_TILE_INTERRUPT_CONNECTION_PASS cases=%u fence_before_scratch=1 guest_zero_required=1 predicated_skip_keeps_writes=1 failures_latched=1 host_contract_only=1\n",nativeCases);
    }
    std::printf("GTA5_TILE_SCENE_CONTINUATION_PASS negatives=%u first_irq=1 second_irq=0 unconditional_writes_retained=5 unknown_bins_rejected=1\n",tailNegatives);
    const std::array<unsigned,12> deferredWords{0xC0003B00,0x7FFF,0xC0001000,0x77777777,
        0xC0025800,3,Scratch+6,0xA0123451,0xC0025800,3,Scratch+2,25};
    std::vector<std::uint8_t> deferred(48),state(21944);
    const auto store=[](auto& b,unsigned at,unsigned v){for(unsigned i=0;i<4;++i)b[at+i]=v>>(24-i*8);};
    for(unsigned i=0;i<12;++i)store(deferred,i*4,deferredWords[i]);
    const auto final=Gta5TileDeferredTail::parse(Child,deferred);assert(final && !final->requiresWaitUntil());
    assert(final->fencePhysical()==Scratch && final->completionValue()==25 && final->commandCursor()==0xA0123451);
    store(state,11024,0xA0001000);store(state,11036,27);store(state,13608,0xA0123450);store(state,13612,1);
    assert(final->matchesProducedState(state,0xA0001000,Scratch));
    assert(!final->matchesProducedState(state,0xA0001000,Scratch+4));
    assert(!final->matchesProducedState(state,0xA0001004,Scratch));
    assert(!final->matchesProducedState(std::span(state).first(15327),0xA0001000,Scratch));
    for(unsigned at:{11024u,11036u,13608u,13612u}) {
        state[at+3]^=2;assert(!final->matchesProducedState(state,0xA0001000,Scratch));state[at+3]^=2;
    }
    state[13608+3]|=1;assert(final->matchesProducedState(state,0xA0001000,Scratch));state[13608+3]&=~1u;
    state[13612+3]|=0x80;assert(final->matchesProducedState(state,0xA0001000,Scratch));state[13612+3]&=~0x80u;
    const auto updates=final->stagingPlan();assert(updates.count==2 && updates.bufferBytes==8);
    assert(updates.words[0].byteOffset==4 && updates.words[1].byteOffset==0);
    assert(updates.words[0].phase==NativeWordPhase::AfterGraphics && updates.words[1].phase==NativeWordPhase::AfterGraphics);
    assert((updates.words[0].bigEndianBytes==std::array<std::uint8_t,4>{0xA0,0x12,0x34,0x51}));
    assert((updates.words[1].bigEndianBytes==std::array<std::uint8_t,4>{0,0,0,25}));
    unsigned deferredNegatives=0;
    for(unsigned i:{0u,1u,2u,3u,4u,5u,6u,8u,9u,10u}) {
        store(deferred,i*4,deferredWords[i]^1);assert(!Gta5TileDeferredTail::parse(Child,deferred));
        store(deferred,i*4,deferredWords[i]);++deferredNegatives;
    }
    for(unsigned n=0;n<48;++n){assert(!Gta5TileDeferredTail::parse(Child,std::span(deferred).first(n)));++deferredNegatives;}
    for(unsigned address:{1u,Scratch,Scratch+4,0x1FFFFFE0u}) {
        assert(!Gta5TileDeferredTail::parse(address,deferred));++deferredNegatives;
    }
    for(unsigned cursor:{0u,0x9FFFFFFFu,0xC0000000u,UINT32_MAX}) {
        store(deferred,28,cursor);assert(!Gta5TileDeferredTail::parse(Child,deferred));++deferredNegatives;
    }
    store(deferred,28,deferredWords[7]);store(deferred,8,0x5C8);store(deferred,12,0x20000);
    const auto waits=Gta5TileDeferredTail::parse(Child,deferred);assert(waits && waits->requiresWaitUntil());
    // Original addi wraps the producer's next ID; no fabricated signed ordering.
    store(deferred,44,UINT32_MAX);store(state,11036,1);
    assert(Gta5TileDeferredTail::parse(Child,deferred)->matchesProducedState(state,0xA0001000,Scratch));
    std::printf("GTA5_TILE_DEFERRED_TAIL_PASS negatives=%u profiles=2 cursor_before_fence=1 producer_identity=1 no_publication=1\n",deferredNegatives);
    {
        unsigned cases=0;
        for(unsigned profile=0;profile<2;++profile)for(int fault=-1;fault<10;++fault) {
            using Native=NativeTileDeferredTail;
            Native execution(profile?*waits:*final);
            NativeGraphicsState s;s.invalidationEpoch=fault==9?UINT64_MAX:8;s.binMask=0x1234;s.knownBinWords=15;
            Native::Journal j{{0x2000,{0xABC,UINT32_MAX}},{0x5C8,{7,UINT32_MAX}},{0x21F9,{9,UINT32_MAX}}};
            std::vector<unsigned> order;unsigned publications=0;
            const Native::Fence fence=[&] {
                order.push_back(0);if(fault==3)throw std::runtime_error("fence failure");
                if(fault==6)assert(!execution.execute(s,j,[]{return true;},[](auto){return true;},[](const auto&){return true;}));
                return fault!=0;
            };
            const Native::Invalidate invalidate=[&](auto mask) {
                assert(order==std::vector<unsigned>{0} && mask==0x7FFF);order.push_back(1);
                if(fault==4)throw std::runtime_error("invalidate failure");
                if(fault==7)assert(!execution.execute(s,j,fence,{},{}));
                return fault!=1;
            };
            const Native::Publish publish=[&](const auto& p) {
                assert((order==std::vector<unsigned>{0,1}));order.push_back(2);
                assert(s.invalidationEpoch==9 && j.at(0x21F9).first==3 && j.at(0x5C8).first==(profile?0x20000u:7u));
                assert(p.count==2 && p.bufferBytes==8);
                for(unsigned i=0;i<2;++i)assert(p.words[i].byteOffset==updates.words[i].byteOffset &&
                    p.words[i].bigEndianBytes==updates.words[i].bigEndianBytes);
                if(fault==5)throw std::runtime_error("publication failure");
                if(fault==8){assert(!execution.execute(s,j,fence,invalidate,{}));return false;}
                if(fault==2)return false;
                ++publications;return true;
            };
            bool ok=false,threw=false;
            try{ok=execution.execute(s,j,fence,invalidate,publish);}catch(const std::runtime_error&){threw=true;}
            assert(ok==(fault==-1) && threw==(fault>=3 && fault<=5) && publications==unsigned(ok));
            assert(execution.phase()==(ok?Native::Phase::Complete:Native::Phase::Failed));
            assert(s.binMask==0x1234 && s.knownBinWords==15 && j.at(0x2000).first==0xABC);
            if(fault==9)assert(order.empty() && j.at(0x21F9).first==9);
            else if(order.size()<3)assert(s.invalidationEpoch==8 && j.at(0x21F9).first==9);
            else assert(s.invalidationEpoch==9); // Do not undo an actual invalidation on failed publication.
            const auto saved=order;assert(!execution.execute(s,j,fence,invalidate,publish) && order==saved);++cases;
        }
        for(unsigned missing=0;missing<4;++missing) {
            NativeTileDeferredTail n(missing==3?Gta5TileDeferredTail{}:*final);
            NativeGraphicsState s;NativeTileDeferredTail::Journal j;unsigned called=0;
            NativeTileDeferredTail::Fence f=[&]{++called;return true;};
            NativeTileDeferredTail::Invalidate i=[&](auto){++called;return true;};
            NativeTileDeferredTail::Publish p=[&](const auto&){++called;return true;};
            if(missing==0)f={};if(missing==1)i={};if(missing==2)p={};
            assert(!n.execute(s,j,f,i,p) && !called && j.empty());++cases;
        }
        std::printf("NATIVE_TILE_DEFERRED_CONNECTION_PASS cases=%u profiles=2 fence_before_publication=1 actual_invalidation_required=1 event_initiator=21f9 cursor_before_fence=1 failures_latched=1 host_contract_only=1\n",cases);
    }
    {
        std::array<std::vector<std::uint8_t>,5> textures{},oldTextures{},newTextures{};
        for(unsigned i=0;i<5;++i){textures[i].assign(16,0xA5);newTextures[i].assign(16,i+1);}oldTextures=textures;
        std::vector<std::uint8_t> fence{0,0,0,23,0xA0,0,0,0};const auto oldFence=fence;
        std::array<std::vector<std::uint8_t>,2> oldWords,newWords;
        std::array<NativeSerializedResourceUpdate,7> batch{};
        for(unsigned i=0;i<5;++i)batch[i]={0xA0100000+i*32,{textures[i].data(),textures[i].size()},&oldTextures[i],&newTextures[i]};
        for(unsigned i=0;i<2;++i) {
            const auto& w=updates.words[i];
            oldWords[i].assign(oldFence.begin()+w.byteOffset,oldFence.begin()+w.byteOffset+4);
            newWords[i].assign(w.bigEndianBytes.begin(),w.bigEndianBytes.end());
            batch[5+i]={0xA0001000+w.byteOffset,{fence.data()+w.byteOffset,4},&oldWords[i],&newWords[i]};
        }
        assert(!publishNativeSerializedResourceBatch(nullptr,0));
        unsigned rejected=0;
        for(unsigned i=0;i<7;++i) {
            const auto original=batch[i];
            for(unsigned fault=0;fault<6;++fault) {
                batch[i]=original;
                switch(fault) {
                    case 0:batch[i].address|=1;break;
                    case 1:batch[i].expected=batch[i].completed;break;
                    case 2:batch[i].completed=&textures[0];break;
                    case 3:batch[i].completed={};break;
                    case 4:batch[i].destination={};break;
                    case 5:batch[i].address=UINT32_MAX-3;break;
                }
                // Four bytes ending exactly at4GiB are valid; use an8-byte
                // span for that overflow case independently below.
                if(fault==5 && original.destination.size()==4)continue;
                assert(!publishNativeSerializedResourceBatch(batch));++rejected;
                assert(textures==oldTextures && fence==oldFence);
            }
            batch[i]=original;
        }
        for(unsigned kind=0;kind<3;++kind) {
            const auto original=batch[1];
            if(kind==0)batch[1].address=batch[0].address;
            if(kind==1)batch[1].destination=batch[0].destination;
            if(kind==2)batch[1].completed=&textures[0];
            assert(!publishNativeSerializedResourceBatch(batch));++rejected;
            assert(textures==oldTextures && fence==oldFence);batch[1]=original;
        }
        assert(publishNativeSerializedResourceBatch(batch));
        assert(textures==newTextures && fence[3]==25);
        assert(std::equal(updates.words[0].bigEndianBytes.begin(),updates.words[0].bigEndianBytes.end(),fence.begin()+4));
        assert(!publishNativeSerializedResourceBatch(batch));++rejected; // Old receipt cannot publish twice.
        std::printf("NATIVE_SERIALIZED_RESOURCE_BATCH_PASS textures=5 tail_words=2 negatives=%u failure_atomic=1 gpu_execution=0\n",rejected);
    }
    std::printf("GTA5_TILE_COMMAND_INTERRUPT_PASS parser_negatives=%u phase_cases=%u guest_irq_not_synthesized=1\n",negatives,phaseCases);
}
