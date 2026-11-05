#include "native_tile_bin_command.h"
#include "gta5_tile_scene_continuation.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    unsigned cases=0;
    const auto bytes=[](unsigned header,unsigned value) {
        std::array<std::uint8_t,8> b{};
        for(unsigned i=0;i<4;++i){b[i]=header>>(24-i*8);b[4+i]=value>>(24-i*8);}return b;
    };
    const auto apply=[&](NativeGraphicsState& state,unsigned i,unsigned v) {
        const auto plan=NativeTileBinCommand::parse(bytes(0xC0006000+(i<<8),v));
        assert(plan && plan->index()==i && plan->value()==v && plan->apply(state));
    };
    const auto bins=[](const NativeGraphicsState& s) {
        return DrawBinState{{unsigned(s.binMask),unsigned(s.binMask>>32),unsigned(s.binSelect),unsigned(s.binSelect>>32)},s.knownBinWords};
    };
    for(unsigned i=0;i<4;++i) {
        NativeGraphicsState s; s.invalidationEpoch=7;s.experimentalIdleCachePairEpoch=9;s.batches=11;s.experimentalColorBatches=13;
        apply(s,i,0x12345678);const auto b=bins(s);
        assert(b.knownWords==(1u<<i) && b.words[i]==0x12345678);
        for(unsigned j=0;j<4;++j)if(i!=j)assert(!b.words[j]);
        assert(s.invalidationEpoch==7 && s.experimentalIdleCachePairEpoch==9 && s.batches==11 && s.experimentalColorBatches==13);++cases;
        apply(s,i,0);assert(bins(s).words[i]==0 && s.knownBinWords==(1u<<i));++cases;
    }
    NativeGraphicsState all;
    for(unsigned i=0;i<4;++i)apply(all,i,0x11111111u*(i+1));
    assert(all.binMask==0x2222222211111111ULL && all.binSelect==0x4444444433333333ULL && all.knownBinWords==15);++cases;
    for(unsigned i=0;i<4;++i)for(unsigned bit=0;bit<32;++bit) {
        const auto header=(0xC0006000u+(i<<8))^(1u<<bit);
        // Two opcode bits can select another supported bin word, not a malformed packet.
        const bool another=header==0xC0006000 || header==0xC0006100 || header==0xC0006200 || header==0xC0006300;
        assert(NativeTileBinCommand::parse(bytes(header,7)).has_value()==another);++cases;
    }
    const auto valid=bytes(0xC0006200,0x80000003);
    for(unsigned n=0;n<8;++n){assert(!NativeTileBinCommand::parse(std::span(valid).first(n)));++cases;}
    std::array<std::uint8_t,9> extra{};std::copy(valid.begin(),valid.end(),extra.begin());
    assert(!NativeTileBinCommand::parse(extra));++cases;
    const auto plan=NativeTileBinCommand::parse(valid);assert(plan);
    for(unsigned i=0;i<4;++i) {
        NativeGraphicsState bad;if(i<2)bad.binMask=std::uint64_t{1}<<(i*32);else bad.binSelect=std::uint64_t{1}<<((i-2)*32);
        const auto before=bins(bad);assert(!plan->apply(bad));const auto after=bins(bad);
        assert(before.words==after.words && before.knownWords==after.knownWords);++cases;
    }
    NativeGraphicsState bad;bad.knownBinWords=16;assert(!plan->apply(bad) && bad.knownBinWords==16);++cases;
    // Original worker SELECT_LO -> scene MASK_HI/LO -> first-pass predicate.
    // Missing high-word knowledge is not silently treated as a zero reset.
    NativeGraphicsState state;
    apply(state,2,0x80000003);apply(state,0,0x80000000);
    assert(evaluateDrawBinPredicate(bins(state),true)==DrawBinDecision::Pass);++cases;
    apply(state,2,0xC);
    assert(evaluateDrawBinPredicate(bins(state),true)==DrawBinDecision::Unknown);++cases;
    apply(state,1,0);
    assert(evaluateDrawBinPredicate(bins(state),true)==DrawBinDecision::Skip && !(state.knownBinWords&8));++cases;
    apply(state,0,UINT32_MAX);
    assert(evaluateDrawBinPredicate(bins(state),true)==DrawBinDecision::Pass);++cases;
    // Same actual continuation packet consumer must receive this retained state.
    using Request=Gta5TileCommandInterrupt;using Tail=Gta5TileSceneContinuation;
    constexpr unsigned child=0x3000,scratch=0x1000,context=0xC0100000;
    const std::array<unsigned,35> request{0x1057C,Request::Callback,context,0x578,4,
        0xC0043C01,0x13,scratch+2,4,UINT32_MAX,0x100,0xC0043C01,0x13,scratch+22,context,UINT32_MAX,0x100,
        0xC0043C01,0x13,scratch+18,Request::Callback,UINT32_MAX,0x100,0xC0005401,4,0x45E,4,
        0xC0043C01,0x13,scratch+2,0,UINT32_MAX,0x100,0x57C,0x0BADF00D};
    std::array<std::uint8_t,Tail::Bytes> tailBytes{};
    const auto put=[&](unsigned i,unsigned v){for(unsigned b=0;b<4;++b)tailBytes[i*4+b]=v>>(24-b*8);};
    put(0,0xC0006000);put(1,UINT32_MAX);put(2,0xC0006000);put(3,0x80000000);put(4,0x5C8);put(5,0x20000);
    for(unsigned i=0;i<request.size();++i)put(i+6,request[i]);
    put(41,0xC0006000);put(42,UINT32_MAX);
    const auto tail=Tail::parse(child,tailBytes);assert(tail);
    auto selected=tail->select(bins(state));assert(selected && !selected->interruptRequired && selected->operationCount==5);++cases;
    apply(state,2,0x80000003);selected=tail->select(bins(state));
    assert(selected && selected->interruptRequired && selected->operationCount==10);++cases;
    std::printf("NATIVE_TILE_BIN_COMMAND_PASS cases=%u canonical_packet=1 unknown_preserved=1 counters_unchanged=1 continuation_connected=1 gpu_execution=0\n",cases);
}
