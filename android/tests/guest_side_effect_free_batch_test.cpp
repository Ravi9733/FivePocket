// Integration test for GuestImportRegistry::submitVerifiedSideEffectFreeBatch.
// Setup sequence deliberately mirrors guest_native_initialization_test.cpp's
// proven path to a submitNativeStateBatch-ready registry (same constants,
// same call order) - submitVerifiedSideEffectFreeBatch shares that exact
// precondition/ring-position contract. Only the backend stub is minimal:
// this function never calls into the backend beyond ready(), unlike
// submitNativeStateBatch.
#include "guest_import_registry.h"
#include "xenos_copy_shader_fixture.h"
#include "xenos_stopped_constant_initialization.h"
#include "xenos_native_initialization.h"
#include "xenos_native_state_batch.h"
#include <cassert>
#include <cstdio>
using namespace recomp::bootstrap;
using namespace recomp::gpu;
using namespace recomp::gpu::test;

namespace {
class Backend final : public GuestGraphicsBackend {
public:
    bool initialize() override { initialized_ = true; return true; }
    bool ready() const noexcept override { return initialized_; }
    bool completeInitialEmptyCacheCoherency(const NativeInitialCoherency&) override { return true; }
    bool completeNativeStateBatch(const std::vector<std::uint32_t>& words, NativeInitializationReadback& output) override {
        NativeStateBatch plan; Pm4Error error;
        if (!planNativeStateBatch(words,plan,error)) return false;
        output.byteCount = plan.updates.bufferBytes;
        for (std::size_t i = 0; i < plan.updates.count; ++i) {
            const auto& w = plan.updates.words[i];
            std::copy(w.bigEndianBytes.begin(),w.bigEndianBytes.end(),output.bytes.begin()+w.byteOffset);
        }
        return true;
    }
    bool completeExperimentalConstantInitialization(const StoppedConstantInitialization& plan,
            NativeInitializationReadback& output) override {
        output.byteCount = plan.memory.updates.bufferBytes;
        output.draws = plan.draws.size();
        for (std::size_t i = 0; i < plan.memory.updates.count; ++i) {
            const auto& w = plan.memory.updates.words[i];
            std::copy(w.bigEndianBytes.begin(),w.bigEndianBytes.end(),output.bytes.begin()+w.byteOffset);
        }
        return true;
    }
private:
    bool initialized_ = false;
};
}

