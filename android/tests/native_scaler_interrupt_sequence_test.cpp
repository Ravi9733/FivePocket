#include "native_scaler_interrupt_sequence.h"
#include "native_identity_scaler_fixture.h"
#include "native_scaler_wrapper_fixture.h"
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace recomp::gpu;
using namespace recomp::gpu::test;
int main() {
    const auto record=identityScalerRecord();
    const auto candidate=NativeIdentityScalerCandidate::inspect(identityScalerInputs(),record.data(),record.size());assert(candidate);
    const auto scaler=NativeIdentityScalerPlan::make(*candidate);assert(scaler);
    const auto command=NativeScalerCommand::make({1,43,0x64000000},*scaler);assert(command);
    auto bytes=scalerWrapperBytes(*command,0x20000,0x82004000,0x208);
    const auto wrapper=NativeScalerWrapper::parse(*command,0x10000,0x10114,bytes.data(),bytes.size());assert(wrapper);
    using Sequence=NativeScalerInterruptSequence;using Kind=NativeScalerWrapper::Kind;using Phase=Sequence::Phase;
    for (unsigned failAt=0;failAt<14;++failAt) for (bool throws:{false,true}) {
        Sequence sequence(*wrapper);unsigned calls=0;
        const auto action=[&](const auto&) {
            if (calls++==failAt) {if (throws) throw std::runtime_error("action failure");return false;}return true;
        };
        bool caught=false;
        try {
            const auto prefixed=sequence.prefix(0x20000,0x20037,action);
            if (failAt<10) assert(!prefixed);
            else {assert(prefixed && sequence.enterGuest() && sequence.guestReturned());assert(!sequence.acknowledge(action));}
        } catch (const std::runtime_error&) {caught=true;}
        assert(caught==throws && sequence.phase()==Phase::Failed && calls==failAt+1);
        assert(sequence.nextOperation()==failAt+(failAt<10?0:1));
        const auto forbidden=[](const auto&)->bool {assert(false);return true;};
        assert(!sequence.prefix(0x20000,0x20037,forbidden) && !sequence.enterGuest() && !sequence.guestReturned() &&
            !sequence.acknowledge(forbidden));
    }
    Sequence sequence(*wrapper);std::array<unsigned,8> scratch{};scratch[2]=0xABCD;scratch[4]=0x0BADF00D;
    std::vector<unsigned> words;unsigned fences=0,acks=0;
    const auto action=[&](const auto& operation) {
        words.push_back(operation.wordOffset);
        switch(operation.kind) {
        case Kind::BinMaskLow:return operation.value==0xFFFFFFFF;
        case Kind::ScratchWrite:scratch[operation.target]=operation.value;return true;
        case Kind::WaitScratch:return scratch[operation.target]==operation.value;
        case Kind::WaitIdle:assert(scratch[1]==1);++fences;return true;
        case Kind::CallbackAcknowledge:++acks;return operation.value==4;
        default:assert(false);return false;
        }
    };
    assert(!sequence.enterGuest() && !sequence.guestReturned() && !sequence.acknowledge(action));
    assert(!sequence.prefix(0x20004,0x20037,action) && !sequence.prefix(0x20000,0x20003,action));
    assert(!sequence.prefix(0x20000,0x20037,{}) && words.empty() && sequence.phase()==Phase::Ready);
    assert(sequence.prefix(0x20000,0x20037,action) && fences==1 && !acks && sequence.nextOperation()==10);
    assert(scratch[0]==4 && scratch[1]==1 && scratch[4]==0x82004000 && scratch[5]==0x208 && scratch[2]==0xABCD);
    assert(!sequence.prefix(0x20000,0x20037,action) && !sequence.acknowledge(action));
    assert(sequence.enterGuest() && !sequence.enterGuest() && !sequence.acknowledge(action));
    // Synthetic guest side effects, deliberately outside the action provider.
    scratch[0]&=~4u;scratch[1]=0;
    assert(sequence.guestReturned() && !sequence.guestReturned());
    assert(sequence.acknowledge(action) && acks==1 && sequence.phase()==Phase::Acknowledged && sequence.nextOperation()==15);
    assert(scratch[4]==0x0BADF00D && scratch[2]==0xABCD && !sequence.acknowledge(action));
    assert((words==std::vector<unsigned>{0,2,4,10,12,12,15,17,23,29,37,39,45,47}));
    // A callback return is not acknowledgement. Actual memory must satisfy it.
    Sequence missing(*wrapper);assert(missing.prefix(0x20000,0x20037,action) && missing.enterGuest() && missing.guestReturned());
    assert(!missing.acknowledge(action) && missing.phase()==Phase::Failed && missing.nextOperation()==12);
    assert(scratch[0]==4 && scratch[1]==1 && scratch[4]==0x82004000);
    assert(!missing.completeTail([](const auto&)->bool {assert(false);return false;}));
    for (unsigned failAt=15;failAt<26;++failAt) for (bool throws:{false,true}) {
        Sequence tail(*wrapper);const auto ok=[](const auto&){return true;};
        assert(!tail.completeTail(ok) && tail.prefix(0x20000,0x20037,ok) && !tail.completeTail(ok));
        assert(tail.enterGuest() && !tail.completeTail(ok) && tail.guestReturned() && !tail.completeTail(ok));
        assert(tail.acknowledge(ok));unsigned calls=0;bool caught=false;
        try {assert(!tail.completeTail([&](const auto& op) {
            assert(op.wordOffset==wrapper->operations()[15+calls].wordOffset);
            if (15+calls++==failAt) {if (throws) throw std::runtime_error("tail failure");return false;}return true;
        }));} catch (const std::runtime_error&) {caught=true;}
        assert(caught==throws && tail.phase()==Phase::Failed && tail.nextOperation()==failAt && calls==failAt-14);
        assert(!tail.completeTail(ok) && !tail.acknowledge(ok) && !tail.prefix(0x20000,0x20037,ok));
    }
    unsigned tailCalls=0;assert(!sequence.completeTail({}));
    assert(sequence.completeTail([&](const auto& op) {
        assert(op.wordOffset==wrapper->operations()[15+tailCalls++].wordOffset);return true;
    }));
    assert(tailCalls==11 && sequence.phase()==Phase::Complete && sequence.nextOperation()==26);
    assert(!sequence.completeTail(action) && !sequence.acknowledge(action));
    std::puts("NATIVE_SCALER_INTERRUPT_SEQUENCE: 14ordered actions,28partial failures,true zero waits,no scaler/tail retirement PASS; CPU fixture only");
    std::puts("NATIVE_SCALER_TAIL_SEQUENCE:11ordered actions,22terminal failures,requires completed IRQ acknowledgement PASS; CPU fixture only");
}
