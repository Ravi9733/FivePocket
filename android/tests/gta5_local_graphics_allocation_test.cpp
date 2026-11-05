#include "gta5_local_graphics_allocation.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    // Authored arena, not captured game or GPU state.
    std::vector<std::uint8_t> state(15344);
    const auto set=[&](unsigned at,unsigned value){for(unsigned i=0;i<4;++i)state[at+i]=value>>(24-8*i);};
    set(15308,0xA0100000);set(15312,0xA0104000);set(15320,0xA0102000);
    set(15328,0xA0100000);set(15332,0xA0101000);set(15336,40);set(15340,4096);
    unsigned reads=0;
    const Gta5ResourceReader owned=[&](auto a,auto n)->std::optional<std::vector<std::uint8_t>> {
        ++reads;if(a<0xA0100000 || std::uint64_t{a}+n>0xA0104000)return std::nullopt;
        return std::vector<std::uint8_t>(n,0xCD);
    };
    const auto check=[&](std::optional<unsigned> fence=0u){return describeGta5LocalGraphicsAllocation(state,6,4,fence,owned);};
    const auto original=state;auto p=check();assert(p && p->address==0xA0102000 && p->end==0xA0102018 && p->counterAfter==64 && !p->wrapped);
    assert(state==original);assert(!check(std::nullopt));assert(!check(1));assert(!check(2));
    assert(!check(0xA0102017));assert(check(0xA010201B)); // delta1 exact boundary.
    set(15320,0xA0103FFC);p=check(1);assert(p && p->wrapped && p->generation==1 && p->address==0xA0100000);
    assert(!check(0));set(13600,0xA0100014);set(13604,1);assert(!check(1));
    set(13600,0xA0100018);assert(check(1));set(13604,0);assert(!check(1));
    state=original;set(15320,0);set(52,0xA0101FBC);p=check();assert(p && p->address==0xA0102000);
    const auto reject=[&](unsigned offset,unsigned value){state=original;set(offset,value);assert(!check());};
    reject(13920,0x10002000);reject(15336,UINT32_MAX);reject(15340,63);
    reject(15308,0x10000000);reject(15312,0xC0000000);reject(15320,0xFFFFFFFC);
    state=original;state[11069]=0x20;assert(!check());state=original;
    for(unsigned alignment:{0u,1u,3u,6u,8192u})assert(!describeGta5LocalGraphicsAllocation(state,6,alignment,0,owned));
    for(unsigned words:{0u,4097u,UINT32_MAX})assert(!describeGta5LocalGraphicsAllocation(state,words,4,0,owned));
    assert(!describeGta5LocalGraphicsAllocation(state,6,4,0,{}));
    assert(!describeGta5LocalGraphicsAllocation(state,6,4,0,[](auto,auto n){return std::vector<std::uint8_t>(n-1);}));
    state.pop_back();assert(!check());assert(reads);
    std::puts("GTA5_LOCAL_GRAPHICS_ALLOCATION_PASS readonly=1 missing_fence_rejected=1 wait_rejected=1 wrap_checked=1 ownership_checked=1 gpu_completion=0");
}
