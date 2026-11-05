#include "gta5_query_command_trace.h"
#include <cassert>
#include <cstdio>
#include <sstream>
using namespace recomp::gpu;
int main() {
    constexpr unsigned Source=0x83817028,Query=0x10000,Device=0x20000,Commands=0xA0100000;
    std::vector<uint8_t> source(168),query(156),device(13860),commands(96);
    const auto put=[](auto& b,unsigned at,unsigned v){for(unsigned i=0;i<4;++i)b[at+i]=v>>(24-8*i);};
    put(source,120,Query);put(query,0,Device);put(query,4,9);
    put(device,48,Commands);put(device,56,Commands+4096);
    for(unsigned i=0;i<commands.size();++i)commands[i]=uint8_t(i);
    const Gta5ResourceReader image=[&](unsigned a,unsigned n)->std::optional<std::vector<uint8_t>> {
        if(a!=Source || n!=source.size())return std::nullopt;return source;
    };
    const Gta5ResourceReader heap=[&](unsigned a,unsigned n)->std::optional<std::vector<uint8_t>> {
        if(a==Query && n==query.size())return query;if(a==Device && n==device.size())return device;
        return std::nullopt;
    };
    const Gta5ResourceReader graphics=[&](unsigned a,unsigned n)->std::optional<std::vector<uint8_t>> {
        if(a!=Commands+4 || n>commands.size())return std::nullopt;
        return std::vector<uint8_t>(commands.begin(),commands.begin()+n);
    };
    const auto begin=[&](){return Gta5QueryCommandTrace::begin(Source,Query,0x82364A34,image,heap);};
    assert(!Gta5QueryCommandTrace::begin(Source+4,Query,0x82364A34,image,heap));
    assert(!Gta5QueryCommandTrace::begin(Source,Query,0x82364A30,image,heap));
    assert(!Gta5QueryCommandTrace::begin(Source,Query+4,0x82364A34,image,heap));
    assert(!Gta5QueryCommandTrace::begin(Source,Query,0x82364A34,{},heap));
    assert(!Gta5QueryCommandTrace::begin(Source,Query,0x82364A34,image,{}));
    put(query,4,10);assert(!begin());put(query,4,9);
    put(query,0,Device+4);assert(!begin());put(query,0,Device);
    put(device,48,Commands+2);assert(!begin());
    put(device,48,Commands+4100);assert(!begin());
    put(device,48,0xFFFFFFFC);put(device,56,0xFFFFFFFF);assert(!begin());
    put(device,48,Commands);put(device,56,Commands+4096);
    auto trace=begin();assert(trace && trace->firstWord==Commands+4 && trace->pendingCommands.empty());
    const auto original=*trace;
    trace->returned(heap,graphics);assert(trace->beginCommands.empty());
    assert(std::string(trace->status)=="begin-return-shape-changed");
    *trace=original;put(device,48,Commands+36);query[16]=2;
    trace->returned(heap,{});assert(std::string(trace->status)=="begin-command-storage-unavailable");
    *trace=original;trace->returned(heap,graphics);assert(trace->beginCommands.size()==36);
    const auto begun=*trace;
    Gta5StoppedQueryEvidence stopped;stopped.device=Device;stopped.sourceObject=Source;
    stopped.queryObject=Query;stopped.sampleBranch=1;stopped.query=query;
    put(device,48,Commands+96);trace->stopped(stopped,device,graphics);
    assert(trace->pendingCommands==commands && trace->lastWord==Commands+96);
    assert(std::string(trace->status)=="owned-pending-prefix-captured");
    unsigned negative=12;
    const auto badStop=[&](const auto& q,const auto& d,const Gta5ResourceReader& g,const char* expected) {
        auto t=begun;t.stopped(q,d,g);assert(t.pendingCommands.empty() && std::string(t.status)==expected);++negative;
    };
    const char* changed="stopped-query-owner-or-state-changed";
    auto bad=stopped;bad.device+=4;badStop(bad,device,graphics,changed);
    bad=stopped;bad.sourceObject+=4;badStop(bad,device,graphics,changed);
    bad=stopped;bad.queryObject+=4;badStop(bad,device,graphics,changed);
    bad=stopped;bad.sampleBranch=0;badStop(bad,device,graphics,changed);
    bad=stopped;bad.query[20]^=1;badStop(bad,device,graphics,changed);
    auto shortState=device;shortState.pop_back();badStop(stopped,shortState,graphics,changed);
    for(unsigned cursor:{Commands,Commands+38,Commands+65540}) {
        auto d=device;put(d,48,cursor);badStop(stopped,d,graphics,"pending-command-range-unavailable");
    }
    badStop(stopped,device,{},"pending-command-storage-unavailable");
    commands[0]^=1;badStop(stopped,device,graphics,"begin-command-prefix-changed");commands[0]^=1;
    const Gta5ResourceReader shortGraphics=[](unsigned,unsigned n){return std::optional{std::vector<uint8_t>(n-1)};};
    badStop(stopped,device,shortGraphics,"pending-command-storage-unavailable");
    unsigned saved=0;std::ostringstream meta;
    assert(writeGta5QueryCommandTrace(*trace,meta,[&](const char*,const auto& b){assert(!b.empty());++saved;return true;}));
    assert(saved==6 && meta.str().find("\"allocation_generation_proven\":false")!=std::string::npos);
    std::ostringstream failed;assert(!writeGta5QueryCommandTrace(*trace,failed,[](const char*,const auto&){return false;}));
    std::printf("GTA5_QUERY_COMMAND_TRACE_PASS negatives=%u owned_prefix_bytes=96 saved_files=6 unavailable_explicit=1 guest_writes=0 execution_authority=0\n",negative);
    auto ended=begun;query[16]=1;query[20]=0x40;put(device,48,Commands+96);
    ended.ended(heap,graphics);assert(ended.commandsThroughEnd==commands && ended.queryAtEnd==query);
    assert(std::string(ended.endStatus)=="original-end-bytes-captured-not-retired");
    unsigned endNegative=0;
    const auto badEnd=[&](const auto& h,const auto& g) {
        auto t=begun;t.ended(h,g);assert(t.commandsThroughEnd.empty());++endNegative;
    };
    badEnd(Gta5ResourceReader{},graphics);badEnd(heap,Gta5ResourceReader{});
    query[16]=2;badEnd(heap,graphics);query[16]=1;
    query[20]=0;badEnd(heap,graphics);query[20]=0x40;
    put(device,48,Commands+65540);badEnd(heap,graphics);put(device,48,Commands+96);
    commands[0]^=1;badEnd(heap,graphics);commands[0]^=1;
    std::ostringstream endMeta;unsigned endSaved=0;
    assert(writeGta5QueryCommandTrace(ended,endMeta,[&](const char*,const auto&){++endSaved;return true;}));
    assert(endSaved==8 && endNegative==6); // No stopped-draw interval in this copy.
    std::printf("GTA5_QUERY_END_TRACE_PASS negatives=%u saved_files=8 cpu_end_only=1 retirement=0\n",endNegative);
    std::optional<Gta5QueryCommandTrace> retained;
    assert(!retainGta5QueryEndDiagnostic(begun,retained) && !retained);
    assert(retainGta5QueryEndDiagnostic(ended,retained));
    ended={};trace.reset();commands.assign(commands.size(),0);query.clear();device.clear();
    std::ostringstream retiredMeta;unsigned retiredFiles=0;
    assert(writeGta5QueryCommandTrace(*retained,retiredMeta,[&](const char* name,const auto& b) {
        if(std::string(name)=="query-command-through-end.bin")assert(b.size()==96 && b[35]==35);
        ++retiredFiles;return true;
    }));
    assert(retiredFiles==8);retained.reset();assert(!retained);
    std::puts("GTA5_QUERY_END_RETENTION_PASS active_reset=1 producer_storage_gone=1 saved_files=8 next_bind_reset=1");
}
