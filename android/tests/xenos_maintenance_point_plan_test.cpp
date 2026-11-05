#include "xenos_maintenance_point_fixture.h"
#include <cstdio>
using namespace recomp::gpu;
using namespace recomp::gpu::test;
using namespace recomp::gpu::test::maintenance;

int main() {
    MaintenancePointPlan output;MaintenancePointError error;unsigned checks=0;
    const auto good=batch();const auto root=snapshot(good);const auto graph=flat(root);
    assert(!planMaintenancePoints(root,graph,output,error)); // Explicit experiment only.
    assert(planMaintenancePoints(root,graph,output,error,true));
    assert(output.points.size()==96 && output.shaderLoads==2 && output.events.size()==2);
    assert(output.barriers.size()==2 && output.barriers[0].precedingPoints==0 && !output.barriers[0].tileFlush);
    assert(output.barriers[1].tileFlush && output.barriers[1].precedingPoints==0);
    assert(output.registers.at(0x21F9)==3 && output.updates.bufferBytes==8 && output.updates.count==2);
    assert((output.updates.words[1].bigEndianBytes==std::array<std::uint8_t,4>{0x12,0x34,0x56,0x78}));
    const auto reject=[&](const Stream& words,MaintenancePointFailure why) {
        const auto r=snapshot(words);MaintenancePointPlan sentinel=output;
        assert(!planMaintenancePoints(r,flat(r),sentinel,error,true));
        if(error.failure!=why)std::fprintf(stderr,"case=%u expected=%u actual=%u at=%u detail=%x\n",
            checks,unsigned(why),unsigned(error.failure),error.offset,error.detail);
        assert(error.failure==why && sentinel.points.size()==output.points.size() &&
            sentinel.events.size()==output.events.size() && sentinel.registers==output.registers);
        ++checks;
    };
    auto bad=good;bad.erase(bad.begin(),bad.begin()+6);reject(bad,MaintenancePointFailure::Incomplete);
    for(unsigned i=0;i<6;++i) {
        bad=good;bad[i]^=i%2?1:0x100;
        reject(bad,MaintenancePointFailure::Incomplete);
    }
    for(unsigned i=0;i<14;i+=2) {
        bad=good;bad.erase(bad.begin()+6+i,bad.begin()+8+i);reject(bad,MaintenancePointFailure::DrawState);
        bad=good;bad[7+i]^=1;reject(bad,MaintenancePointFailure::Register);
    }
    // Even unsupported writes overwritten before a draw must fail at their site.
    for(const auto& write:std::vector<Stream>{{0x1DC,0},{0x1DD,0x1000},{0x578,7},
            {0x4800,0},{0x5C8,0x20001},{0x2104,15},{0x2200,4},{0x2203,0x20},{0xD00,0xFFFFFFFF}}) {
        bad=idle();append(bad,write);append(bad,Stream(good.begin()+6,good.end()));
        reject(bad,MaintenancePointFailure::Register);
    }
    bad=good;bad[good.size()-8]|=2;reject(bad,MaintenancePointFailure::Header);
    bad=good;bad[good.size()-8]|=1;reject(bad,MaintenancePointFailure::Header); // Predicated event.
    for(const auto& packet:std::vector<Stream>{{0xC0002200,0},{0xC0003600,0x20081},
            {0xC0003600,0x110081}}) {
        bad=idle();append(bad,settings());append(bad,shaders());append(bad,packet);append(bad,event());
        reject(bad,packet[0]==0xC0002200?MaintenancePointFailure::Opcode:MaintenancePointFailure::DrawLayout);
    }
    bad=idle();append(bad,settings());append(bad,draw());append(bad,event());reject(bad,MaintenancePointFailure::ShaderProof);
    for(const auto& shader:std::vector<Bytes>{program({alloc(3),exec()},{alu()}),program({alloc(),exec()},{alu(32)}),
            program({alloc(),exec()},{alu(0,15,0,true,false,64)})}) {
        bad=idle();append(bad,settings());append(bad,shaders());append(bad,load(1,shader));
        append(bad,draw());append(bad,event());reject(bad,MaintenancePointFailure::ShaderProof);
    }
    bad=idle();append(bad,settings());append(bad,shaders());append(bad,{0xC0012B00,1,3});
    append(bad,draw());append(bad,event());reject(bad,MaintenancePointFailure::ShaderLayout);
    for(unsigned mask:{0x100u,0x300u,0x7FFFu}) {
        bad=idle();append(bad,settings());append(bad,shaders());append(bad,{0xC0003B00,mask});
        append(bad,draw());append(bad,event());
        reject(bad,mask==0x7FFF?MaintenancePointFailure::DrawState:MaintenancePointFailure::ShaderProof);
    }
    bad=good;bad.resize(bad.size()-8);append(bad,{0xC0003B00,0x7FFF});append(bad,shaders());
    append(bad,draw());append(bad,event());reject(bad,MaintenancePointFailure::DrawState);
    // A program loaded but not drawn must be retained, not executed or rejected
    // by a draw-only shader proof. Any subsequent draw of it must still fail.
    bad=good;bad.resize(bad.size()-8);
    const auto unused=program({alloc(3),exec()},{alu(32)});
    append(bad,load(1,unused));append(bad,event());
    auto r=snapshot(bad);MaintenancePointPlan retained;
    assert(planMaintenancePoints(r,flat(r),retained,error,true) && retained.finalPrograms[1]==unused);
    bad.resize(bad.size()-4);append(bad,draw());append(bad,event());reject(bad,MaintenancePointFailure::ShaderProof);
    // Full invalidation can be followed by an explicitly re-established state.
    bad=idle();append(bad,{0xC0003B00,0x7FFF});append(bad,Stream(good.begin()+6,good.end()));
    r=snapshot(bad);assert(planMaintenancePoints(r,flat(r),retained,error,true) && retained.invalidations==1);
    bad=good;bad.resize(bad.size()-8);append(bad,{0x5C8,0x20000,0xC0003B00,0x100});append(bad,event());
    r=snapshot(bad);assert(planMaintenancePoints(r,flat(r),retained,error,true));
    assert(retained.finalPrograms==output.finalPrograms && retained.barriers.size()==4);
    assert(retained.barriers[2].precedingPoints==96 && retained.barriers[3].invalidateMask==0x100);
    for(const auto& tail:std::vector<Stream>{draw(),{0x2181,0},{0xC0003B00,0x100},{0xC0004600,15},shaders()}) {
        bad=good;append(bad,tail);reject(bad,MaintenancePointFailure::EventOrdering);
    }
    for(const auto& e:std::vector<Stream>{{0xC0025800,0x80000003,0x2002,7},{0xC0025800,3,0x2001,7},
            {0xC0025800,3,0x20000002,7},{0xC0015800,3,0x2002}}) {
        bad=good;bad.resize(bad.size()-8);append(bad,e);reject(bad,MaintenancePointFailure::Event);
    }
    bad=good;append(bad,event());reject(bad,MaintenancePointFailure::Event);
    bad=good;bad.resize(bad.size()-8);reject(bad,MaintenancePointFailure::Incomplete);
    bad=batch(0);reject(bad,MaintenancePointFailure::Event);
    reject(batch(129),MaintenancePointFailure::DrawLayout);
    bad=idle();append(bad,{0xC0004600,16});reject(bad,MaintenancePointFailure::Event);
    bad=idle();append(bad,{0xC0003B00,1});reject(bad,MaintenancePointFailure::Opcode);
    bad=idle();for(unsigned i=0;i<63;++i)append(bad,{0xC0004600,15});
    append(bad,{0xC0004600,15});reject(bad,MaintenancePointFailure::Event);
    bad=idle();for(unsigned i=0;i<17;++i)append(bad,shaders());reject(bad,MaintenancePointFailure::ShaderLayout);
    bad=good;bad.resize(bad.size()-8);
    for(unsigned i=0;i<65;++i)append(bad,event(0x2000+4*i));reject(bad,MaintenancePointFailure::Event);
    // Traversal follows call sites, not node-storage order. Child state survives
    // a return; each barrier retains the count of points preceding that event.
    auto parent=idle();append(parent,settings());
    auto child1=shaders();append(child1,draw());
    Stream child2=draw();append(child2,{0xC0004600,15});append(child2,draw());
    const auto at=static_cast<unsigned>(parent.size());
    append(parent,{0xC0013F00,0x3000,static_cast<unsigned>(child1.size()),
        0xC0013F00,0x4000,static_cast<unsigned>(child2.size())});append(parent,event());
    r=snapshot(parent);auto g=flat(r);
    g.nodes={{0,at+3,0x4000,1,snapshot(child2)},{0,at,0x3000,1,snapshot(child1)}};
    for(const auto& n:g.nodes){g.totalWords+=n.snapshot.words().size();g.totalPackets+=n.snapshot.packets().size();}
    assert(planMaintenancePoints(r,g,retained,error,true));
    assert(retained.points.size()==3 && retained.points[0].node==2 && retained.points[1].node==1);
    assert(retained.barriers.size()==2 && retained.barriers[1].precedingPoints==2);
    for(unsigned field=0;field<8;++field) {
        auto broken=g;
        switch(field) {
        case 0:++broken.totalWords;break;
        case 1:++broken.totalPackets;break;
        case 2:broken.nodes[0].parent=2;break;
        case 3:++broken.nodes[0].depth;break;
        case 4:++broken.nodes[0].parentOffsetWords;break;
        case 5:broken.nodes[0].physicalAddress+=4;break;
        case 6:broken.nodes[0].physicalAddress|=1;break;
        case 7:broken.nodes[0].parentOffsetWords=broken.nodes[1].parentOffsetWords;break;
        }
        assert(!planMaintenancePoints(r,broken,retained,error,true));++checks;
    }
    std::printf("MAINTENANCE_POINT_PLAN_PASS authored_points=96 negative_cases=%u nested_execution_order=1 "
        "unused_program_retention=1 gpu_executed=0 submission_authority=0\n",checks);
}
