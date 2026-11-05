#include "xenos_stopped_work.h"
#include <cassert>
using namespace recomp::gpu;
Pm4Snapshot make(std::initializer_list<std::uint32_t> words) {
    std::vector<std::uint8_t> bytes;
    for (auto w : words) for (unsigned i = 0; i < 4; ++i) bytes.push_back(std::uint8_t(w >> (24-8*i)));
    Pm4Snapshot out; Pm4Error e; assert(decodePm4(bytes.data(),bytes.size(),{},out,e)); return out;
}
int main() {
    std::vector<Pm4DrawInitiator> out; Pm4Error e;
    assert(inspectPm4DrawInitiators(make({0xC0003600,0x80,0xC0012201,0x100,0xFFFF1BFF}),out,e));
    assert(out.size() == 2 && out[0].indexCount == 0 && out[0].source == 2 && !out[0].conditionalVizQuery);
    assert(out[1].packetOffsetWords == 2 && out[1].indexCount == 65535 && out[1].primitive == 63 &&
        out[1].source == 3 && out[1].majorMode == 3 && out[1].index32 && out[1].notEndOfPacket &&
        out[1].conditionalVizQuery && out[1].predicated);
    for (auto count : {0u,1025u}) {
        assert(!inspectPm4DrawInitiators(make({}),out,e,count)); assert(out.size() == 2);
    }
    assert(!inspectPm4DrawInitiators(make({0xC0002200,0}),out,e));
    assert(e.failure == Pm4Failure::DrawLayout && out.size() == 2);
    assert(!inspectPm4DrawInitiators(make({0xC0003600,0,0xC0003600,0}),out,e,1));
    assert(e.failure == Pm4Failure::DrawLimit && out.size() == 2);
    for (unsigned source = 0; source < 4; ++source) for (unsigned primitive = 0; primitive < 64; ++primitive) {
        assert(inspectPm4DrawInitiators(make({0xC0003600,(3u<<16)|(source<<6)|primitive}),out,e));
        assert(out[0].source == source && out[0].primitive == primitive && out[0].indexCount == 3);
    }
    assert(inspectPm4DrawInitiators(make({0xC0007F00,0,0x80000000}),out,e) && out.empty());
    Pm4IndirectGraph graph;
    graph.nodes.push_back({0,0,0x1000,1,make({0xC0003600,0x30080})});
    StoppedGpuWork work; StoppedGpuWorkError error;
    assert(inspectStoppedGpuWork(make({0x80000000}),graph,work,error));
    assert(work.draws.size() == 1 && work.draws[0].node == 1 && work.draws[0].draw.indexCount == 3);
    graph.nodes[0].snapshot = make({0xC0002200,0});
    assert(!inspectStoppedGpuWork(make({}),graph,work,error));
    assert(error.node == 1 && error.kind == 2 && work.draws.size() == 1);
    graph.nodes[0].snapshot = make({0xC0002B00,0});
    assert(!inspectStoppedGpuWork(make({}),graph,work,error));
    assert(error.node == 1 && error.kind == 1 && work.draws.size() == 1);
}
