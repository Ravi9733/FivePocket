#include "native_color_chain_sync.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
using Words=std::vector<std::uint32_t>;
static bool makePlan(const Words& words,DrawCommandPlan& p){
    std::vector<std::uint8_t> bytes;
    for(auto word:words)for(unsigned b=0;b<4;++b)bytes.push_back(word>>(24-b*8));
    Pm4Snapshot snapshot;Pm4Error decode;DrawCommandError error;
    return decodePm4(bytes.data(),bytes.size(),{},snapshot,decode) && planDrawCommandCandidates(snapshot,{},{},p,error);
}
int main(){
    // Synthetic framing and inert shader bytes. No retail payload or GPU work.
    const Words words{0x200D,0,0xC0042B00,0,3,1,2,3,0xC0003B00,0x300,
        0xC0003600,0x30088,0xA31,0x10000,0x10A2F,0,0x1000,
        0xC0043C00,3,0xA31,0,0x80000000,8,
        0xC0012200,0,0x40086,0xC0012200,0,0x40086,
        0xC0003B00,0x100,0x5C8,0x20000,0xC0003600,0x30088,
        0xC0004600,6,0x5C8,0x20000,0xC0003B00,0x7FFF,0x5C8,0x20000,
        0xC0025800,3,0x1002,11,0xC0025800,3,0x2002,22};
    DrawCommandPlan plan;assert(makePlan(words,plan));NativeColorChainSyncRequests output;
    assert(planNativeColorChainSync(plan,output));
    assert(output.depthStatusOffset==12 && output.depthRangeOffset==14 && output.depthWaitOffset==17 && output.cacheFlushOffset==35);
    assert((output.invalidationOffsets==std::array<unsigned,3>{8,29,39}) && (output.idleCleanOffsets==std::array<unsigned,3>{31,37,41}));
    const auto saved=output;unsigned rejects=0,parserRejections=0;
    const auto unchanged=[&](){return output.depthStatusOffset==saved.depthStatusOffset && output.depthRangeOffset==saved.depthRangeOffset &&
        output.depthWaitOffset==saved.depthWaitOffset && output.cacheFlushOffset==saved.cacheFlushOffset &&
        output.invalidationOffsets==saved.invalidationOffsets && output.idleCleanOffsets==saved.idleCleanOffsets;};
    const auto reject=[&](const auto& p){output=saved;assert(!planNativeColorChainSync(p,output) && unchanged());++rejects;};
    const auto rejectWords=[&](const Words& w){DrawCommandPlan p;if(!makePlan(w,p)){++parserRejections;return;}reject(p);};
    for(unsigned at:{1u,9u,13u,15u,16u,18u,19u,20u,21u,22u,30u,32u,36u,38u,40u,42u})
        for(unsigned bit=0;bit<32;++bit){auto bad=words;bad[at]^=1u<<bit;rejectWords(bad);}
    for(unsigned at:{12u,14u,17u,31u,37u,41u})for(unsigned bit:{0u,7u,15u}){
        auto bad=words;bad[at]^=1u<<bit;rejectWords(bad);
    }
    for(unsigned at:{8u,12u,29u,31u,37u,39u,41u}){
        auto bad=words;bad.erase(bad.begin()+at,bad.begin()+at+2);rejectWords(bad);
        bad=words;bad.insert(bad.begin()+at,words.begin()+at,words.begin()+at+2);rejectWords(bad);
    }
    auto bad=words;bad.insert(bad.begin()+8,{0x123,0});rejectWords(bad);
    bad=words;bad.insert(bad.begin()+8,{0xC0022100,0x5C8,0xFFFFFFFF,0});rejectWords(bad);
    bad=words;std::swap(bad[29+1],bad[39+1]);rejectWords(bad);
    for(unsigned reg:{0xA31u,0xA2Fu,0xA30u,0x200Du}){
        auto p=plan;p.waits[0].registers.erase(reg);reject(p);
        for(unsigned bit=0;bit<32;++bit){p=plan;p.waits[0].registers[reg].bits.known^=1u<<bit;reject(p);}
        p=plan;p.waits[0].registers[reg].lastAssignmentOffset=17;reject(p);
    }
    auto p=plan;p.waits.clear();reject(p);p=plan;p.waits[0].priorDraws=0;reject(p);
    p=plan;p.waits[0].offsetWords++;reject(p);
    p=plan;p.actions[6].kind=DrawCommandKind::Nop;reject(p);
    Words next=words;next.insert(next.begin(),{0x5C8,0x20000,0xF01,0x200E});DrawCommandPlan second;
    assert(makePlan(next,second));NativeColorChainSyncRequests later;
    assert(planNativeColorChainSync(second,later,NativeColorBatchPhase::PostOutput));
    assert(later.phase==NativeColorBatchPhase::PostOutput && later.leadingIdleOffset==0 && later.legacySetupOffset==2);
    assert(later.depthWaitOffset==saved.depthWaitOffset+4 && later.idleCleanOffsets[2]==saved.idleCleanOffsets[2]+4);
    const auto laterSaved=later;unsigned leadingRejects=0;
    const auto rejectLater=[&](const Words& bytes){DrawCommandPlan q;
        if(makePlan(bytes,q)){
            later=laterSaved;assert(!planNativeColorChainSync(q,later,NativeColorBatchPhase::PostOutput));
            assert(later.phase==laterSaved.phase && later.leadingIdleOffset==laterSaved.leadingIdleOffset &&
                later.legacySetupOffset==laterSaved.legacySetupOffset && later.depthWaitOffset==laterSaved.depthWaitOffset);
        }++leadingRejects;
    };
    for(unsigned at=0;at<4;++at)for(unsigned bit=0;bit<32;++bit){auto bad=next;bad[at]^=1u<<bit;rejectLater(bad);}
    for(unsigned at:{0u,2u}){
        auto bad=next;bad.erase(bad.begin()+at,bad.begin()+at+2);rejectLater(bad);
        bad=next;bad.insert(bad.begin()+at,next.begin()+at,next.begin()+at+2);rejectLater(bad);
    }
    auto latePrefix=words;latePrefix.insert(latePrefix.begin()+12,{0x5C8,0x20000,0xF01,0x200E});rejectLater(latePrefix);
    auto reversed=next;std::swap(reversed[0],reversed[2]);std::swap(reversed[1],reversed[3]);rejectLater(reversed);
    auto separated=next;separated.insert(separated.begin()+2,{0x200D,0});rejectLater(separated);
    assert(!planNativeColorChainSync(second,later));
    assert(!planNativeColorChainSync(plan,later,NativeColorBatchPhase::PostOutput));
    assert(!planNativeColorChainSync(second,later,static_cast<NativeColorBatchPhase>(99)));
    std::printf("NATIVE_POST_OUTPUT_COLOR_SYNC: %u leading-prefix refusals,phase separation,new idle/setup obligation retained,no execution PASS\n",leadingRejects);
    std::printf("NATIVE_COLOR_CHAIN_SYNC: %u request rejections, %u earlier parser rejections; zero-size not no-work, 3 idleclean waits retained PASS\n",rejects,parserRejections);
}
