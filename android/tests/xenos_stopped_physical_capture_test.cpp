#include "xenos_stopped_physical_capture.h"
#include <cassert>
#include <iostream>
#include <stdexcept>
using namespace recomp::gpu;
int main() {
    const auto scope=StoppedSwapRequestScope::make(0xA793B130,0x0793B130,0xA7900000,0x700000,0x071D0000);
    assert(scope && scope->permitsCommandPayload(0x0791F164,0x0793B130-0x0791F164) &&
        scope->permitsCommandPayload(0x0793B130,256) &&
        scope->permitsOwnershipQuery(0x07900000,0x700000) &&
        scope->permitsOwnershipQuery(0x071D0000,StoppedSwapRequestScope::ImageBytes));
    unsigned requestGuards=0;
    for(const auto& range:StoppedPhysicalRanges{{0x078FFFFC,4},{0x0793B22C,8},{0x0793B230,4},
            {0x0791F165,4},{0x0791F164,3},{0x0791F164,0},{0x071D0000,4},{0x1F670040,4},
            {0x0791F164,UINT32_MAX},{UINT32_MAX,4}}){
        assert(!scope->permitsCommandPayload(range[0],range[1]));++requestGuards;
    }
    for(const auto& range:StoppedPhysicalRanges{{0x071CFFFC,4},{0x071D0000,StoppedSwapRequestScope::ImageBytes+4},
            {0x071D0001,4},{0x071D0000,0},{0x1F670040,4},{0x07900000,0x700004},{0x078FFFFC,8}}){
        assert(!scope->permitsOwnershipQuery(range[0],range[1]));++requestGuards;
    }
    const auto commandsOnly=StoppedSwapRequestScope::make(0xA793B130,0x0793B130,0xA7900000,0x700000);
    assert(commandsOnly && commandsOnly->permitsCommandPayload(0x0793B130,256) &&
        !commandsOnly->permitsOwnershipQuery(0x071D0000,4));
    for(const auto& fields:std::vector<std::array<unsigned,5>>{
            {0xA793B131,0x0793B130,0xA7900000,0x700000,0x071D0000},
            {0xA793B130,0x0793B131,0xA7900000,0x700000,0x071D0000},
            {0xA793B130,0x0793B130,0xA7900001,0x700000,0x071D0000},
            {0xA793B130,0x0793B130,0xA7900000,0x700001,0x071D0000},
            {0xA793B130,0x0793B130,0xA7900000,0x100,0x071D0000},
            {0xA793B130,0x0793B130,0xA793B134,0x1000,0x071D0000},
            {0xFFFFF000,0x0793B130,0xFFFFF000,0x2000,0x071D0000},
            {0xA793B130,0x1000,0xA7900000,0x700000,0x071D0000},
            {0x0793B130,0xA793B130,0x07900000,0x700000,0x071D0000},
            {0xA793B130,0x1FFFB130,0xA7900000,0x700000,0x071D0000},
            {0xA793B130,0x0793B130,0xA7900000,0x700000,0x071D0001},
            {0xA793B130,0x0793B130,0xA7900000,0x700000,0x1FFFF000},
            {0xA793B130,0x0793B130,0xA7900000,0x700000,0x07900000}}){
        assert(!StoppedSwapRequestScope::make(fields[0],fields[1],fields[2],fields[3],fields[4]));++requestGuards;
    }
    std::cout<<"STOPPED_SWAP_REQUEST_SCOPE: "<<requestGuards<<"guards,command-only payload,image metadata only,no lease/execution PASS\n";
    assert(validStoppedOwnershipRanges({{0x1F2D8000,3768320}}));
    assert(!validStoppedPhysicalRanges({{0x1F2D8000,3768320}},MaxStoppedPhysicalByteBudget));
    assert(validStoppedOwnershipRanges({{0,0x1000000},{0x1000000,4},{0x1FFFFFFC,4}}));
    for(const auto& bad:std::vector<StoppedPhysicalRanges>{{},{{0,0}},{{0,0x1000004}},{{1,4}},
        {{0x1FFFFFFC,8}},{{0,8},{4,4}},StoppedPhysicalRanges(9,{0,4})})assert(!validStoppedOwnershipRanges(bad));
    const StoppedPhysicalRanges ranges{{0x1000,12},{0x2000,16}};
    std::vector<std::vector<std::uint8_t>> output{{0x17}};
    const auto sentinel=output;
    unsigned calls=0;
    const Pm4PhysicalReader reader=[&](auto a,auto n) {
        ++calls; return std::optional<std::vector<std::uint8_t>>(std::vector<std::uint8_t>(n,a>>12));
    };
    assert(captureStoppedPhysicalRanges(ranges,reader,output));
    assert(calls==2 && output.size()==2 && output[0]==std::vector<std::uint8_t>(12,1));
    unsigned failures=0, guards=0;
    for (unsigned at=1;at<=2;++at) for (unsigned mode=0;mode<5;++mode) {
        calls=0;output=sentinel;
        const Pm4PhysicalReader failing=[&](auto,auto n)->std::optional<std::vector<std::uint8_t>> {
            if (++calls!=at) return std::vector<std::uint8_t>(n,0);
            if (mode==0) return std::nullopt;
            if (mode==1) throw std::runtime_error("reader");
            if (mode==2) throw std::bad_alloc();
            return std::vector<std::uint8_t>(mode==3?n-4:n+4,0);
        };
        assert(!captureStoppedPhysicalRanges(ranges,failing,output));
        assert(output==sentinel && calls==at);++failures;
    }
    for (const auto& bad:std::vector<StoppedPhysicalRanges>{ {},{{0,0}},{{1,4}},{{0,3}},{{0,4100}},
            {{0x1FFFFFFC,8}},{{UINT32_MAX,4}},{{0x1000,8},{0x1004,4}},{{0,4},{0,4}},
            {{0,4},{0x1000,4096}},StoppedPhysicalRanges(9,{0,4})}) {
        calls=0;output=sentinel;assert(!captureStoppedPhysicalRanges(bad,reader,output));
        assert(output==sentinel && !calls);++guards;
    }
    assert(validStoppedPhysicalRanges({{0,4096}}));
    assert(validStoppedPhysicalRanges({{0x1FFFFFFC,4}}));
    assert(validStoppedPhysicalRanges({{0x1000,4},{0x1004,4}}));
    output=sentinel;assert(!captureStoppedPhysicalRanges(ranges,{},output));assert(output==sentinel);
    for(unsigned budget:{0u,1u,4097u,MaxStoppedPhysicalByteBudget+4,UINT32_MAX}){
        calls=0;output=sentinel;assert(!captureStoppedPhysicalRanges(ranges,reader,output,budget));
        assert(output==sentinel && calls==0);
    }
    calls=0;output=sentinel;
    const StoppedPhysicalRanges large{{0x1000,MaxStoppedPhysicalByteBudget-4},{0x400000,4}};
    assert(!captureStoppedPhysicalRanges(large,reader,output));assert(output==sentinel && calls==0);
    assert(captureStoppedPhysicalRanges(large,reader,output,MaxStoppedPhysicalByteBudget));
    assert(calls==2 && output[0].size()==MaxStoppedPhysicalByteBudget-4 && output[1].size()==4);
    auto excessive=large;excessive[1][1]=8;calls=0;output=sentinel;
    assert(!captureStoppedPhysicalRanges(excessive,reader,output,MaxStoppedPhysicalByteBudget));
    assert(output==sentinel && calls==0);
    calls=0;const Pm4PhysicalReader lateFailure=[&](auto,auto n)->std::optional<std::vector<std::uint8_t>>{
        if(++calls==2)return std::nullopt;return std::vector<std::uint8_t>(n,7);};
    assert(!captureStoppedPhysicalRanges(large,lateFailure,output,MaxStoppedPhysicalByteBudget));
    assert(output==sentinel && calls==2);
    std::cout<<"STOPPED_PHYSICAL_BUDGET_V1 default=4096 maximum=2097152 explicit_full_capture=1 late_failure_atomic=1\n";
    std::cout<<"STOPPED_PHYSICAL_CAPTURE_V1 failures="<<failures<<" guards="<<guards<<" executed=0\n";
}
