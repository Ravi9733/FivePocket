#include "guest_import_registry.h"
#include "xenos_command_stream.h"
#include "guest_me_init.h"
#include "guest_stopped_gpu_submission.h"
#include "xenos_dma_triangle_draw.h"
#include <cstdio>
#include <algorithm>
#include <cassert>
#include <memory>

using namespace recomp::bootstrap;
namespace {
class FixtureBackend final : public GuestGraphicsBackend {
public:
    bool initialize() override { active = true; return true; }
    bool ready() const noexcept override { return active; }
    bool active = false;
};
}
int main() {
    GuestAddressSpace space; assert(space.reserve());
    GuestImportRegistry registry;
    auto dispatch = [&](const char* name, std::initializer_list<std::uint32_t> args, bool expected, bool returns = false) {
        GuestImportCall c; std::copy(args.begin(), args.end(), c.arguments.begin());
        c.returnValue = 0xABCDEF0123456789;
        assert(registry.dispatch(name, c).handled() == expected);
        if (!returns) assert(c.returnValue == 0xABCDEF0123456789);
        return static_cast<std::uint32_t>(c.returnValue);
    };
    constexpr auto Init = "__imp__VdInitializeRingBuffer";
    constexpr auto Wb = "__imp__VdEnableRingBufferRPtrWriteBack";
    constexpr auto Id = "__imp__VdSetSystemCommandBufferGpuIdentifierAddress";
    constexpr auto Alloc = "__imp__MmAllocatePhysicalMemoryEx";
    constexpr auto Free = "__imp__MmFreePhysicalMemory";
    for (auto origin : {0U, 0x20000000U, 0xFFF00000U}) {
        assert(registry.initialize(space, 0x10000000, 0x10000));
        auto backend = std::make_shared<FixtureBackend>();
        assert(registry.bindGraphicsBackend(backend, 0x4000, 0x1000, [](auto a) { return a == 0x4000; }));
        dispatch(Init, {origin, 13}, false); dispatch(Wb, {origin + 4, 6}, false);
        assert(!registry.stoppedGraphicsWordIsWritable(origin));
        dispatch("__imp__VdInitializeEngines", {0x53020000, 0x4000, 0, 0x4100, 0x4200}, true, true);
        dispatch(Init, {origin, 13}, false); // No physical namespace yet.
        assert(registry.bindPhysicalMemoryArena(0xA0000000, 0x100000, origin));
        dispatch(Init, {origin, 13}, false); // Reserved arena is not an allocation.
        auto allocate = [&](std::uint32_t bytes, bool writable = true) {
            auto address = dispatch(Alloc, {1, bytes, writable ? 0x404U : 0x402U, 0, 0xFFFFFFFF, 0}, true, true);
            assert(address); return address;
        };
        auto phys = [&](std::uint32_t address) { return origin + (address - 0xA0000000); };
        const auto owner = allocate(0x20000), writeBack = allocate(4), readOnly = allocate(4, false);
        const auto ring = owner + 0x10000;
        std::fill_n(space.translate(owner), 0x20000, 0xA5);
        std::fill_n(space.translate(writeBack), 4, 0xA5);
        for (auto bad : {phys(ring) + 1, phys(ring) + 4}) dispatch(Init, {bad, 13}, false);
        dispatch(Init, {phys(ring), 14}, false); // Crosses even the owning allocation.
        dispatch(Init, {phys(owner), 22}, false); dispatch(Init, {phys(owner), 0xFFFFFFFF}, false);
        dispatch(Init, {origin, 13}, false);
        assert(!registry.graphicsRingConfiguration());
        assert(!registry.snapshotStoppedGraphicsRing());
        dispatch(Wb, {phys(writeBack), 6}, false); // Ring first.
        dispatch(Id, {writeBack + 8}, false);
        dispatch(Init, {phys(ring), 13}, true);
        const auto configured = registry.graphicsRingConfiguration(); assert(configured);
        std::uint32_t decodedBytes = 0; assert(recomp::gpu::pm4RingBytes(13, decodedBytes));
        assert(configured->physicalAddress == phys(ring) && configured->virtualAddress == ring);
        assert(configured->bytes == 0x10000 && configured->bytes == decodedBytes && configured->readWord == 0);
        assert(!configured->writeBack);
        auto snapshot = registry.snapshotStoppedGraphicsRing(); assert(snapshot && snapshot->size() == configured->bytes);
        assert(std::all_of(snapshot->begin(), snapshot->end(), [](auto b) { return b == 0xA5; }));
        (*snapshot)[0] = 0x17; assert(space.translate(ring)[0] == 0xA5); // Independent ownership.
        space.translate(ring)[1] = 0x28; assert((*snapshot)[1] == 0xA5);
        space.translate(ring)[1] = 0xA5;
        auto physicalSnapshot = registry.snapshotStoppedGraphicsPhysical(phys(owner), 4);
        assert(physicalSnapshot && physicalSnapshot->size() == 4 && (*physicalSnapshot)[0] == 0xA5);
        (*physicalSnapshot)[0] = 0; assert(space.translate(owner)[0] == 0xA5);
        assert(registry.snapshotStoppedGraphicsPhysical(phys(readOnly), 4));
        if(origin==0) {
            const std::vector<std::uint8_t> prior(16,0xA5),done(16,0x42),bad(16,0);
            std::array<GuestImportRegistry::SerializedGraphicsUpdate,3> publication{{
                {owner+0x800,&prior,&done},{owner+0x900,&prior,&done},{owner+0xA00,&prior,&done}}};
            publication.back().expected=&bad;
            assert(!registry.publishSerializedGraphicsBatch(publication));
            for(const auto& u:publication)assert(std::equal(prior.begin(),prior.end(),space.translate(u.virtualAddress)));
            publication.back().expected=&prior;publication.back().virtualAddress=readOnly;
            assert(!registry.publishSerializedGraphicsBatch(publication));
            assert(std::equal(prior.begin(),prior.end(),space.translate(owner+0x800)));
            publication.back().virtualAddress=owner+0x800; // Aliased destination.
            assert(!registry.publishSerializedGraphicsBatch(publication));
            publication.back().virtualAddress=owner+0xA00;
            assert(registry.publishSerializedGraphicsBatch(publication));
            for(const auto& u:publication)assert(std::equal(done.begin(),done.end(),space.translate(u.virtualAddress)));
            assert(!registry.publishSerializedGraphicsBatch(publication));
            for(const auto& u:publication)assert(registry.publishSerializedGraphicsData(u.virtualAddress,done,prior));
            std::puts("SERIALIZED_RESOURCE_REGISTRY_BATCH_PASS current_owners=3 stale_last_rejected=1 readonly_rejected=1 aliases_rejected=1");
            // Real ring73 draws include141 uint16 indices:282 bytes, not dword
            // aligned. Exercise the actual allocation reader through the DMA
            // capture rather than a permissive fake reader alone.
            const auto indexAddress=phys(owner)+0x1000;
            const std::array<std::uint32_t,5> words{0xC0032201,0,(141u<<16)|4,indexAddress,0x40000000|141};
            std::array<std::uint8_t,20> encoded{};
            for(unsigned i=0;i<words.size();++i)for(unsigned b=0;b<4;++b)
                encoded[i*4+b]=static_cast<std::uint8_t>(words[i]>>(24-8*b));
            recomp::gpu::Pm4Snapshot draw;recomp::gpu::Pm4Error error;
            assert(recomp::gpu::decodePm4(encoded.data(),encoded.size(),{},draw,error));
            recomp::gpu::StoppedDmaInputs legacy,actual;
            assert(recomp::gpu::captureStoppedDmaInputs(draw,{},
                [&](auto a,auto n){return registry.snapshotStoppedGraphicsPhysical(a,n);},legacy));
            assert(legacy.unavailable==1 && legacy.inputs[0].bytes.empty());
            assert(recomp::gpu::captureStoppedDmaInputs(draw,{},
                [&](auto a,auto n){return registry.copySerializedGraphicsData(a,n);},actual));
            assert(actual.unavailable==0 && actual.inputs.size()==1 && actual.inputs[0].bytes.size()==282);
            assert(std::all_of(actual.inputs[0].bytes.begin(),actual.inputs[0].bytes.end(),[](auto b){return b==0xA5;}));
            assert(!registry.copySerializedGraphicsData(phys(owner)+0x20000-4,6));
            assert(!registry.copySerializedGraphicsData(indexAddress+2,282));
            assert(!registry.copySerializedGraphicsData(0x1ffffffc,6));
            std::puts("DMA_REGISTRY_BYTE_READER_PASS odd_count=141 exact_bytes=282 bounds_preserved=1 guest_writes=0");
        }
        const auto ownership=registry.stoppedGraphicsPhysicalOwnership(phys(owner)+0x100,0x10000);
        assert(bool(ownership)==(origin==0));
        if(ownership){
            assert(ownership->physicalBase==phys(owner) && ownership->virtualBase==owner);
            assert(ownership->byteCount==0x20000 && ownership->protection==0x404 && ownership->writable);
            const auto ro=registry.stoppedGraphicsPhysicalOwnership(phys(readOnly),4);
            assert(ro && !ro->writable && ro->protection==0x402);
        }
        for(auto size:{0u,1u,0x20004u,0x1000004u,0xFFFFFFFFu})
            assert(!registry.stoppedGraphicsPhysicalOwnership(phys(owner),size));
        assert(!registry.stoppedGraphicsPhysicalOwnership(phys(owner)+1,4));
        assert(!registry.stoppedGraphicsPhysicalOwnership(0x1FFFFFFC,8));
        assert(registry.stoppedGraphicsWordIsWritable(phys(owner)) == (origin == 0));
        assert(!registry.stoppedGraphicsWordIsWritable(phys(readOnly)));
        assert(!registry.stoppedGraphicsWordIsWritable(phys(owner)+1));
        assert(!registry.stoppedGraphicsWordIsWritable(origin));
        assert(!registry.stoppedGraphicsWordIsWritable(0x1FFFFFFD));
        for (auto bad : {phys(owner) + 1, origin, phys(owner) + 0x20000 - 4})
            assert(!registry.snapshotStoppedGraphicsPhysical(bad, 8));
        for (auto badSize : {0u, 1u, 0x400004u, 0xFFFFFFFFu})
            assert(!registry.snapshotStoppedGraphicsPhysical(phys(owner), badSize));
        dispatch(Init, {phys(owner), 13}, false); // Never discard an active ring.
        dispatch(Wb, {0, 6}, false); dispatch(Wb, {phys(writeBack) + 1, 6}, false);
        dispatch(Wb, {phys(writeBack), 20}, false); dispatch(Wb, {phys(readOnly), 6}, false);
        dispatch(Wb, {phys(ring), 6}, false); // Cannot corrupt commands by writeback.
        dispatch(Wb, {origin, 6}, false);
        assert(!registry.graphicsRingConfiguration()->writeBack);
        dispatch(Wb, {phys(writeBack), 6}, true);
        const auto wb = registry.graphicsRingConfiguration()->writeBack; assert(wb);
        assert(wb->virtualAddress == writeBack && wb->physicalAddress == phys(writeBack) && wb->blockSizeLog2 == 6);
        dispatch(Wb, {phys(writeBack), 6}, false);
        assert(!registry.graphicsSystemIdentifierAddress());
        for (auto invalid : {ring, writeBack, writeBack + 1, readOnly, 0xA0000000U, 0xFFFFFFFFU}) {
            dispatch(Id, {invalid}, false); assert(!registry.graphicsSystemIdentifierAddress());
        }
        const auto identifier = allocate(4);
        std::fill_n(space.translate(identifier), 4, 0xA5);
        dispatch(Id, {identifier}, true); assert(registry.graphicsSystemIdentifierAddress() == identifier);
        assert(registry.stoppedGraphicsWordAliasesIdentifier(phys(identifier)) == (origin == 0));
        assert(!registry.stoppedGraphicsWordAliasesIdentifier(phys(writeBack)));
        dispatch(Id, {writeBack + 8}, false); // No silent reconfiguration.
        dispatch(Free, {1, identifier}, false);
        assert(std::all_of(space.translate(identifier), space.translate(identifier) + 4, [](auto b) { return b == 0xA5; }));
        dispatch(Id, {0}, true); assert(!registry.graphicsSystemIdentifierAddress());
        assert(!registry.stoppedGraphicsWordAliasesIdentifier(phys(identifier)));
        dispatch(Free, {1, identifier}, true); // Clearing releases this allocation's pin.
        dispatch(Id, {writeBack + 8}, true); assert(registry.graphicsSystemIdentifierAddress() == writeBack + 8);
        constexpr std::uint32_t Stack = 0x12000, Packet = Stack + 80;
        std::array<std::uint32_t, 19> words{};
        words[0] = 0xC0114800; words[1] = 0x3FF;
        for (unsigned i = 0; i < 8; ++i) words[4 + i] = i * 0x80;
        words[12] = 0x10800; words[13] = 7;
        assert(matchesNativeMeInitProfile(words));
        auto publish = [&] {
            auto* bytes = space.translate(Packet);
            for (unsigned i = 0; i < words.size(); ++i) for (unsigned b = 0; b < 4; ++b)
                bytes[i * 4 + b] = static_cast<std::uint8_t>(words[i] >> (24 - b * 8));
        };
        auto verify = [&](std::uint32_t stack, std::uint32_t pointer, std::uint32_t count, bool expected) {
            GuestImportCall c; c.stackPointer = stack; c.arguments[0] = pointer; c.arguments[1] = count;
            c.returnValue = 0x1234567890ABCDEF;
            assert(registry.dispatch("__imp__KiApcNormalRoutineNop", c).handled() == expected);
            assert(c.returnValue == 0x1234567890ABCDEF);
        };
        for (unsigned i = 0; i < words.size(); ++i) {
            words[i] ^= 1; assert(!matchesNativeMeInitProfile(words)); publish();
            verify(Stack, Packet, 19, false); assert(!registry.graphicsMeInitVerified());
            words[i] ^= 1;
        }
        publish();
        verify(Stack, Packet, 18, false); verify(Stack, Packet + 4, 19, false);
        verify(0, Packet, 19, false); verify(0xFFFFFFF0, 0x40, 19, false);
        std::array<std::uint8_t, 76> original{}; std::copy_n(space.translate(Packet), original.size(), original.begin());
        verify(Stack, Packet, 19, true); assert(registry.graphicsMeInitVerified());
        assert(std::equal(original.begin(), original.end(), space.translate(Packet)));
        std::copy(original.begin(), original.end(), space.translate(ring));
        GuestScalarAccess access{0x7FC80714, 4, 19, true, GuestScalarRegion::GpuRegisters};
        auto inspection = inspectStoppedGpuSubmission(access, registry);
        assert(inspection.status == StoppedGpuSubmissionStatus::Decoded);
        assert(inspection.snapshot.packets().size() == 1 && inspection.snapshot.words().size() == 19);
        assert(inspection.snapshot.packets()[0].opcode == 0x48);
        assert(registry.graphicsRingConfiguration()->readWord == 0);
        assert(std::equal(original.begin(), original.end(), space.translate(ring)));
        access.value = 18;
        inspection = inspectStoppedGpuSubmission(access, registry);
        assert(inspection.status == StoppedGpuSubmissionStatus::DecodeRejected);
        assert(inspection.error.failure == recomp::gpu::Pm4Failure::TruncatedPacket && inspection.snapshot.words().empty());
        access.value = 0;
        inspection = inspectStoppedGpuSubmission(access, registry);
        assert(inspection.status == StoppedGpuSubmissionStatus::Decoded && inspection.snapshot.words().empty());
        for (const auto value : {std::uint64_t{0x4000}, std::uint64_t{0x100000013}}) {
            access.value = value;
            assert(inspectStoppedGpuSubmission(access, registry).status == StoppedGpuSubmissionStatus::InvalidWritePointer);
        }
        access.value = 19; access.write = false;
        assert(inspectStoppedGpuSubmission(access, registry).status == StoppedGpuSubmissionStatus::NotSubmission);
        access.write = true; access.width = 8;
        assert(inspectStoppedGpuSubmission(access, registry).status == StoppedGpuSubmissionStatus::NotSubmission);
        access.width = 4; access.address = 0x7FC8112C;
        assert(inspectStoppedGpuSubmission(access, registry).status == StoppedGpuSubmissionStatus::NotSubmission);
        std::fill_n(space.translate(ring), original.size(), 0xA5);
        verify(Stack, Packet, 19, false);
        assert(std::all_of(space.translate(owner), space.translate(owner) + 0x20000, [](auto b) { return b == 0xA5; }));
        assert(std::all_of(space.translate(writeBack), space.translate(writeBack) + 4, [](auto b) { return b == 0xA5; }));
        dispatch(Free, {1, owner}, false); dispatch(Free, {1, writeBack}, false);
        dispatch(Free, {1, readOnly}, true);
        assert(!registry.stoppedGraphicsPhysicalOwnership(phys(readOnly),4));
        assert(space.translate(owner)[0] == 0xA5 && space.translate(writeBack)[0] == 0xA5);
        backend->active = false; dispatch(Init, {phys(ring), 13}, false); dispatch(Wb, {phys(writeBack), 6}, false);
        assert(!registry.snapshotStoppedGraphicsRing());
        registry.reset(); assert(!registry.graphicsRingConfiguration());
        assert(!registry.stoppedGraphicsPhysicalOwnership(phys(owner),4));
        assert(!registry.snapshotStoppedGraphicsPhysical(phys(owner), 4));
        assert(!registry.snapshotStoppedGraphicsRing());
        assert(!registry.graphicsSystemIdentifierAddress());
        assert(!registry.graphicsMeInitVerified());
    }
    // Physical address zero is a valid allocated ring address, not a null host pointer.
    assert(registry.initialize(space, 0x10000000, 0x10000));
    auto backend = std::make_shared<FixtureBackend>();
    assert(registry.bindGraphicsBackend(backend, 0x4000, 0x1000, [](auto a) { return a == 0x4000; }));
    dispatch("__imp__VdInitializeEngines", {0x53020000, 0x4000, 0, 0x4100, 0x4200}, true, true);
    assert(registry.bindPhysicalMemoryArena(0xA0000000, 0x10000, 0));
    assert(dispatch(Alloc, {1, 0x10000, 4, 0, 0xFFFFFFFF, 0}, true, true) == 0xA0000000);
    dispatch(Init, {0, 13}, true);
    assert(registry.graphicsRingConfiguration()->virtualAddress == 0xA0000000);
    registry.unbindPhysicalMemoryArena(); assert(!registry.graphicsRingConfiguration());
    assert(!registry.snapshotStoppedGraphicsRing());
    dispatch(Init, {0, 13}, false);
    registry.reset();
}
