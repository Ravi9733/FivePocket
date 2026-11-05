#include "gta5_stopped_query_evidence.h"
#include <cassert>
#include <cstdio>
#include <sstream>
#include <stdexcept>
using namespace recomp::gpu;
int main() {
    constexpr unsigned Device=0x10040000,Query=0x11000000,Sample=0xBC120000;
    std::vector<std::uint8_t> source(168),query(156),samples(64,0xDA);
    const auto put=[](auto& b,unsigned at,unsigned v){for(unsigned i=0;i<4;++i)b[at+i]=v>>(24-i*8);};
    put(source,120,Query);put(source,124,11);put(query,0,Device);put(query,4,9);
    put(query,28,Sample);put(query,148,1);put(query,152,1);query[16]=2;
    unsigned imageReads=0,heapReads=0,dataReads=0;
    Gta5ResourceReader image=[&](auto a,auto n)->std::optional<std::vector<uint8_t>> {
        assert(a==0x83817028 && n==168);++imageReads;return source;};
    Gta5ResourceReader heap=[&](auto a,auto n)->std::optional<std::vector<uint8_t>> {
        assert(a==Query && n==156);++heapReads;return query;};
    Gta5ResourceReader data=[&](auto a,auto n)->std::optional<std::vector<uint8_t>> {
        assert(a==Sample && n==64);++dataReads;return samples;};
    Gta5StoppedSurfaceEvidence caller;caller.link=0x823BD3A0;
    caller.frames={{0x64001000,0x64001060,0x82364C30},{0x64001060,0x64001510,0x8234F018}};
    const auto capture=[&](unsigned branch=1){return captureGta5StoppedQueryEvidence(Device,branch,0x83817028,caller,image,heap,data);};
    const auto result=capture();assert(result && result->source==source && result->query==query && result->samples==samples);
    assert(result->sampleBranch==1 && result->sampleAddress==Sample && dataReads==1);
    assert(capture(0)->sampleBranch==0); // Preserve skipped branch, don't invent begin.
    std::ostringstream output;unsigned saved=0;
    assert(writeGta5StoppedQueryEvidence(*result,output,[&](const auto&,const auto&){++saved;return true;}) && saved==3);
    assert(output.str().find("\"completion_proven\":false")!=std::string::npos);
    assert(output.str().find("\"sample_branch_r20\":1")!=std::string::npos);
    const auto reads=std::array{imageReads,heapReads,dataReads};
    assert(!capture(2));caller.link^=4;assert(!capture());caller.link^=4;
    caller.frames[0].savedLink^=4;assert(!capture());caller.frames[0].savedLink^=4;
    caller.frames[1].savedLink^=4;assert(!capture());caller.frames[1].savedLink^=4;
    assert(!captureGta5StoppedQueryEvidence(Device,1,0x8381702C,caller,image,heap,data));
    assert((reads==std::array{imageReads,heapReads,dataReads}));
    put(query,4,10);assert(capture()->samples.empty());put(query,4,9);
    put(query,0,Device+4);assert(capture()->samples.empty());put(query,0,Device);
    put(query,148,2);assert(capture()->samples.empty());put(query,148,1);
    put(query,152,2);assert(capture()->samples.empty());put(query,152,1);
    for(auto address:{0u,Sample+1,0xFFFFFFFCu}){put(query,28,address);assert(capture()->samples.empty());}
    put(query,28,Sample);assert(dataReads==reads[2]);
    data=[](auto,auto){return std::optional<std::vector<uint8_t>>{};};assert(capture()->samples.empty());
    heap=[](auto,auto){return std::optional<std::vector<uint8_t>>{std::vector<uint8_t>(155)};};
    assert(capture()->query.empty());
    image=[](auto,auto){return std::optional<std::vector<uint8_t>>{std::vector<uint8_t>(167)};};
    assert(capture()->source.empty());
    image=[](auto,auto)->std::optional<std::vector<uint8_t>>{throw std::runtime_error("diagnostic unavailable");};
    assert(!capture());
    std::ostringstream failed;assert(!writeGta5StoppedQueryEvidence(*result,failed,[](const auto&,const auto&){return false;}));
    puts("GTA5_STOPPED_QUERY_EVIDENCE_PASS caller_chain=1 mutable_image=1 branch_preserved=1 bounded_owned_reads=1 unavailable_explicit=1 capture_only=1");
}
