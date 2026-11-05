#include "xenos_stopped_constant_initialization.h"
#include "xenos_copy_shader_fixture.h"
#include <cassert>

using namespace recomp::gpu;
using namespace recomp::gpu::test;
using Stream = std::vector<std::uint32_t>;
static Stream settings() {
    return {0x2180,0x1000000E,0x2181,0,0x2104,0,0x2200,0,0x2208,4,0x2293,0,0x2280,0x80008,
        0x1DD,0x1000,0x1DC,0x20001,0x578,0x12345678};
}
static void append(Stream& to, const Stream& from) { to.insert(to.end(),from.begin(),from.end()); }
static Stream shaders(bool constant = true) {
    const auto vs = program({alloc(2),exec(2,1,1),alloc(1),exec(3)},
        {constant ? alu(0,0,0,true,true) : alu(),alu(62,8,8,true,true)});
    const auto ps = program({alloc(),exec()},{alu()});
    Stream result;
    unsigned stage = 0;
    for (const auto& data : {vs,ps}) {
        auto words = static_cast<std::uint32_t>(data.size()/4);
        append(result,{0xC0002B00 | ((words+1) << 16),stage++,words});
        for (std::size_t i = 0; i < data.size(); i += 4)
            result.push_back((std::uint32_t{data[i]} << 24) | (std::uint32_t{data[i+1]} << 16) |
                (std::uint32_t{data[i+2]} << 8) | data[i+3]);
    }
    return result;
}
static Stream draw() { return {0xC0003600,0x10081}; }
static Stream event() { return {0xC0025800,3,0x2002,7}; }
static Pm4Snapshot snapshot(const Stream& words) {
    Bytes bytes;
    for (auto w : words) for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(w >> shift);
    Pm4Snapshot result; Pm4Error error;
    assert(decodePm4(bytes.data(),bytes.size(),{},result,error)); return result;
}
static Pm4IndirectGraph flat(const Pm4Snapshot& root) {
    Pm4IndirectGraph graph; graph.totalWords = root.words().size(); graph.totalPackets = root.packets().size(); return graph;
}
int main() {
    Stream words = settings(); append(words,shaders());
    for (unsigned i = 0; i < 24; ++i) append(words,draw());
    append(words,event());
    auto root = snapshot(words);
    StoppedConstantInitialization output; StoppedConstantError error;
    assert(inspectStoppedConstantInitialization(root,flat(root),output,error));
    assert(output.draws.size() == 24 && output.shaderLoads == 2 && output.memory.destinationCount == 2);
    assert(output.draws[0].pair.position[3] == 1 && output.draws[0].pair.parameter[0] == 0);
    const auto reject = [&](const Stream& bad, StoppedConstantFailure failure) {
        const auto input = snapshot(bad);
        assert(!inspectStoppedConstantInitialization(input,flat(input),output,error));
        assert(error.failure == failure && output.draws.size() == 24 && output.memory.destinationCount == 2);
    };
    for (unsigned i = 0; i < 14; i += 2) {
        auto bad = words; bad[i+1] ^= 1; reject(bad,StoppedConstantFailure::DrawState);
        bad = words; bad.erase(bad.begin()+i,bad.begin()+i+2); reject(bad,StoppedConstantFailure::MissingDrawState);
    }
    auto bad = settings(); append(bad,draw()); append(bad,event()); reject(bad,StoppedConstantFailure::MissingShader);
    bad = settings(); append(bad,shaders(false)); append(bad,draw()); append(bad,event()); reject(bad,StoppedConstantFailure::ShaderLinkage);
    bad = settings(); append(bad,shaders()); append(bad,{0xC0003B00,0x300}); append(bad,draw()); append(bad,event());
    reject(bad,StoppedConstantFailure::MissingShader);
    bad = words; bad[17] = 1; reject(bad,StoppedConstantFailure::WordStaging); // Unknown scratch byte order.
    bad = settings(); append(bad,shaders()); append(bad,{0xC0003600,0x20081}); append(bad,event());
    reject(bad,StoppedConstantFailure::DrawLayout);
    bad = words; append(bad,{0xC0007F00,0}); reject(bad,StoppedConstantFailure::MemoryInspection);
    bad = words; append(bad,{0xC0003B00,1}); reject(bad,StoppedConstantFailure::UnsupportedControl);
    bad = words; append(bad,{0xC0012700,0,3}); reject(bad,StoppedConstantFailure::UnsupportedControl);
    bad = settings(); append(bad,shaders()); append(bad,draw()); append(bad,event()); append(bad,draw());
    reject(bad,StoppedConstantFailure::WordStaging); // Mid-draw events cannot use a before/after plan.
    bad = settings(); append(bad,shaders()); append(bad,draw()); append(bad,{0x2104,15}); append(bad,draw()); append(bad,event());
    reject(bad,StoppedConstantFailure::DrawState); // Later invalid draw invalidates the entire output.
    bad = settings(); append(bad,{0xC0002B00,0}); append(bad,draw()); append(bad,event());
    reject(bad,StoppedConstantFailure::ShaderLayout);
    bad = settings(); append(bad,shaders()); bad[25] ^= 1u << 20; append(bad,draw()); append(bad,event());
    reject(bad,StoppedConstantFailure::ShaderProof);

    // A trailing full-state invalidation does not erase already-proved draws.
    // Subsequent draws need new state evidence AND new shader loads. This is a
    // conservative analysis-cache rule, not claimed hardware reset semantics.
    auto trailing = words; append(trailing,{0xC0003B00,0x7FFF});
    const auto trailingRoot = snapshot(trailing);
    assert(inspectStoppedConstantInitialization(trailingRoot,flat(trailingRoot),output,error));
    assert(output.draws.size() == 24 && output.invalidations == 1);
    bad = settings(); append(bad,shaders()); append(bad,{0xC0003B00,0x7FFF}); append(bad,shaders());
    append(bad,draw()); append(bad,event()); reject(bad,StoppedConstantFailure::MissingDrawState);
    bad = settings(); append(bad,shaders()); append(bad,{0xC0003B00,0x7FFF});
    auto drawSettings = settings(); drawSettings.resize(14);
    append(bad,drawSettings); append(bad,draw()); append(bad,event()); reject(bad,StoppedConstantFailure::MissingShader);
    Stream reload = settings(); append(reload,shaders()); append(reload,{0xC0003B00,0x7FFF});
    append(reload,drawSettings); append(reload,shaders());
    for (unsigned i = 0; i < 24; ++i) append(reload,draw());
    append(reload,event()); const auto reloadRoot = snapshot(reload);
    assert(inspectStoppedConstantInitialization(reloadRoot,flat(reloadRoot),output,error));
    assert(output.draws.size() == 24 && output.shaderLoads == 4 && output.invalidations == 1);

    // Interleaved parent / child / return: settings in parent, shaders in one
    // child, draws in another, trailing event back in parent. Child-list order
    // is not draw execution order; resolving the actual IB sites is essential.
    auto parent = settings();
    const auto childShaders = snapshot(shaders());
    Stream childWords = {0xC0022100,0x81,0xFFFFFFFF,0x80010000,0xC0022100,0x82,0xFFFFFFFF,0};
    append(childWords,draw());
    const auto childDraws = snapshot(childWords);
    const auto offset = static_cast<std::uint32_t>(parent.size());
    append(parent,{0xC0013F00,0x3000,static_cast<std::uint32_t>(shaders().size()),
                   0xC0013F00,0x4000,static_cast<std::uint32_t>(childWords.size())});
    append(parent,event()); root = snapshot(parent); auto graph = flat(root);
    // Deliberately reverse siblings in storage; both have parent id 0.
    graph.nodes = {{0,offset+3,0x4000,1,childDraws},{0,offset,0x3000,1,childShaders}};
    graph.totalWords += childDraws.words().size()+childShaders.words().size();
    graph.totalPackets += childDraws.packets().size()+childShaders.packets().size();
    assert(inspectStoppedConstantInitialization(root,graph,output,error));
    assert(output.draws.size() == 1 && output.draws[0].node == 1 && output.shaderLoads == 2);
    assert(output.registers[0x81].known == 0x80010000 && output.registers[0x81].value == 0x80010000);
    assert(output.registers[0x82].known == 0 && output.registers[0x82].value == 0);
    for (unsigned field = 0; field < 5; ++field) {
        auto broken = graph;
        switch (field) {
        case 0: ++broken.totalWords; break;
        case 1: ++broken.nodes[0].parent; break;
        case 2: ++broken.nodes[0].depth; break;
        case 3: ++broken.nodes[0].parentOffsetWords; break;
        case 4: broken.nodes[0].physicalAddress += 4; break;
        }
        assert(!inspectStoppedConstantInitialization(root,broken,output,error));
        assert(output.draws.size() == 1 && error.failure == StoppedConstantFailure::MemoryInspection);
    }
    // All four-bit partial knowledge states and RMW masks preserve exactly the
    // concrete possible low-nibble results, without assigning unknown bits.
    for (unsigned known = 0; known < 16; ++known) for (unsigned v = 0; v < 16; ++v) if (!(v & ~known))
        for (unsigned andMask = 0; andMask < 16; ++andMask) for (unsigned orMask = 0; orMask < 16; ++orMask) {
            StoppedRegisterKnowledge state{v,known}; state.modify(andMask,orMask);
            unsigned allOne = 15, anyOne = 0;
            for (unsigned completion = 0; completion < 16; ++completion) if ((completion & known) == v) {
                const auto result = (completion & andMask) | orMask; allOne &= result; anyOne |= result;
            }
            assert((state.known & 15) == ((~(allOne ^ anyOne)) & 15));
            assert(state.value == allOne);
        }
}
