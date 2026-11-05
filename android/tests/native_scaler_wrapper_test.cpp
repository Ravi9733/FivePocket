#include "native_identity_scaler_fixture.h"
#include "native_scaler_wrapper_fixture.h"
#include "xenos_command_stream.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    using Wrapper=NativeScalerWrapper;using Kind=Wrapper::Kind;
    constexpr std::uint32_t child=0x2000,commandPhysical=child+Wrapper::CommandOffsetWords*4;
    constexpr std::uint32_t scratch=0x10000,callback=0x80004000,context=0x12345678;
    const auto record=test::identityScalerRecord();
    const auto candidate=NativeIdentityScalerCandidate::inspect(test::identityScalerInputs(),record.data(),record.size());assert(candidate);
    const auto plan=NativeIdentityScalerPlan::make(*candidate);assert(plan);
    const auto command=NativeScalerCommand::make({73,43,0x60002000},*plan);assert(command);
    auto bytes=test::scalerWrapperBytes(*command,scratch,callback,context);const auto original=bytes;
    const auto parse=[&] { return Wrapper::parse(*command,child,commandPhysical,bytes.data(),bytes.size()); };
    const auto good=parse();assert(good);
    assert(good->scratchPhysical()==scratch && good->callbackAddress()==callback && good->callbackContext()==context);
    assert(good->RequiresGuestInterrupt && good->RequiresScalerUpdate && good->InterruptCpuIndex==2 && good->InterruptCpuMask==4);
    assert(good->RequiredScratchSlots==0x33);
    for (unsigned mask=0;mask<256;++mask)
        assert(good->scratchStateMatches(scratch,0x20000|mask)==((mask&0x33)==0x33));
    for (unsigned bit=8;bit<32;++bit) assert(!good->scratchStateMatches(scratch,0x20037^(1u<<bit)));
    for (unsigned bit=0;bit<32;++bit) assert(!good->scratchStateMatches(scratch^(1u<<bit),0x20037));
    const std::array<Kind,26> kinds{Kind::BinMaskLow,Kind::ScratchWrite,Kind::WaitScratch,Kind::WaitIdle,
        Kind::ScratchWrite,Kind::ScratchWrite,Kind::ScratchWrite,Kind::WaitScratch,Kind::WaitScratch,Kind::WaitScratch,
        Kind::Interrupt,Kind::CallbackAcknowledge,Kind::WaitScratch,Kind::ScratchWrite,Kind::WaitScratch,
        Kind::ScalerUpdateLock,Kind::ScalerEnable,Kind::ScalerUpdateLock,Kind::WaitScalerUpdate,Kind::FlipControl,
        Kind::MasterUpdateLock,Kind::IdentityScaler,Kind::InvalidateState,Kind::DiagnosticNop,Kind::LiteralEvent,Kind::LiteralEvent};
    const std::array<unsigned,26> offsets{0,2,4,10,12,12,15,17,23,29,35,37,39,45,47,53,55,57,59,65,67,69,133,135,137,141};
    const std::array<unsigned,26> targets{0,1,1,0x5C8,4,5,0,0,5,4,4,0x45E,0,4,1,0x1973,0x1964,0x1973,
        0x1973,0x1852,0x1838,commandPhysical,0,0,0x30000,0x31000};
    const std::array<unsigned,26> values{0xFFFFFFFF,1,1,0x20000,callback,context,4,4,context,callback,4,4,0,
        0x0BADF00D,0,0x10000,1,0,0,0,1,64,0x7FFF,0x77777777,0x123,0xABC};
    for (unsigned i=0;i<26;++i) {
        const auto& op=good->operations()[i];
        assert(op.kind==kinds[i] && op.wordOffset==offsets[i] && op.target==targets[i] && op.value==values[i]);
    }
    unsigned rejected=0;
    for (unsigned w=0;w<Wrapper::Words;++w) {
        if (w==139 || w==140 || w==143 || w==144) continue; // Variable event destinations and literals.
        for (unsigned b=0;b<32;++b) { bytes[w*4+b/8]^=1u<<(b%8);assert(!parse());bytes=original;++rejected; }
    }
    const auto put=[&](unsigned at,unsigned value) { for (unsigned b=0;b<4;++b) bytes[at*4+b]=value>>(24-b*8); };
    for (auto destination:{child,child+Wrapper::Bytes-4,scratch,scratch+28,0x20000000u}) {
        for (auto at:{139u,143u}) { put(at,destination|2);assert(!parse());bytes=original; }
    }
    put(143,0x30002);assert(!parse());bytes=original;
    for (unsigned b=0;b<2;++b) for (auto at:{139u,143u}) { bytes[at*4+3]^=1u<<b;assert(!parse());bytes=original; }
    for (unsigned at:{140u,144u}) for (unsigned value:{0u,0xFFFFFFFFu,0x98765432u}) {
        put(at,value);const auto changed=parse();assert(changed && changed->operations()[at==140?24:25].value==value);bytes=original;
    }
    // Coherent parameter changes are valid request plans, not arbitrary code
    // execution. Live authority/function mapping/ownership are separate gates.
    for (unsigned base:{0u,0x8000u,0x1FFFFFE0u}) {
        bytes=test::scalerWrapperBytes(*command,base,callback,context);assert(parse());
    }
    for (unsigned base:{child,child+Wrapper::Bytes-4,0x1FFFFFF0u,0x20000000u}) {
        bytes=test::scalerWrapperBytes(*command,base,callback,context);assert(!parse());
    }
    for (unsigned address:{0u,1u,callback+1,0x0BADF00Du}) {
        bytes=test::scalerWrapperBytes(*command,scratch,address,context);assert(!parse());
    }
    bytes=test::scalerWrapperBytes(*command,scratch,callback+4,context+1);assert(parse());bytes=original;
    for (unsigned size=0;size<Wrapper::Bytes;++size) assert(!Wrapper::parse(*command,child,commandPhysical,bytes.data(),size));
    assert(!Wrapper::parse(*command,child,commandPhysical,bytes.data(),bytes.size()+4));
    assert(!Wrapper::parse(*command,child,commandPhysical,nullptr,bytes.size()));
    assert(!Wrapper::parse(*command,child+1,commandPhysical,bytes.data(),bytes.size()));
    assert(!Wrapper::parse(*command,child,commandPhysical+4,bytes.data(),bytes.size()));
    assert(!Wrapper::parse(*command,0x1FFFFE00,0x1FFFFF14,bytes.data(),bytes.size()));
    const auto wrong=NativeScalerCommand::make({74,43,0x60002000},*plan);assert(wrong);
    assert(!Wrapper::parse(*wrong,child,commandPhysical,bytes.data(),bytes.size()));
    Pm4Snapshot snapshot;Pm4Error error;
    assert(!decodePm4(bytes.data(),bytes.size(),{},snapshot,error) && error.offsetWords==69 && error.failure==Pm4Failure::TruncatedPacket);
    bytes.fill(0);assert(good->command().matches(command->bytes().data(),NativeScalerCommand::Bytes));
    assert(good->operations()[4].value==callback && good->operations()[25].value==0xABC);
    std::printf("NATIVE_SCALER_WRAPPER_PLAN: %u bit controls,26 ordered obligations,callback waits and ownership boundaries PASS; no execution\n",rejected);
}
