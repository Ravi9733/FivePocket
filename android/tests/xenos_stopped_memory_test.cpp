#include "xenos_stopped_memory.h"
#include <cassert>
using namespace recomp::gpu;
Pm4Snapshot make(std::vector<std::uint32_t> words) {
    std::vector<std::uint8_t> bytes;
    for (auto w : words) for (unsigned i = 0; i < 4; ++i) bytes.push_back(std::uint8_t(w >> (24-8*i)));
    Pm4Snapshot out; Pm4Error error;
    assert(decodePm4(bytes.data(),bytes.size(),{},out,error)); return out;
}
Pm4IndirectGraph flat(const Pm4Snapshot& root) {
    Pm4IndirectGraph out; out.totalWords = root.words().size(); out.totalPackets = root.packets().size(); return out;
}
int main() {
    for (unsigned mask = 0; mask < 256; ++mask) {
        assert(scratchWritebackBigEndianProfile(0x20000 | mask));
        assert(!scratchWritebackBigEndianProfile(mask));
        assert(!scratchWritebackBigEndianProfile(0x10000 | mask));
        assert(!scratchWritebackBigEndianProfile(0x30000 | mask));
    }
    for (unsigned bit = 8; bit < 32; ++bit) {
        if (bit != 17) assert(!scratchWritebackBigEndianProfile(0x200FF | (1u << bit)));
    }
    StoppedMemoryEffects out; StoppedMemoryError error;
    const auto root = make({0x1DD,0x1000,0x1DC,0x20037,0x70578,1,2,3,4,5,6,7,8,
                           0xC0003600,0x10081,0xC0025800,3,0x2002,9});
    assert(inspectStoppedMemoryEffects(root,flat(root),out,error));
    assert(out.writes.size() == 6 && out.unmodeledScratchMaskBits == 0 && out.drawRequests == 1);
    const unsigned slots[] = {0,1,2,4,5};
    for (unsigned i = 0; i < 5; ++i) {
        assert(out.writes[i].physicalAddress == 0x1000 + slots[i]*4 && out.writes[i].value == slots[i]+1);
        assert(out.writes[i].kind == 0 && out.writes[i].priorDrawRequests == 0);
        assert(out.writes[i].bigEndianProfile);
    }
    assert(out.writes[5].kind == 1 && out.writes[5].physicalAddress == 0x2000 &&
        out.writes[5].value == 9 && out.writes[5].priorDrawRequests == 1 && out.writes[5].bigEndianProfile);
    for (auto words : std::vector<std::vector<std::uint32_t>>{
        {0x578,0}, {0x1DC,1,0x578,0}, {0x1DD,1}, {0x1DD,0x20000000},
        {0xC0025800,0x80000003,0x2002,0}, {0xC0025800,3,0x2001,0},
        {0xC0025801,3,0x2002,0}, {0xC0025800,3,0x20000002,0},
        {0xC0015800,3,0}, {0xC0022100,0x1DC,0,0}, {0xC0007F00,0}}) {
        const auto bad = make(words);
        assert(!inspectStoppedMemoryEffects(bad,flat(bad),out,error));
        assert(out.writes.size() == 6 && error.failure != StoppedMemoryFailure::None);
    }
    auto noWrite = make({0x1DC,0,0x578,0});
    assert(inspectStoppedMemoryEffects(noWrite,flat(noWrite),out,error) && out.writes.empty());
    auto edge = make({0xC0025800,3,0x1FFFFFFE,7});
    assert(inspectStoppedMemoryEffects(edge,flat(edge),out,error) && out.writes[0].physicalAddress == 0x1FFFFFFC);
    auto repeat = make({0x1DD,0,0x1DC,1,0x18578,1,2});
    assert(inspectStoppedMemoryEffects(repeat,flat(repeat),out,error));
    assert(out.writes.size() == 2 && out.writes[0].physicalAddress == 0 && out.writes[1].value == 2);
    assert(!out.writes[0].bigEndianProfile && !out.writes[1].bigEndianProfile);
    std::vector<std::uint32_t> excessive;
    for (unsigned i = 0; i < 65; ++i) excessive.insert(excessive.end(),{0xC0025800,3,0x1002,i});
    const auto limit = make(excessive);
    assert(!inspectStoppedMemoryEffects(limit,flat(limit),out,error) && error.failure == StoppedMemoryFailure::Limit);
    assert(out.writes.size() == 2);

    const auto parent = make({0x1DD,0x1000,0x1DC,1,0xC0013F00,0x3000,4,0xC0025800,3,0x2002,7});
    Pm4IndirectGraph graph = flat(parent);
    graph.nodes.push_back({0,4,0x3000,1,make({0x578,9,0xC0003600,0x10081})});
    graph.totalWords += 4; graph.totalPackets += 2;
    assert(inspectStoppedMemoryEffects(parent,graph,out,error));
    assert(out.writes.size() == 2 && out.writes[0].node == 1 && out.writes[1].node == 0 &&
        out.writes[1].priorDrawRequests == 1);
    for (unsigned field = 0; field < 5; ++field) {
        auto broken = graph;
        switch (field) {
        case 0: broken.nodes[0].physicalAddress += 4; break;
        case 1: broken.nodes[0].parent = 1; break;
        case 2: broken.nodes[0].parentOffsetWords = 0; break;
        case 3: broken.nodes[0].depth = 2; break;
        case 4: ++broken.totalWords; break;
        }
        assert(!inspectStoppedMemoryEffects(parent,broken,out,error)); assert(out.writes.size() == 2);
    }
    const auto changedMask = make({0x1DD,0x1000,0x1DC,0x20001,0x578,1,
        0x1DC,0x30001,0x578,2,0x1DC,0x20001,0x578,3});
    assert(inspectStoppedMemoryEffects(changedMask,flat(changedMask),out,error));
    assert(out.writes.size() == 3 && out.unmodeledScratchMaskBits == 0x30000);
    assert(out.writes[0].bigEndianProfile && !out.writes[1].bigEndianProfile && out.writes[2].bigEndianProfile);
}
