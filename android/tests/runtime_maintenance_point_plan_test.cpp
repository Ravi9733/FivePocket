#include "xenos_maintenance_point_fixture.h"
#include <cstdio>
using namespace recomp::gpu;
using namespace recomp::gpu::test;
using namespace recomp::gpu::test::maintenance;

struct Fixture {
    Stream setup,gpr,points,main;
    Fixture() {
        setup={0xC0003B00,0x300};append(setup,settings());append(setup,shaders());
        assert(setup.size()<74);setup.resize(74,0x80000000);
        gpr={0x5C8,0x20000,0x78D00,0x40401,0x40401,0x40401,0x40401,
             0x40401,0x40401,0x40401,0x40401,0xD00,0x40400};
        for(unsigned i=0;i<24;++i)append(points,draw());
        main={0x22080,0,0,0x01800500,0xC0013F00,0x20000,74,
            0xC0013F00,0x30000,13,0xC0013F00,0x40000,48};append(main,idle());
        // Opaque, unused later programs are retained. They are never claimed
        // to execute safely; a draw using them must be separately proved.
        for(unsigned i=0;i<97;++i) {
            append(main,{0xC0003B00,0x100});append(main,load(0,Bytes(45*4,0)));
            if(i<67) {
                const unsigned count=i?144:576;append(main,{0xC0005600|(count<<16),0x4100});
                for(unsigned j=0;j<count;++j)main.push_back(i==66 && j==0?0x7FC00001u:(i<<16)|j);
            }
            main.push_back(0x80000000);
        }
        append(main,{0xC0003B00,0x7FFF,0xC0001000,0x77777777});
        append(main,event(0x60004,0xA0005000));append(main,event(0x60000,27));
    }
    std::pair<Pm4Snapshot,Pm4IndirectGraph> graph() const {
        const auto r=snapshot({0xC0013F00,0x10000,static_cast<unsigned>(main.size())});
        Pm4IndirectGraph g;Pm4GraphError error;
        assert(inspectPm4IndirectGraph(r,[&](unsigned address,unsigned size)->std::optional<Bytes>{
            const Stream* w=address==0x10000?&main:address==0x20000?&setup:address==0x30000?&gpr:address==0x40000?&points:nullptr;
            if(!w || size!=w->size()*4)return std::nullopt;
            Bytes b;for(auto v:*w)for(unsigned i=0;i<4;++i)b.push_back(v>>(24-8*i));return b;
        },g,error));return {r,g};
    }
};
int main() {
    const Fixture fixture;const auto [root,graph]=fixture.graph();MaintenancePointPlan plan;MaintenancePointError error;
    assert(graph.totalWords>4096 && planRuntimeMaintenancePoints(root,graph,plan,error));
    assert(!plan.experimentalLeadingIdlePair && plan.runtimeTail && plan.points.size()==24 &&
        plan.shaderLoads==99 && plan.invalidations==99 && plan.events.size()==2 && plan.barriers.size()==101);
    assert(plan.barriers[1].precedingPoints==0 && plan.barriers[2].precedingPoints==24);
    assert(plan.registers.at(0x4100)==0x7FC00001 && plan.registers.at(0x433F)==575 &&
        plan.registers.at(0x5C8)==0x20000 && plan.registers.at(0xE00)==1 && plan.registers.at(0xE40)==1);
    assert(plan.finalPrograms[0]==Bytes(45*4,0) && plan.runtimeTail->completionValue()==27);
    assert(!planMaintenancePoints(root,graph,plan,error,true) && error.failure==MaintenancePointFailure::Limits);
    unsigned negative=0;
    const auto reject=[&](const Fixture& f) {
        auto [r,g]=f.graph();auto sentinel=plan;
        assert(!planRuntimeMaintenancePoints(r,g,sentinel,error));
        assert(sentinel.registers==plan.registers && sentinel.points.size()==plan.points.size());++negative;
    };
    for(unsigned at:{0u,1u,2u,3u,13u,14u,15u,16u,17u,18u}) {
        auto f=fixture;
        // Header changes preserve framing for the underlying graph decoder.
        f.main[at]^=(at==0 || at==13 || at==15 || at==17)?0x100:1;reject(f);
    }
    for(unsigned at:{1u,3u,10u,12u}){auto f=fixture;f.gpr[at]^=1;reject(f);}
    for(unsigned at:{0u,1u,46u,47u}){auto f=fixture;f.points[at]^=1;reject(f);}
    for(unsigned at:{3u,5u,7u,9u,11u,13u,15u}) {
        auto f=fixture;f.setup[at]^=1;reject(f);
    }
    // Find actual suffix packets; mutate selector, size, predicates, shader
    // stage/instruction memory address and event endian/order/value relations.
    const auto stream=snapshot(fixture.main);
    unsigned constant=0,program=0;
    for(const auto&p:stream.packets()) {
        if(p.kind==Pm4Kind::Command && p.opcode==0x56 && !constant)constant=p.offsetWords;
        if(p.kind==Pm4Kind::Command && p.opcode==0x2B && !program)program=p.offsetWords;
    }
    assert(constant && program);
    for(unsigned selector:{0x578u,0x4800u,0x4101u,0xFFFF4100u}){auto f=fixture;f.main[constant+1]=selector;reject(f);}
    {auto f=fixture;f.main[constant]|=1;reject(f);}
    {auto f=fixture;f.main[program+1]=2;reject(f);}
    {auto f=fixture;f.main[program+2]|=0x10000;reject(f);}
    {auto f=fixture;f.main[program]|=1;reject(f);}
    for(unsigned fromEnd:{12u,11u,10u,9u,8u,7u,6u,4u,3u,2u}) {
        auto f=fixture;f.main[f.main.size()-fromEnd]^=1;reject(f);
    }
    for(const Stream& extra:std::vector<Stream>{draw(),event(),{0x578,7},{0xC0001000,0}}) {
        auto f=fixture;f.main.insert(f.main.end()-12,extra.begin(),extra.end());reject(f);
    }
    {auto g=graph;g.nodes[2].parentOffsetWords=4;MaintenancePointPlan p;
        assert(!planRuntimeMaintenancePoints(root,g,p,error));++negative;}
    // Original leading-idle profile remains separate and accepts its old test.
    auto legacy=snapshot(batch());MaintenancePointPlan old;
    assert(planMaintenancePoints(legacy,flat(legacy),old,error,true) && !old.runtimeTail);
    assert(!planRuntimeMaintenancePoints(legacy,flat(legacy),old,error));
    std::printf("RUNTIME_MAINTENANCE_PLAN_PASS points=24 loads=99 constant_packets=67 negatives=%u legacy_preserved=1 GPU=0 gameplay=0\n",negative);
}
