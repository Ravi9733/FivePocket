#include "guest_import_registry.h"
#include "xenos_native_state_batch.h"
#include "xenos_maintenance_point_fixture.h"
#include <cstdio>
using namespace recomp::bootstrap;
using namespace recomp::gpu;
using namespace recomp::gpu::test::maintenance;
// Authored lifecycle/publication fixture, NOT a GPU implementation or receipt.
class MaintenanceBackend final:public GuestGraphicsBackend {
public:
    bool active=false,accept=true,corrupt=false,wrongDraws=false,wrongSize=false,skipPublisher=false,throwBefore=false;
    unsigned calls=0,commits=0;
    bool initialize() override {active=true;return true;}
    bool ready() const noexcept override {return active;}
    bool completeInitialEmptyCacheCoherency(const NativeInitialCoherency&) override {return true;}
    bool completeExperimentalConstantInitialization(const StoppedConstantInitialization& p,NativeInitializationReadback& r) override {
        r.byteCount=p.memory.updates.bufferBytes;r.draws=p.draws.size();
        for(std::size_t i=0;i<p.memory.updates.count;++i) {
            const auto& w=p.memory.updates.words[i];std::copy(w.bigEndianBytes.begin(),w.bigEndianBytes.end(),r.bytes.begin()+w.byteOffset);
        }
        return true;
    }
    bool completeNativeStateBatch(const std::vector<std::uint32_t>& words,NativeInitializationReadback& r) override {
        NativeStateBatch p;Pm4Error e;if(!planNativeStateBatch(words,p,e))return false;
        r.byteCount=p.updates.bufferBytes;
        for(std::size_t i=0;i<p.updates.count;++i) {
            const auto& w=p.updates.words[i];std::copy(w.bigEndianBytes.begin(),w.bigEndianBytes.end(),r.bytes.begin()+w.byteOffset);
        }
        return true;
    }
    bool executeExperimentalMaintenancePoints(const Pm4Snapshot& root,const Pm4IndirectGraph& graph,
            const std::function<bool(const NativeInitializationReadback&)>& publish) override {
        ++calls;if(throwBefore)throw 7;if(!accept)return false;if(skipPublisher)return true;
        MaintenancePointPlan p;MaintenancePointError e;assert(planMaintenancePoints(root,graph,p,e,true));
        NativeInitializationReadback r;r.byteCount=p.updates.bufferBytes+unsigned(wrongSize);r.draws=p.points.size()+unsigned(wrongDraws);
        for(std::size_t i=0;i<p.updates.count;++i) {
            const auto& w=p.updates.words[i];std::copy(w.bigEndianBytes.begin(),w.bigEndianBytes.end(),r.bytes.begin()+w.byteOffset);
        }
        if(corrupt)r.bytes[0]^=1;
        if(!publish(r))return false;++commits;return true;
    }
};
int main() {
    GuestAddressSpace space;assert(space.reserve());GuestImportRegistry registry;
    assert(registry.initialize(space,0x10000000,0x10000));
    const auto write=[&](auto address,auto value){for(unsigned b=0;b<4;++b)space.translate(address)[b]=value>>(24-b*8);};
    const auto read=[&](auto address){unsigned value=0;for(unsigned b=0;b<4;++b)value=(value<<8)|space.translate(address)[b];return value;};
    const auto dispatch=[&](const char* name,std::initializer_list<std::uint32_t> args,unsigned stack=0) {
        GuestImportCall c;c.stackPointer=stack;std::copy(args.begin(),args.end(),c.arguments.begin());
        assert(registry.dispatch(name,c).handled());return static_cast<unsigned>(c.returnValue);
    };
    const auto backend=std::make_shared<MaintenanceBackend>();
    assert(registry.bindGraphicsBackend(backend,0x4000,0x1000,[](auto a){return a==0x4000;}));
    assert(registry.bindPhysicalMemoryArena(0xA0000000,0x2000000,0));
    assert(registry.bindNativeInitialCoherencySubmission() && registry.bindExperimentalNativeInitialization());
    assert(registry.bindEmptyGuestCacheCoherency() && registry.bindNativeStateBatches() && registry.bindExperimentalIdleCachePair());
    assert(!registry.submitNativeMaintenancePoints(28));
    dispatch("__imp__VdInitializeEngines",{0x53020000,0x4000,0,0x4100,0x4200});
    const auto allocate=[&](unsigned size,unsigned flags=4u){return dispatch("__imp__MmAllocatePhysicalMemoryEx",{1,size,flags,0,0xFFFFFFFF,0});};
    const auto ring=allocate(0x10000u),wb=allocate(0x1000u),cold=allocate(0x1000u),ib=allocate(0x4000u),ib2=allocate(0x4000u),destination=allocate(0x1000u);
    const auto readOnly=allocate(0x1000u,2u);assert(readOnly);
    const auto phys=[](auto a){return a-0xA0000000;};
    const auto store=[&](unsigned a,const Stream& words){for(unsigned i=0;i<words.size();++i)write(a+i*4,words[i]);};
    dispatch("__imp__VdInitializeRingBuffer",{phys(ring),13});dispatch("__imp__VdEnableRingBufferRPtrWriteBack",{phys(wb),7});
    dispatch("__imp__VdSetSystemCommandBufferGpuIdentifierAddress",{wb+8});
    Stream boot(22);boot[0]=0xC0114800;boot[1]=0x3FF;
    for(unsigned i=0;i<8;++i)boot[i+4]=i*0x80;
    boot[12]=0x10800;boot[13]=7;boot[19]=0xC0013F00;boot[20]=phys(cold);boot[21]=11;
    store(ring,boot);store(cold,{0xA31,0x01000000,0x10A2F,0x1000,0x20000,0xC0043C00,3,0xA31,0,0x80000000,8});
    store(0x12050,Stream(boot.begin(),boot.begin()+19));dispatch("__imp__KiApcNormalRoutineNop",{0x12050,19},0x12000);
    assert(registry.submitNativeInitialCoherency(22));
    auto initial=settings();append(initial,shaders());append(initial,draw());append(initial,event(phys(destination)));
    store(ib,initial);store(ring+88,{0xC0013F00,phys(ib),static_cast<unsigned>(initial.size())});
    assert(registry.submitExperimentalNativeInitialization(25));
    auto words=batch();words[words.size()-6]=phys(destination)|2;words[words.size()-2]=phys(destination+4)|2;
    const Stream nested{0xC0013F00,phys(ib2),static_cast<unsigned>(words.size())};
    store(ib,nested);store(ib2,words);store(ring+100,{0xC0013F00,phys(ib),3});
    write(destination,0xA5A5A5A5);write(destination+4,0xB6B6B6B6);
    unsigned negative=0;
    const auto unchanged=[&] {
        assert(read(wb)==25 && registry.graphicsRingConfiguration()->readWord==25 && !registry.completedNativeStateBatches());
        assert(read(destination)==0xA5A5A5A5 && read(destination+4)==0xB6B6B6B6 && !backend->commits);
    };
    for(auto value:{0u,25u,27u,29u,0xFFFFFFFFu}) {assert(!registry.submitNativeMaintenancePoints(value));unchanged();++negative;}
    assert(!registry.submitNativeStateBatch(28));unchanged(); // Old state-only path stays strict.
    for(auto address:{phys(ring),phys(wb),phys(wb)+8,phys(readOnly),phys(ib2),0x1FFFFFFCu}) {
        auto bad=words;bad[bad.size()-6]=address|2;store(ib2,bad);
        const auto calls=backend->calls;assert(!registry.submitNativeMaintenancePoints(28) && calls==backend->calls);unchanged();++negative;
    }
    store(ib2,words);
    for(auto address:{phys(ring),phys(wb),phys(wb)+8,0x1FFFFFFCu}) {
        write(ib+4,address);const auto calls=backend->calls;
        assert(!registry.submitNativeMaintenancePoints(28) && calls==backend->calls);unchanged();++negative;
    }
    store(ib,nested);
    write(wb,26);assert(!registry.submitNativeMaintenancePoints(28));write(wb,25);unchanged();++negative;
    backend->active=false;assert(!registry.submitNativeMaintenancePoints(28));backend->active=true;unchanged();++negative;
    backend->accept=false;assert(!registry.submitNativeMaintenancePoints(28));backend->accept=true;unchanged();++negative;
    for(bool* flag:{&backend->corrupt,&backend->wrongDraws,&backend->wrongSize,&backend->skipPublisher,&backend->throwBefore}) {
        *flag=true;assert(!registry.submitNativeMaintenancePoints(28));*flag=false;unchanged();++negative;
    }
    assert(registry.submitNativeMaintenancePoints(28));
    assert(read(wb)==28 && read(destination)==7 && read(destination+4)==0x12345678 && backend->commits==1);
    auto state=registry.completedNativeStateBatches();assert(state && state->readWord==28 && state->words==2 &&
        state->maintenanceBatches==1 && state->maintenanceDraws==96 && registry.experimentalIdleCachePairs()==1);
    assert(!registry.submitNativeMaintenancePoints(28));
    // The NEXT ordinary state producer must recognize this real completion,
    // and the next maintenance producer must then recognize the state producer.
    store(ib,event(phys(destination),0xDEADBEEF));store(ring+28*4,{0xC0013F00,phys(ib),4});
    assert(registry.submitNativeStateBatch(31) && read(wb)==31 && read(destination)==0xDEADBEEF);
    store(ib,nested);store(ring+31*4,{0xC0013F00,phys(ib),3});
    assert(registry.submitNativeMaintenancePoints(34) && read(wb)==34 && read(destination)==7 && backend->commits==2);
    state=registry.completedNativeStateBatches();assert(state->batches==3 && state->maintenanceBatches==2 && state->maintenanceDraws==192);
    std::printf("GUEST_MAINTENANCE_POINTS_PASS points=192 negative_cases=%u owned_nested_graph=1 event_readback_audit=1 "
        "state_maintenance_state_chain=1 real_gpu=0\n",negative);
}
