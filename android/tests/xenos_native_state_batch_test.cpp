#include "xenos_native_state_batch.h"
#include <cassert>
using namespace recomp::gpu;
int main() {
    // Synthetic dimensions, event addresses/values, and nontrivial bin words.
    const std::vector<std::uint32_t> words{0xC0006000,0x12345678,0xC0006100,0x9ABCDEF0,
        0xC0006200,0x11223344,0xC0006300,0x55667788,0x22080,0xFFFF0001,0,0x200030,
        0x5C8,0x20000,0xF01,0x200E,0xC0003B00,0x7FFF,0xC0001000,0xDEADBEEF,
        0xC0025800,3,0x10002,0x01020304,0xC0025800,3,0x10006,0xAABBCCDD};
    NativeStateBatch plan; Pm4Error error;
    assert(planNativeStateBatch(words,plan,error));
    assert(plan.stateWrites==11 && plan.waits==1 && plan.invalidations==1);
    assert(plan.registers.at(0x2080)==0xFFFF0001 && plan.registers.at(0x2082)==0x200030);
    assert(plan.registers.at(0x21F9)==3 && *plan.bins[1]==0x9ABCDEF0 && *plan.bins[2]==0x11223344);
    assert(plan.updates.count==2 && plan.updates.bufferBytes==8 && plan.destinations[1]==0x10004);
    assert((plan.updates.words[0].bigEndianBytes==std::array<std::uint8_t,4>{1,2,3,4}));
    const auto reject=[&](const auto& bad) {
        plan.stateWrites=0xDEAD; plan.destinations[0]=0xBEEF;
        assert(!planNativeStateBatch(bad,plan,error));
        assert(plan.stateWrites==0xDEAD && plan.destinations[0]==0xBEEF);
    };
    for (auto index : {0u,2u,4u,6u,8u,12u,13u,14u,15u,16u,17u,18u,20u,21u,22u,24u,25u,26u}) {
        auto bad=words; bad[index]^=1; reject(bad);
    }
    for (auto tail : {std::vector<std::uint32_t>{0xC0003600,0x10081},
                     {0xC0013F00,0x1000,1},{0xC0003B00,0},
                     {0xC0025800,0x80000003,0x1000A,1},
                     {0xC0025800,3,0x20000002,1},
                     {0xC0025800,3,0x10002,1}, {0x1DC,0x40000}, {0xC0003C00,3}}) {
        auto bad=words; bad.insert(bad.end(),tail.begin(),tail.end()); reject(bad);
    }
    auto truncated=words; truncated.pop_back(); reject(truncated);
    for (auto bit : {1u,2u,3u,4u,5u,6u,7u,15u}) {
        auto bad=words; bad[0]|=1u<<bit; reject(bad);
    }
    reject(std::vector<std::uint32_t>{0x80000001});
    reject(std::vector<std::uint32_t>{}); reject(std::vector<std::uint32_t>(257,0x80000000));
    assert(planNativeStateBatch({0xC0006000,7},plan,error) && plan.updates.count==0 && plan.updates.bufferBytes==4);
    assert(plan.bins[0]==7 && !plan.bins[1] && !plan.bins[2] && !plan.bins[3]);
    assert(planNativeStateBatch({0xC0025800,3,0x1FFFFFFE,0xFFFFFFFF},plan,error));
    assert(plan.destinations[0]==0x1FFFFFFC);
    const std::vector<std::uint32_t> setup{0xD02,0x10800,0x30A02,0xC0100000,0x07F00000,0xC0000000,0x00100000,
        0x1DD,0x10000,0x1DC,0x20037,0xD04,0,0x5C8,0x20000};
    assert(planNativeStateBatch(setup,plan,error) && plan.registers.at(0x1DC)==0x20037 &&
        plan.registers.at(0x1DD)==0x10000 && plan.updates.count==0);
    for (auto i : {1u,3u,4u,5u,6u,8u,12u,14u}) { auto bad=setup; bad[i]^=1; reject(bad); }
    auto bad=setup; bad[10]=0x40037; reject(bad);
    bad=setup; bad.insert(bad.end(),{0x578,0xAABBCCDD}); reject(bad); // Writeback itself not modeled yet.
    const std::vector<std::uint32_t> pair{0x5C8,0x20000,0xE00,1,0xE40,1};
    reject(pair); // Existing/default executable profile never absorbs unknown registers.
    assert(planNativeStateBatch(pair,plan,error,true) && plan.experimentalIdleCachePair &&
        plan.stateWrites==3 && plan.waits==1 && plan.invalidations==0 &&
        plan.registers.empty() && plan.updates.count==0 && plan.updates.bufferBytes==4);
    for(unsigned word=0;word<6;++word) for(unsigned bit=0;bit<32;++bit) {
        auto changed=pair; changed[word]^=1U<<bit;
        plan.stateWrites=0xDEAD;
        assert(!planNativeStateBatch(changed,plan,error,true) && plan.stateWrites==0xDEAD);
    }
    for(unsigned a=0;a<3;++a)for(unsigned b=a+1;b<3;++b) {
        auto changed=pair;std::swap(changed[a*2],changed[b*2]);std::swap(changed[a*2+1],changed[b*2+1]);
        assert(!planNativeStateBatch(changed,plan,error,true));
    }
    for(unsigned begin=0;begin<6;++begin)for(unsigned end=begin+1;end<=6;++end) {
        if(begin==0 && end==6)continue;
        const std::vector<std::uint32_t> part(pair.begin()+begin,pair.begin()+end);
        if(planNativeStateBatch(part,plan,error,true))assert(!plan.experimentalIdleCachePair);
    }
    auto mixed=pair;mixed.push_back(0x80000000);assert(!planNativeStateBatch(mixed,plan,error,true));
    mixed=pair;mixed.insert(mixed.begin(),0x80000000);assert(!planNativeStateBatch(mixed,plan,error,true));
    assert(planNativeStateBatch(setup,plan,error,true) && !plan.experimentalIdleCachePair);
}
