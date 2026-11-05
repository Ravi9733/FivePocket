#include "native_post_output_swap_interrupt_sequence.h"
#include "native_post_output_swap_fixture.h"
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace recomp::gpu;
template<unsigned Ticket> void exercise(){
    using Wrapper=NativeSurfaceSwapWrapper<Ticket>;
    const auto ticket=NativeSwapCommand::make({Ticket,0x2028,0x100000,67});assert(ticket);
    const auto bytes=test::postOutputSwapBytes(*ticket);
    const auto plan=Wrapper::parse(*ticket,0x2000,bytes.data(),bytes.size());assert(plan);
    using Sequence=NativeSurfaceSwapInterruptSequence<Ticket>;using Phase=typename Sequence::Phase;using Kind=typename Wrapper::Kind;
    const auto forbidden=[](const auto&)->bool{assert(false);return false;};
    const auto ok=[](const auto&){return true;};
    unsigned failures=0;
    for(unsigned fault=0;fault<11;++fault)for(unsigned behavior=0;behavior<3;++behavior){
        Sequence s(*plan);unsigned calls=0;bool caught=false;
        const auto action=[&](const auto& op){
            assert(op.kind!=Kind::Interrupt && !s.enterGuest() && !s.guestReturned() && !s.acknowledge(forbidden) &&
                !s.prefix(0x30000,0x20031,forbidden));
            if(calls++==fault){if(behavior==1)throw std::runtime_error("fixture action");
                if(behavior==2){s.fail();return true;}return false;}
            return true;
        };
        try{const bool began=s.prefix(0x30000,0x20031,action);
            if(fault<8)assert(!began);
            else{assert(began && s.enterGuest() && s.guestReturned());assert(!s.acknowledge(action));}
        }catch(const std::runtime_error&){caught=true;}
        assert(caught==(behavior==1) && calls==fault+1 && s.phase()==Phase::Failed &&
            s.nextOperation()==fault+(fault<8?0:1));
        assert(!s.prefix(0x30000,0x20031,forbidden) && !s.enterGuest() && !s.guestReturned() && !s.acknowledge(forbidden));++failures;
    }
    for(unsigned untouched1:{0u,1u,0xFFFFFFFFu}){
        Sequence s(*plan);std::array<unsigned,8> scratch{0,untouched1,0x234,0x345,0x0BADF00D,0x567,0x678,0x789};
        const auto original=scratch;std::vector<unsigned> offsets;unsigned fences=0,acks=0;
        const auto action=[&](const auto& op){offsets.push_back(op.wordOffset);
            switch(op.kind){
            case Kind::BinMaskLow:return op.value==0xFFFFFFFF;
            case Kind::WaitIdle:++fences;return op.value==0x20000;
            case Kind::ScratchWrite:assert(op.target!=1 && op.value);scratch[op.target]=op.value;return true;
            case Kind::WaitScratch:assert(op.target!=1);return scratch[op.target]==op.value;
            case Kind::CallbackAcknowledge:++acks;return op.value==4;
            default:assert(false);return false;
            }};
        assert(!s.enterGuest() && !s.guestReturned() && !s.acknowledge(forbidden));
        assert(!s.prefix(0x30004,0x20031,forbidden) && !s.prefix(0x30000,0x20011,forbidden) && !s.prefix(0x30000,0x20031,{}));
        assert(s.phase()==Phase::Ready && offsets.empty());
        assert(s.prefix(0x30000,0x20031,action) && s.nextOperation()==8 && fences==1 && !acks &&
            scratch[0]==4 && scratch[4]==plan->callbackAddress() && scratch[5]==0x100208);
        assert(!s.prefix(0x30000,0x20031,forbidden) && !s.guestReturned() && !s.acknowledge(forbidden));
        assert(s.enterGuest() && !s.enterGuest() && !s.acknowledge(forbidden));
        // Synthetic guest acknowledgement only; no host action manufactures it.
        scratch[0]&=~4u;
        assert(s.guestReturned() && !s.guestReturned() && !s.acknowledge({}));
        assert(s.acknowledge(action) && s.phase()==Phase::Acknowledged && s.nextOperation()==12 && acks==1);
        assert(!s.acknowledge(forbidden) && !s.enterGuest() && !s.guestReturned() && !s.prefix(0x30000,0x20031,forbidden));
        assert((offsets==std::vector<unsigned>{78,80,82,82,85,87,93,99,107,109,115}));
        for(unsigned i:{1u,2u,3u,6u,7u})assert(scratch[i]==original[i]);
        assert(!scratch[0] && scratch[4]==0x0BADF00D && scratch[5]==0x100208);
    }
    Sequence missing(*plan);unsigned ack=0,poison=0;
    assert(missing.prefix(0x30000,0x20031,ok) && missing.enterGuest() && missing.guestReturned());
    assert(!missing.acknowledge([&](const auto& op){
        if(op.kind==Kind::CallbackAcknowledge){++ack;return true;}
        if(op.kind==Kind::WaitScratch)return false;
        ++poison;return true;
    }));
    assert(missing.phase()==Phase::Failed && missing.nextOperation()==10 && ack==1 && !poison);
    assert(failures==33);
    std::printf("%s:11ordered actions,33terminal faults,true scratch0 wait,scratch1 untouched,old IRQ independent; no GPU/guest/presentation PASS\n",
        Ticket==2?"POST_OUTPUT_SWAP_INTERRUPT_SEQUENCE":"DEFERRED_SWAP_INTERRUPT_SEQUENCE");
}
int main(){
    static_assert(!std::is_constructible_v<NativeDeferredSwapInterruptSequence,const NativePostOutputSwapWrapper&>);
    static_assert(!std::is_convertible_v<NativePostOutputSwapInterruptSequence,NativeDeferredSwapInterruptSequence>);
    exercise<2>();exercise<3>();
}
