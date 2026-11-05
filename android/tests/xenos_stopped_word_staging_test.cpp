#include "xenos_stopped_word_staging.h"
#include <cassert>
using namespace recomp::gpu;
int main() {
    StoppedMemoryEffects effects;
    effects.drawRequests = 24;
    effects.otherRegisterWrites = 50; // Explicitly not validated by a memory subplan.
    effects.writes = {{1,2,0,0x1000,0x12345678,0,true},
        {1,2,0,0x1008,0x89ABCDEF,0,true},{2,0,1,0x2000,0x10203040,24,true}};
    StoppedWordStaging plan;
    assert(planStoppedWordStaging(effects,plan));
    assert(plan.destinationCount == 3 && plan.updates.count == 3 && plan.updates.bufferBytes == 12);
    assert(plan.destinations[0].physicalAddress == 0x1000 && plan.destinations[1].stagingByteOffset == 4);
    assert(plan.updates.words[0].phase == NativeWordPhase::BeforeGraphics &&
        plan.updates.words[2].phase == NativeWordPhase::AfterGraphics);
    const std::array<std::uint8_t,4> expected{0x89,0xAB,0xCD,0xEF};
    assert(plan.updates.words[1].bigEndianBytes == expected);
    const auto reject = [&](const StoppedMemoryEffects& bad) {
        assert(!planStoppedWordStaging(bad,plan));
        assert(plan.destinationCount == 3 && plan.updates.words[1].bigEndianBytes == expected);
    };
    auto bad = effects; bad.unmodeledScratchMaskBits = 0x10000; reject(bad);
    bad = effects; bad.drawRequests = 0; reject(bad);
    bad = effects; bad.drawRequests = 65; reject(bad);
    bad = effects; bad.writes.clear(); reject(bad);
    bad = effects; bad.writes.resize(65); reject(bad);
    for (auto address : {1u,0x20000000u,0xFFFFFFFCu}) {
        bad = effects; bad.writes[0].physicalAddress = address; reject(bad);
    }
    bad = effects; bad.writes[0].kind = 2; reject(bad);
    bad = effects; bad.writes[0].bigEndianProfile = false; reject(bad);
    bad = effects; bad.writes[1].priorDrawRequests = 1; reject(bad);
    bad = effects; bad.writes[0].priorDrawRequests = 24; reject(bad);
    bad = effects; bad.writes[1].physicalAddress = 0x1000; reject(bad);
    auto repeated = effects; repeated.writes[2].physicalAddress = 0x1000;
    assert(planStoppedWordStaging(repeated,plan) && plan.destinationCount == 2 && plan.updates.count == 3);
    assert(plan.updates.words[0].byteOffset == plan.updates.words[2].byteOffset);
    repeated.writes[0].physicalAddress = 0;
    repeated.writes[2].physicalAddress = 0x1FFFFFFC;
    assert(planStoppedWordStaging(repeated,plan)); // Canonical namespace is not ownership proof.
}
