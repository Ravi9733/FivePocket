#include "xenos_side_effect_free_batch.h"
#include <cassert>
#include <map>
using namespace recomp::gpu;

namespace {
// Builds a resolver over a fixed table of {address -> words}, matching how a
// real indirect buffer would be read from guest memory.
SideEffectFreeIndirectResolver tableResolver(const std::map<uint32_t, std::vector<uint32_t>>& table) {
    return [&table](uint32_t address, uint32_t wordCount, std::vector<uint32_t>& out) {
        const auto it = table.find(address);
        if (it == table.end() || it->second.size() != wordCount) return false;
        out = it->second;
        return true;
    };
}
std::vector<uint32_t> indirectBufferPacket(uint32_t address, uint32_t wordCount) {
    return {0xC0013F00, address, wordCount};
}
}

int main() {
    // Setup child: RB_COLOR_MASK=0, RB_DEPTHCONTROL=0, a 2-payload-word shader
    // load, a cache invalidate, and a 1-word EVENT_WRITE - all individually safe.
    // 0xC0012B00: kind=Command, count_field=1 -> payloadWords=2, opcode=0x2B.
    // 0xC0003B00: kind=Command, count_field=0 -> payloadWords=1, opcode=0x3B.
    // 0xC0004600: kind=Command, count_field=0 -> payloadWords=1, opcode=0x46.
    const std::vector<uint32_t> setup{
        0x2104, 0,              // RegisterSequence: RB_COLOR_MASK = 0
        0x2200, 0,              // RegisterSequence: RB_DEPTHCONTROL = 0
        0xC0012B00, 0, 3,       // IM_LOAD_IMMEDIATE, 2 payload words
        0xC0003B00, 0x7FFF,     // TC_INVALIDATE, 1 payload word
        0xC0004600, 0xF,        // EVENT_WRITE, 1 payload word - register only, safe
    };
    const std::map<uint32_t, std::vector<uint32_t>> table{{0x1000, setup}};

    // Root: follow the setup indirect buffer, then 2 predicated DRAW_INDX
    // (viz_query_condition=0, payloadWords=2: token + a draw-initiator word
    // DRAW_INDX still requires) and 1 unpredicated DRAW_INDX_2.
    // 0xC0012201: kind=Command, count_field=1 -> payloadWords=2, opcode=0x22, predicated (bit0 set).
    // 0xC0003600: kind=Command, count_field=0 -> payloadWords=1, opcode=0x36, not predicated.
    std::vector<uint32_t> root = indirectBufferPacket(0x1000, static_cast<uint32_t>(setup.size()));
    root.insert(root.end(), {0xC0012201, 0, 0});
    root.insert(root.end(), {0xC0012201, 0, 0});
    root.insert(root.end(), {0xC0003600, 0x10081});

    SideEffectFreeBatchResult result; SideEffectFreeBatchError error;
    assert(verifySideEffectFreeCommandBatch(root, tableResolver(table), {}, result, error));
    assert(result.drawsVerified == 3 && result.indirectBuffersFollowed == 1);
    assert(result.totalWords == root.size() + setup.size());

    const auto reject = [&](const std::vector<uint32_t>& bad, SideEffectFreeBatchFailure expected) {
        SideEffectFreeBatchResult r; SideEffectFreeBatchError e;
        assert(!verifySideEffectFreeCommandBatch(bad, tableResolver(table), {}, r, e));
        assert(e.failure == expected);
    };

    // Nonzero RB_COLOR_MASK before a draw must reject the whole batch.
    {
        auto badSetup = setup; badSetup[1] = 1;  // write_red0 set
        std::map<uint32_t, std::vector<uint32_t>> badTable{{0x1000, badSetup}};
        auto bad = indirectBufferPacket(0x1000, static_cast<uint32_t>(badSetup.size()));
        bad.insert(bad.end(), {0xC0012201, 0, 0});
        SideEffectFreeBatchResult r; SideEffectFreeBatchError e;
        assert(!verifySideEffectFreeCommandBatch(bad, tableResolver(badTable), {}, r, e));
        assert(e.failure == SideEffectFreeBatchFailure::UnsafeColorWrite);
    }
    // Nonzero RB_DEPTHCONTROL before a draw must reject the whole batch.
    {
        auto badSetup = setup; badSetup[3] = 4;  // z_write_enable set
        std::map<uint32_t, std::vector<uint32_t>> badTable{{0x1000, badSetup}};
        auto bad = indirectBufferPacket(0x1000, static_cast<uint32_t>(badSetup.size()));
        bad.insert(bad.end(), {0xC0012201, 0, 0});
        SideEffectFreeBatchResult r; SideEffectFreeBatchError e;
        assert(!verifySideEffectFreeCommandBatch(bad, tableResolver(badTable), {}, r, e));
        assert(e.failure == SideEffectFreeBatchFailure::UnsafeDepthWrite);
    }
    // A draw before any RB_COLOR_MASK/RB_DEPTHCONTROL write is unsafe (unknown state).
    reject({0xC0012201, 0, 0}, SideEffectFreeBatchFailure::UnsafeColorWrite);
    // Nonzero viz_query_condition on DRAW_INDX must reject.
    {
        auto bad = indirectBufferPacket(0x1000, static_cast<uint32_t>(setup.size()));
        bad.insert(bad.end(), {0xC0012201, 1, 0});
        reject(bad, SideEffectFreeBatchFailure::UnsafeOcclusionQuery);
    }
    // An EVENT_WRITE with more than 1 payload word (a real memory write) must reject.
    // 0xC0014600: kind=Command, count_field=1 -> payloadWords=2, opcode=0x46.
    {
        std::vector<uint32_t> badSetup{0x2104, 0, 0x2200, 0, 0xC0014600, 0xF, 0x2000};
        std::map<uint32_t, std::vector<uint32_t>> badTable{{0x1000, badSetup}};
        auto bad = indirectBufferPacket(0x1000, static_cast<uint32_t>(badSetup.size()));
        bad.insert(bad.end(), {0xC0012201, 0, 0});
        SideEffectFreeBatchResult r; SideEffectFreeBatchError e;
        assert(!verifySideEffectFreeCommandBatch(bad, tableResolver(badTable), {}, r, e));
        assert(e.failure == SideEffectFreeBatchFailure::UnsupportedOpcode && e.opcode == 0x46);
    }
    // A genuinely unrecognized opcode (e.g. a destination-write command this
    // verifier does not certify) must reject the whole batch, not skip it.
    // 0xC0025800: kind=Command, count_field=2 -> payloadWords=3, opcode=0x58.
    {
        auto bad = indirectBufferPacket(0x1000, static_cast<uint32_t>(setup.size()));
        bad.insert(bad.end(), {0xC0025800, 3, 0x10002, 0x01020304});
        reject(bad, SideEffectFreeBatchFailure::UnsupportedOpcode);
    }
    // An indirect buffer the resolver cannot resolve must reject, not silently skip.
    reject(indirectBufferPacket(0x9999, 4), SideEffectFreeBatchFailure::UnresolvedIndirectBuffer);
    // A malformed indirect buffer descriptor (zero word count) must reject.
    reject(indirectBufferPacket(0x1000, 0), SideEffectFreeBatchFailure::InvalidIndirectBuffer);
    // Recursion depth limit: a self-referencing indirect buffer must not loop forever.
    {
        std::vector<uint32_t> selfRef = indirectBufferPacket(0x2000, 3);
        std::map<uint32_t, std::vector<uint32_t>> loopTable{{0x2000, selfRef}};
        SideEffectFreeBatchResult r; SideEffectFreeBatchError e;
        SideEffectFreeBatchLimits limits; limits.maxIndirectDepth = 4;
        assert(!verifySideEffectFreeCommandBatch(selfRef, tableResolver(loopTable), limits, r, e));
        assert(e.failure == SideEffectFreeBatchFailure::RecursionLimit);
    }
    // Empty input is rejected up front.
    {
        SideEffectFreeBatchResult r; SideEffectFreeBatchError e;
        assert(!verifySideEffectFreeCommandBatch({}, tableResolver(table), {}, r, e));
        assert(e.failure == SideEffectFreeBatchFailure::InvalidInput);
    }
    // Regression for docs/claude/Audit.md's three counterexamples: each was
    // independently reproduced as ACCEPTED by an earlier version of this
    // verifier and must now be REJECTED.
    // 1. SCRATCH_WRITEBACK: SCRATCH_ADDR/SCRATCH_UMSK/SCRATCH_REG0 writes can
    //    perform a real memory store on real hardware (WriteRegister in the
    //    pinned reference); this verifier does not understand that register
    //    range at all, so it must fail closed on the write itself, not wait
    //    to see whether a store would actually occur.
    reject({0x01DD, 0x10000, 0x01DC, 1, 0x0578, 0x12345678},
        SideEffectFreeBatchFailure::UnsupportedOpcode);
    // 2. PERSISTENT_COLOR_MASK: a nonzero RB_COLOR_MASK with no draw at all to
    //    gate must still reject - the register write itself persists past
    //    this batch on real hardware.
    reject({0x2104, 15}, SideEffectFreeBatchFailure::UnsafeColorWrite);
    // 3. MISSING_DRAW_INITIATOR: a DRAW_INDX with only the viz-query-condition
    //    word and no VGT_DRAW_INITIATOR word is a malformed packet the
    //    reference itself rejects (ExecutePacketType3Draw); it must not be
    //    accepted here as a verified draw.
    {
        std::vector<uint32_t> malformed{0x2104, 0, 0x2200, 0, 0xC0002201, 0};
        SideEffectFreeBatchResult r; SideEffectFreeBatchError e;
        assert(!verifySideEffectFreeCommandBatch(malformed, tableResolver(table), {}, r, e));
        assert(e.failure == SideEffectFreeBatchFailure::UnsupportedOpcode && e.opcode == 0x22);
    }
    return 0;
}
