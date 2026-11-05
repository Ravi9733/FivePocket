#include "query_neutral_fan_fixture.h"
#include "gta5_pending_query_neutral_fan.h"
#include <cstdio>
using namespace recomp::gpu;
int main(int argc,char** argv) {
    assert(argc==3);QueryNeutralFanFixture f(argv[1]);const std::string cap=argv[2];
    Gta5StoppedQueryEvidence q;
    q.source=QueryNeutralFanFixture::bytes(cap+"/query-source-object.bin");
    q.query=QueryNeutralFanFixture::bytes(cap+"/sample-query-object.bin");
    q.samples=QueryNeutralFanFixture::bytes(cap+"/sample-query-slot.bin");
    const auto word=QueryNeutralFanFixture::word;
    q.device=f.draw.device=word(q.query,0);q.sourceObject=0x83817028;q.sampleBranch=1;
    q.queryObject=word(q.source,120);q.sampleAddress=word(q.query,28);
    const auto first=word(f.draw.deviceState,48)-48;
    auto commands=QueryNeutralFanFixture::bytes(cap+"/query-command-pending.bin");
    auto device=f.draw.deviceState,source=q.source,query=q.query,slot=q.samples,vertices=f.draw.vertices;
    const Gta5ResourceReader heap=[&](unsigned a,unsigned n)->std::optional<std::vector<uint8_t>> {
        if(a==q.device && n==device.size())return device;
        if(a==q.queryObject && n==query.size())return query;return std::nullopt;
    };
    const Gta5ResourceReader image=[&](unsigned a,unsigned n)->std::optional<std::vector<uint8_t>> {
        return a==q.sourceObject && n==source.size()?std::optional{source}:std::nullopt;
    };
    const Gta5ResourceReader data=[&](unsigned a,unsigned n)->std::optional<std::vector<uint8_t>> {
        if(a==q.sampleAddress && n==slot.size())return slot;
        if(a==first && n==commands.size())return commands;
        if(a==f.draw.vertexAddress && n==vertices.size())return vertices;return std::nullopt;
    };
    const auto guard=Gta5PendingQueryNeutralFan::capture(f.draw,q,data);assert(guard && guard->unchanged(f.draw,image,heap,data));
    unsigned negatives=0;
    for(auto* v:{&device,&source,&query,&slot,&vertices,&commands}) {
        (*v)[0]^=1;assert(!guard->unchanged(f.draw,image,heap,data));(*v)[0]^=1;++negatives;
    }
    assert(!guard->unchanged(f.draw,{},heap,data));++negatives;
    assert(!guard->unchanged(f.draw,image,{},data));++negatives;
    assert(!guard->unchanged(f.draw,image,heap,{}));++negatives;
    for(unsigned at=0;at<commands.size();++at)if(at<24 || at>=28) {
        commands[at]^=1;assert(!Gta5PendingQueryNeutralFan::capture(f.draw,q,data));commands[at]^=1;++negatives;
    }
    auto bad=q;bad.query[16]=1;assert(!Gta5PendingQueryNeutralFan::capture(f.draw,bad,data));++negatives;
    bad=q;bad.query[20]=0x40;assert(!Gta5PendingQueryNeutralFan::capture(f.draw,bad,data));++negatives;
    bad=q;bad.sampleBranch=0;assert(!Gta5PendingQueryNeutralFan::capture(f.draw,bad,data));++negatives;
    bad=q;bad.samples.pop_back();assert(!Gta5PendingQueryNeutralFan::capture(f.draw,bad,data));++negatives;
    assert(negatives==61 && guard->unchanged(f.draw,image,heap,data));
    std::printf("QUERY_NEUTRAL_PENDING_PASS bytes=52 current_capture=1 negatives=%u writes=0 lifetime_beyond_gate=0 retirement=0\n",negatives);
}
