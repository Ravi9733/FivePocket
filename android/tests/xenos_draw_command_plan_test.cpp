#include "xenos_draw_command_plan_fixture.h"
#include <cassert>
#include <cstdio>
#include <stdexcept>

using namespace recomp::gpu;
using Words = std::vector<std::uint32_t>;
using Bytes = std::vector<std::uint8_t>;
static void append(Words& to, const Words& from) { to.insert(to.end(),from.begin(),from.end()); }
static Words command(unsigned op, Words payload, bool predicated = false) {
    assert(!payload.empty() && payload.size() <= 16384);
    Words out{0xC0000000 | (static_cast<unsigned>(payload.size()-1)<<16) | (op<<8) | unsigned(predicated)};
    append(out,payload); return out;
}
static Bytes encode(const Words& words) {
    Bytes bytes;
    for (auto w : words) for (unsigned lane = 0; lane < 4; ++lane) bytes.push_back(w >> (24-lane*8));
    return bytes;
}
static Pm4Snapshot snapshot(const Words& words, bool zeroPadding = false) {
    const auto bytes = encode(words); Pm4Snapshot result; Pm4Error error;
    Pm4Limits limits; limits.zeroWordIsPadding = zeroPadding;
    assert(decodePm4(bytes.data(),bytes.size(),limits,result,error)); return result;
}
static Words vertex() { return command(0x2B,{0,3,0x11223344,0x55667788,0x99AABBCC}); }
static Words draw(bool predicated = true) { return command(0x36,{0x30088},predicated); }
static Words minimal() { auto w = vertex(); append(w,draw()); return w; }
int main() {
    const auto source = encode({0x10203040,0x50607080,0x90A0B0C0});
    unsigned reads = 0, rejects = 0;
    const Pm4PhysicalReader reader = [&](auto address, auto size) -> std::optional<Bytes> {
        ++reads;
        if (address == 0x1000 && size == 12) return source;
        if (address == 0x2000 && size == 8) return encode({0x3F800000,0xBF800000});
        return std::nullopt;
    };
    Words words{0x22000,10,20,30,0x40000000 | 0x81 | (0x82<<11),40,50,0x18004,60,70};
    append(words,command(0x21,{0x83,0xFFFFFFF0,5}));
    append(words,command(0x60,{1})); append(words,command(0x62,{1}));
    append(words,vertex()); append(words,command(0x27,{0x1001,3}));
    append(words,command(0x2F,{0x2000,0x20,2})); append(words,draw());
    append(words,command(0x3B,{0x300})); append(words,{0x2001,99});
    append(words,command(0x21,{0x83,0xFFFFFFF7,8})); append(words,command(0x62,{0}));
    append(words,command(0x61,{0})); append(words,command(0x63,{0}));
    append(words,command(0x22,{0,0x40086},true)); append(words,draw(false));
    append(words,{0xA31,0x10000}); append(words,command(0x3C,{3,0xA31,0,0x80000000,8}));
    append(words,command(0x46,{6})); append(words,command(0x58,{3,0x3002,11}));
    append(words,command(0x10,{0xDEADBEEF})); append(words,{0x80000000});
    auto input = snapshot(words); DrawCommandPlan output; DrawCommandError error;
    assert(planDrawCommandCandidates(input,{},reader,output,error));
    assert(error.failure == DrawCommandFailure::None && reads == 2);
    assert(output.actions.size() == input.packets().size());
    assert(output.commands.words() == words && output.draws.size() == 3 && output.waits.size() == 1);
    assert(output.shaders.size() == 2 && output.constants.size() == 1 && output.shaders[1].bigEndianCode == source);
    assert(output.shaders[0].bigEndianCode == encode({0x11223344,0x55667788,0x99AABBCC}));
    const auto& first = output.draws[0]; const auto& second = output.draws[1];
    assert(first.shaders[0] == 0 && first.shaders[1] == 1 && second.shaders == first.shaders);
    assert(first.registers.at(0x2001).bits.equals(20) && second.registers.at(0x2001).bits.equals(99));
    assert(first.registers.at(0x83).bits.value == 5 && first.registers.at(0x83).bits.known == 15);
    assert(second.registers.at(0x83).bits.value == 13 && second.registers.at(0x83).bits.known == 15);
    assert(first.registers.at(0x4020).bits.equals(0x3F800000) && first.registers.at(0x4021).bits.equals(0xBF800000));
    assert(first.registers.at(0x81).bits.equals(40) && first.registers.at(0x82).bits.equals(50));
    assert(first.registers.at(4).bits.equals(70)); // Repeat type-0, not base+i.
    assert(first.registers.count(5) == 0 && first.registers.count(0xA31) == 0);
    assert(evaluateDrawBinPredicate(first.predicate.state,first.predicate.predicated) == DrawBinDecision::Pass);
    assert(evaluateDrawBinPredicate(second.predicate.state,second.predicate.predicated) == DrawBinDecision::Skip);
    assert(evaluateDrawBinPredicate(output.draws[2].predicate.state,false) == DrawBinDecision::Pass);
    assert(output.waits[0].priorDraws == 3 && output.waits[0].registers.at(0xA31).bits.equals(0x10000));
    assert(output.finalRegisters.at(0xA31).bits.equals(0x10000)); // WAIT never fakes a busy-bit transition.
    for (const auto& a : output.actions) if (a.kind == DrawCommandKind::EventWord) assert(a.priorDraws == 3);
    const auto saved = recomp::gpu::test::encodeDrawCommandPlan(output);
    input = {}; // All snapshots, code, constants and register maps are owned.
    assert(recomp::gpu::test::encodeDrawCommandPlan(output) == saved);
    const auto reject = [&](const Words& bad, DrawCommandFailure expected, const DrawBinState& bins = DrawBinState{}) {
        const auto priorReads = reads;
        assert(!planDrawCommandCandidates(snapshot(bad),bins,reader,output,error));
        assert(error.failure == expected && recomp::gpu::test::encodeDrawCommandPlan(output) == saved);
        if (expected != DrawCommandFailure::MissingSource) assert(reads == priorReads);
        ++rejects;
    };
    reject({},DrawCommandFailure::Empty);
    reject(vertex(),DrawCommandFailure::Empty);
    reject(draw(),DrawCommandFailure::MissingVertex);
    reject(minimal(),DrawCommandFailure::InitialBins,{{{1,0,0,0}},0});
    reject(minimal(),DrawCommandFailure::InitialBins,{{},16});
    for (unsigned bit = 1; bit < 16; ++bit) if (bit != 8 && bit != 9 && bit != 10 && bit != 11 && bit != 12 && bit != 13 && bit != 14) {
        auto bad = minimal(); bad[0] |= 1u<<bit; reject(bad,DrawCommandFailure::Header);
    }
    auto bad = minimal(); append(bad,{0x80000001}); reject(bad,DrawCommandFailure::Header);
    bad = minimal(); append(bad,{0x40400000,0,0}); reject(bad,DrawCommandFailure::Header);
    bad = minimal(); append(bad,{0x17FFF,1,2}); reject(bad,DrawCommandFailure::RegisterRange);
    for (auto op : {0x10u,0x21u,0x27u,0x2Bu,0x2Fu,0x3Bu,0x3Cu,0x46u,0x58u,0x60u,0x61u,0x62u,0x63u}) {
        bad = minimal(); append(bad,command(op,{0},true)); reject(bad,DrawCommandFailure::PredicatedState);
    }
    // Unknown controls, including indirect branches, never disappear from a successful plan.
    for (unsigned op = 0; op < 128; ++op) {
        if (op==0x10 || op==0x21 || op==0x22 || op==0x27 || op==0x2B || op==0x2F || op==0x36 ||
            op==0x3B || op==0x3C || op==0x46 || op==0x58 || (op>=0x60 && op<=0x63)) continue;
        bad = words; append(bad,command(op,{0})); reject(bad,DrawCommandFailure::UnsupportedCommand);
    }
    for (auto payload : {Words{0},Words{2,3,1,2,3},Words{0,0},Words{0,1,1},Words{0,6,1,2,3},Words{0,0x10003,1,2,3}}) {
        bad = minimal(); append(bad,command(0x2B,payload)); reject(bad,DrawCommandFailure::ShaderLayout);
    }
    for (auto payload : {Words{0x20000000,3},Words{0x1FFFFFFC,3},Words{0x1002,3},Words{0x1000,3,0},Words{0x1000,0},Words{0x1000,1026}}) {
        bad = minimal(); append(bad,command(0x27,payload)); reject(bad,DrawCommandFailure::ShaderLayout);
    }
    for (auto payload : {Words{0x2000,0},Words{0x2001,0,1},Words{0x2000,0x800,1},Words{0x2000,0,0},
                         Words{0x2000,0,2049},Words{0x2000,2047,2},Words{0x1FFFFFFC,0,2}}) {
        bad = minimal(); append(bad,command(0x2F,payload)); reject(bad,DrawCommandFailure::ConstantLayout);
    }
    for (auto control : {0u,0x88u,0x30008u,0x30188u,0x30888u,0x31088u,0x10010088u}) {
        bad = vertex(); append(bad,command(0x36,{control})); reject(bad,DrawCommandFailure::DrawLayout);
    }
    bad = vertex(); append(bad,command(0x22,{1,0x40086})); reject(bad,DrawCommandFailure::DrawLayout);
    bad = vertex(); append(bad,command(0x22,{0x40086})); reject(bad,DrawCommandFailure::DrawLayout);
    bad = vertex(); append(bad,command(0x36,{0,0x30088})); reject(bad,DrawCommandFailure::DrawLayout);
    for (auto op : {0x3Bu,0x3Cu,0x46u,0x58u,0x60u}) {
        bad = minimal(); append(bad,command(op,{0,0})); reject(bad,DrawCommandFailure::UnsupportedCommand);
    }
    bad = minimal(); append(bad,command(0x21,{0x2000,0,0})); reject(bad,DrawCommandFailure::UnsupportedCommand);
    bad = minimal(); append(bad,command(0x21,{0})); reject(bad,DrawCommandFailure::UnsupportedCommand);
    bad = minimal(); append(bad,command(0x27,{0x3000,3})); reject(bad,DrawCommandFailure::MissingSource);
    // Every reader failure is reported at the exact source packet; no partial plan escapes.
    auto needsRead = command(0x27,{0x1000,3}); append(needsRead,draw());
    for (unsigned mode = 0; mode < 5; ++mode) {
        const Pm4PhysicalReader invalid = [mode](auto,auto) -> std::optional<Bytes> {
            if (mode == 0) return std::nullopt;
            if (mode == 1) return Bytes(11);
            if (mode == 2) return Bytes(13);
            if (mode == 3) throw std::runtime_error("synthetic reader failure");
            throw std::bad_alloc();
        };
        assert(!planDrawCommandCandidates(snapshot(needsRead),{},invalid,output,error));
        assert(error.offsetWords == 0 && error.failure == (mode == 0 ? DrawCommandFailure::MissingSource :
            (mode < 3 ? DrawCommandFailure::SourceSize : (mode == 3 ? DrawCommandFailure::ReaderException : DrawCommandFailure::AllocationFailure))));
        assert(recomp::gpu::test::encodeDrawCommandPlan(output) == saved); ++rejects;
    }
    assert(!planDrawCommandCandidates(snapshot(needsRead),{},{},output,error));
    assert(error.failure == DrawCommandFailure::MissingSource); ++rejects;
    auto padding = minimal(); append(padding,{0});
    assert(!planDrawCommandCandidates(snapshot(padding,true),{},reader,output,error));
    assert(error.failure == DrawCommandFailure::Header); ++rejects;
    // Explicit local budgets, independent of old cold-initialization limits.
    bad = minimal(); bad.resize(4097,0x80000000); reject(bad,DrawCommandFailure::Limit);
    bad = minimal(); bad.resize(520,0x80000000); reject(bad,DrawCommandFailure::Limit);
    bad = vertex(); for (unsigned i = 0; i < 65; ++i) append(bad,draw()); reject(bad,DrawCommandFailure::Limit);
    bad = minimal(); for (unsigned i = 0; i < 65; ++i) append(bad,command(0x3C,{3,0xA31,0,0x80000000,8}));
    reject(bad,DrawCommandFailure::Limit);
    bad = minimal(); for (unsigned i = 0; i < 8; ++i) append(bad,vertex()); reject(bad,DrawCommandFailure::Limit);
    bad = minimal(); for (unsigned i = 0; i < 5; ++i) append(bad,command(0x27,{0x1000,255}));
    reject(bad,DrawCommandFailure::Limit);
    bad = minimal(); for (unsigned i = 0; i < 33; ++i) append(bad,command(0x2F,{0x2000,0,1}));
    reject(bad,DrawCommandFailure::Limit);
    bad = minimal(); append(bad,command(0x2F,{0x2000,0,2048})); append(bad,command(0x2F,{0x2000,0,1}));
    reject(bad,DrawCommandFailure::Limit);
    bad = minimal(); append(bad,command(0x2F,{0x2000,0,2048}));
    append(bad,{(2048u<<16)|0x1000}); bad.resize(bad.size()+2049,0);
    reject(bad,DrawCommandFailure::Limit);
    // Boundary: exactly 4096 distinct registers, no fabricated defaults beyond them.
    auto large = vertex(); append(large,command(0x2F,{0x2000,0,2048}));
    append(large,{(2047u<<16)|0x1000}); large.resize(large.size()+2048,0); append(large,draw());
    const Pm4PhysicalReader zero = [](auto address,auto size) -> std::optional<Bytes> {
        assert(address == 0x2000 && size == 8192); return Bytes(size);
    };
    DrawCommandPlan boundary;
    assert(planDrawCommandCandidates(snapshot(large),{},zero,boundary,error));
    assert(boundary.finalRegisters.size() == 4096 && boundary.draws[0].registers.size() == 4096);
    assert(evaluateDrawBinPredicate(boundary.draws[0].predicate.state,true) == DrawBinDecision::Unknown);
    assert(recomp::gpu::test::encodeDrawCommandPlan(output) == saved);
    std::printf("DrawCommandPlan: %u rejection controls; owned ordered state, sources and 4096-register boundary PASS\n",rejects);
}