int main() {
    GuestAddressSpace space; assert(space.reserve()); GuestImportRegistry registry;
    auto write = [&](auto address, auto value) {
        for (unsigned b = 0; b < 4; ++b) space.translate(address)[b] = value >> (24-b*8);
    };
    auto read = [&](auto address) {
        const auto* b = space.translate(address);
        return (std::uint32_t{b[0]}<<24) | (std::uint32_t{b[1]}<<16) | (std::uint32_t{b[2]}<<8) | b[3];
    };
    auto dispatch = [&](const char* name, std::initializer_list<std::uint32_t> args, std::uint32_t stack = 0) {
        GuestImportCall c; c.stackPointer = stack; std::copy(args.begin(),args.end(),c.arguments.begin());
        assert(registry.dispatch(name,c).handled()); return static_cast<std::uint32_t>(c.returnValue);
    };
    assert(registry.initialize(space,0x10000000,0x10000));
    auto backend = std::make_shared<Backend>();
    assert(registry.bindGraphicsBackend(backend,0x4000,0x1000,[](auto a){return a == 0x4000;}));
    assert(registry.bindPhysicalMemoryArena(0xA0000000,0x2000000,0));
    assert(registry.bindNativeInitialCoherencySubmission());
    assert(registry.bindExperimentalNativeInitialization());
    assert(registry.bindEmptyGuestCacheCoherency());
    assert(registry.bindNativeStateBatches());
    dispatch("__imp__VdInitializeEngines",{0x53020000,0x4000,0,0x4100,0x4200});
    auto allocate = [&](auto size, auto flags = 4u) { return dispatch("__imp__MmAllocatePhysicalMemoryEx",{1,size,flags,0,0xFFFFFFFF,0}); };
    const auto ring = allocate(0x10000u,4u), wb = allocate(0x1000u,4u), cold = allocate(0x1000u,4u);
    const auto ib = allocate(0x1000u,4u), ib2 = allocate(0x1000u,4u), destination = allocate(0x1000u,4u);
    const auto phys = [](auto a) { return a-0xA0000000; };
    dispatch("__imp__VdInitializeRingBuffer",{phys(ring),13});
    dispatch("__imp__VdEnableRingBufferRPtrWriteBack",{phys(wb),7});
    dispatch("__imp__VdSetSystemCommandBufferGpuIdentifierAddress",{wb+8});
    std::vector<std::uint32_t> root(22);
    root[0] = 0xC0114800; root[1] = 0x3FF;
    for (unsigned i = 0; i < 8; ++i) root[i+4] = i*0x80;
    root[12] = 0x10800; root[13] = 7; root[19] = 0xC0013F00; root[20] = phys(cold); root[21] = 11;
    const std::vector<std::uint32_t> coldWords{0xA31,0x01000000,0x10A2F,0x1000,0x20000,0xC0043C00,3,0xA31,0,0x80000000,8};
    for (unsigned i = 0; i < root.size(); ++i) write(ring+i*4,root[i]);
    for (unsigned i = 0; i < coldWords.size(); ++i) write(cold+i*4,coldWords[i]);
    for (unsigned i = 0; i < 19; ++i) write(0x12050+i*4,root[i]);
    dispatch("__imp__KiApcNormalRoutineNop",{0x12050,19},0x12000);
    assert(registry.submitNativeInitialCoherency(22));
    assert(registry.graphicsRingConfiguration()->readWord==22 && read(wb)==22);

    // submitVerifiedSideEffectFreeBatch requires completedExperimentalInitialization_,
    // which only submitExperimentalNativeInitialization sets (submitNativeInitialCoherency
    // sets a separate field) - replicate that one bounded shader-upload batch,
    // advancing the ring from 22 to 25, exactly as guest_native_initialization_test.cpp does.
    const auto vs = program({alloc(2),exec(2,1,1),alloc(1),exec(3)}, {alu(0,0,0,true,true),alu(62,8,8,true,true)});
    const auto ps = program({alloc(),exec()},{alu()});
    std::vector<std::uint32_t> initBatch{0x2180,0x1000000E,0x2181,0,0x2104,0,0x2200,0,0x2208,4,0x2293,0,0x2280,0x80008,
        0x1DD,phys(destination),0x1DC,0x20001,0x578,0x11223344};
    unsigned stage = 0;
    for (const auto& bytes : {vs,ps}) {
        const auto count = static_cast<std::uint32_t>(bytes.size()/4);
        initBatch.insert(initBatch.end(),{0xC0002B00 | ((count+1)<<16),stage++,count});
        for (std::size_t i = 0; i < bytes.size(); i += 4)
            initBatch.push_back((std::uint32_t{bytes[i]}<<24) | (std::uint32_t{bytes[i+1]}<<16) | (std::uint32_t{bytes[i+2]}<<8) | bytes[i+3]);
    }
    initBatch.insert(initBatch.end(),{0xC0003600,0x10081,0xC0025800,3,phys(destination)+6,0x55667788});
    write(ring+88,0xC0013F00); write(ring+92,phys(ib)); write(ring+96,static_cast<std::uint32_t>(initBatch.size()));
    for (std::size_t i = 0; i < initBatch.size(); ++i) write(ib+i*4,initBatch[i]);
    assert(registry.submitExperimentalNativeInitialization(25));
    assert(registry.graphicsRingConfiguration()->readWord==25 && read(wb)==25);

    // A safe child: RB_COLOR_MASK=0, RB_DEPTHCONTROL=0, a shader load, a cache
    // invalidate, a 1-word EVENT_WRITE, then 2 predicated DRAW_INDX
    // (viz_query_condition=0) and 1 unpredicated DRAW_INDX_2. Same packet
    // shapes/headers verified by xenos_side_effect_free_batch_test.cpp.
    // Padded past submitNativeStateBatch's own 256-word root cap (a trailing
    // NOP carrying 240 payload words) so this exercises the real >256-word
    // regime submitVerifiedSideEffectFreeBatch is gated to: below that cap,
    // the registry requires submitNativeStateBatch to handle the batch
    // instead, since only that path publishes readback via
    // publishNativeGpuOutputs - a batch this small must never be accepted
    // here even if its content happens to verify safe.
    std::vector<std::uint32_t> nested{
        0x2104, 0, 0x2200, 0, 0xC0012B00, 0, 3, 0xC0003B00, 0x7FFF, 0xC0004600, 0xF,
        0xC0012201, 0, 0, 0xC0012201, 0, 0, 0xC0003600, 0x10081,
    };
    const auto smallSize = nested.size();
    nested.push_back(0xC0EF1000);  // NOP, payloadWords=240.
    nested.insert(nested.end(), 240, 0u);
    for (unsigned i = 0; i < nested.size(); ++i) write(ib2+i*4,nested[i]);
    const std::vector<std::uint32_t> batch{0xC0013F00, phys(ib2), static_cast<std::uint32_t>(nested.size())};
    auto publish = [&](const auto& words) {
        for (unsigned i = 0; i < root.size(); ++i) write(ring+i*4,root[i]); // Keep the cold-init root intact.
        write(ring+25*4,words[0]); write(ring+26*4,words[1]); write(ring+27*4,words[2]);
    };
    auto unchanged = [&] {
        assert(registry.graphicsRingConfiguration()->readWord == 25 && read(wb) == 25);
        assert(!registry.completedSideEffectFreeBatches());
    };
    publish(batch);
    for (auto bad : {0u,25u,27u,29u,0x4000u,0xFFFFFFFFu}) { assert(!registry.submitVerifiedSideEffectFreeBatch(bad)); unchanged(); }
    // Reject: unowned/overlapping indirect buffer target.
    for (auto bad : {phys(ring),phys(wb),phys(wb)+8,0x1FFFFFFCu}) {
        auto modified = batch; modified[1] = bad;
        publish(modified); assert(!registry.submitVerifiedSideEffectFreeBatch(28)); unchanged();
    }
    // Reject: nonzero RB_COLOR_MASK inside the nested buffer.
    {
        auto badNested = nested; badNested[1] = 1;
        for (unsigned i = 0; i < badNested.size(); ++i) write(ib2+i*4,badNested[i]);
        publish(batch); assert(!registry.submitVerifiedSideEffectFreeBatch(28)); unchanged();
        for (unsigned i = 0; i < nested.size(); ++i) write(ib2+i*4,nested[i]); // Restore.
    }
    // Reject: nonzero viz_query_condition on a DRAW_INDX.
    {
        auto badNested = nested; badNested[12] = 1;
        for (unsigned i = 0; i < badNested.size(); ++i) write(ib2+i*4,badNested[i]);
        publish(batch); assert(!registry.submitVerifiedSideEffectFreeBatch(28)); unchanged();
        for (unsigned i = 0; i < nested.size(); ++i) write(ib2+i*4,nested[i]); // Restore.
    }
    // Reject: an unrecognized opcode (0x58, destination write) inside the nested buffer.
    {
        auto badNested = nested; badNested.insert(badNested.end(),{0xC0025800,3,0x10002,0x01020304});
        for (unsigned i = 0; i < badNested.size(); ++i) write(ib2+i*4,badNested[i]);
        const std::vector<std::uint32_t> badBatch{0xC0013F00, phys(ib2), static_cast<std::uint32_t>(badNested.size())};
        publish(badBatch);
        assert(!registry.submitVerifiedSideEffectFreeBatch(28)); unchanged();
        for (unsigned i = 0; i < nested.size(); ++i) write(ib2+i*4,nested[i]); // Restore.
    }
    // Reject: an otherwise-safe batch at/below submitNativeStateBatch's own
    // 256-word root cap must still fall through, never be accepted here -
    // only submitNativeStateBatch publishes real readback via
    // publishNativeGpuOutputs for batches this small, and a guest expecting
    // that readback must never have it silently skipped.
    {
        const std::vector<std::uint32_t> smallNested(nested.begin(), nested.begin() + smallSize);
        for (unsigned i = 0; i < smallNested.size(); ++i) write(ib2+i*4,smallNested[i]);
        const std::vector<std::uint32_t> smallBatch{0xC0013F00, phys(ib2), static_cast<std::uint32_t>(smallNested.size())};
        publish(smallBatch);
        assert(!registry.submitVerifiedSideEffectFreeBatch(28)); unchanged();
        for (unsigned i = 0; i < nested.size(); ++i) write(ib2+i*4,nested[i]); // Restore.
    }
    // Accept: the verified-safe batch actually advances the ring, without
    // ever calling into the backend beyond ready() (no completeNativeStateBatch
    // or completeExperimentalIdleCachePair call exists on this path at all).
    publish(batch);
    assert(registry.submitVerifiedSideEffectFreeBatch(28));
    assert(registry.graphicsRingConfiguration()->readWord == 28 && read(wb) == 28);
    const auto completed = registry.completedSideEffectFreeBatches();
    // indirectBuffers counts only NESTED follows beyond the mandatory outer
    // root resolution (which submitVerifiedSideEffectFreeBatch itself performs
    // before ever calling the verifier) - zero here since "nested" has no
    // further indirect buffer of its own.
    assert(completed && completed->readWord == 28 && completed->batches == 1 &&
        completed->draws == 3 && completed->indirectBuffers == 0);
    assert(!registry.submitVerifiedSideEffectFreeBatch(28));
    assert(!registry.submitVerifiedSideEffectFreeBatch(31));

    // submitNativeStateBatch recognizes this completion's ring position too,
    // and vice versa - the shared chain stays consistent regardless of which
    // path actually advanced it.
    std::vector<std::uint32_t> stateWords{0xC0006000,0x12345678};
    write(ib+0,0xC0013F00); write(ib+4,phys(ib)+64); write(ib+8,static_cast<std::uint32_t>(stateWords.size()));
    for (unsigned i = 0; i < stateWords.size(); ++i) write(ib+64+i*4,stateWords[i]);
    write(ring+28*4,read(ib+0)); write(ring+29*4,read(ib+4)); write(ring+30*4,read(ib+8));
    assert(registry.submitNativeStateBatch(31));
    assert(registry.graphicsRingConfiguration()->readWord == 31);
    std::puts("SIDE_EFFECT_FREE_BATCH: verified color/depth/query-safe consumption, opcode/register rejection, shared ring-chain continuity PASS");
    return 0;
}
