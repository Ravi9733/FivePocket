#include "native_color_chain_events.h"
#include "native_gpu_word_readback.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
using Words=std::vector<std::uint32_t>;
static Pm4Snapshot snapshot(const Words& words) {
    std::vector<std::uint8_t> bytes;
    for (auto word:words) for (unsigned b=0;b<4;++b) bytes.push_back(word>>(24-b*8));
    Pm4Snapshot p;Pm4Error error;assert(decodePm4(bytes.data(),bytes.size(),{},p,error));return p;
}
static DrawCommandPlan plan(const Words& words) {
    DrawCommandPlan p;DrawCommandError error;
    assert(planDrawCommandCandidates(snapshot(words),{},{},p,error));return p;
}
static Words encode(const NativeColorChainEventRequests& e) {
    Words words{unsigned(e.staging.count),e.staging.bufferBytes,e.cacheFlushOffset,
        e.physicalAddresses[0],e.physicalAddresses[1],e.packetOffsets[0],e.packetOffsets[1]};
    for (const auto& w:e.staging.words) {
        words.push_back(w.byteOffset);words.push_back(unsigned(w.phase));
        for (auto b:w.bigEndianBytes) words.push_back(b);
    }
    return words;
}
int main() {
    // Synthetic shader/commands, not retail payloads and no GPU execution.
    const Words words{0xC0042B00,0,3,1,2,3,
        0xC0003601,0x30088,0xC0012201,0,0x40086,0xC0012201,0,0x40086,
        0xC0003600,0x30088,0xC0004600,6,0x21F9,0,
        0xC0025800,3,0x1002,0x12345678,0xC0025800,3,0x2002,0xA1B2C3D4};
    auto input=plan(words);NativeColorChainEventRequests output;
    assert(planNativeColorChainFinalEvents(input,output));
    assert((output.cacheFlushOffset==16 && output.packetOffsets==std::array<std::uint32_t,2>({20,24})));
    assert((output.physicalAddresses==std::array<std::uint32_t,2>({0x1000,0x2000})));
    assert(output.staging.count==2 && output.staging.bufferBytes==40);
    assert(output.staging.words[0].byteOffset==16 && output.staging.words[1].byteOffset==20);
    assert((output.staging.words[0].bigEndianBytes==std::array<std::uint8_t,4>({0x12,0x34,0x56,0x78})));
    assert((output.staging.words[1].bigEndianBytes==std::array<std::uint8_t,4>({0xA1,0xB2,0xC3,0xD4})));
    const auto saved=encode(output);unsigned rejects=0;
    const auto reject=[&](const DrawCommandPlan& p) {
        assert(!planNativeColorChainFinalEvents(p,output) && encode(output)==saved);++rejects;
    };
    for (unsigned at:{21u,25u}) for (unsigned bit=0;bit<32;++bit) {
        auto bad=words;bad[at]^=1u<<bit;reject(plan(bad));
    }
    for (unsigned at:{22u,26u}) {
        for (unsigned mode:{0u,1u,3u}) {auto bad=words;bad[at]=(bad[at]&~3u)|mode;reject(plan(bad));}
        for (auto address:{0x20000002u,0xFFFFFFFEu}) {auto bad=words;bad[at]=address;reject(plan(bad));}
    }
    auto bad=words;bad[26]=bad[22];reject(plan(bad));
    for (unsigned bit=0;bit<32;++bit) {bad=words;bad[17]^=1u<<bit;reject(plan(bad));}
    bad=words;bad.erase(bad.begin()+16,bad.begin()+18);reject(plan(bad));
    bad=words;bad.erase(bad.begin()+24,bad.end());reject(plan(bad));
    bad=words;bad.insert(bad.end(),{0xC0025800,3,0x3002,7});reject(plan(bad));
    bad=words;bad.insert(bad.end(),{0xC0004600,6});reject(plan(bad));
    bad=words;bad.insert(bad.begin()+6,{0xC0004600,6});reject(plan(bad));
    bad=words;bad.insert(bad.begin()+6,{0xC0025800,3,0x3002,7});reject(plan(bad));
    bad=words;bad.insert(bad.end(),{0xC0003600,0x30088});reject(plan(bad));
    bad=words;bad.erase(bad.begin()+14,bad.begin()+16);reject(plan(bad));
    auto broken=input;broken.draws[0].offsetWords++;reject(broken);
    broken=input;broken.actions[5].priorDraws++;reject(broken);
    broken=input;broken.actions[0].offsetWords++;reject(broken);
    broken=input;broken.actions.pop_back();reject(broken);
    // Forged mutable plan metadata cannot hide malformed event headers/framing.
    for (unsigned at:{16u,20u,24u}) for (unsigned bit:{0u,1u,7u,15u}) {
        bad=words;bad[at]^=1u<<bit;broken=input;broken.commands=snapshot(bad);reject(broken);
    }
    // Packet values are not restricted to known sample markers or timestamps.
    for (unsigned value:{0u,1u,0x80000000u,0xFFFFFFFFu}) {
        bad=words;bad[23]=value;bad[27]=~value;
        assert(planNativeColorChainFinalEvents(plan(bad),output));
        assert((output.staging.words[0].bigEndianBytes==std::array<std::uint8_t,4>({
            std::uint8_t(value>>24),std::uint8_t(value>>16),std::uint8_t(value>>8),std::uint8_t(value)})));
    }
    assert(planNativeColorChainFinalEvents(input,output));
    std::array<std::uint8_t,40> readback;readback.fill(0x5A);
    for (const auto& w:output.staging.words) if (w.phase==NativeWordPhase::AfterGraphics)
        for (unsigned b=0;b<4;++b) readback[w.byteOffset+b]=w.bigEndianBytes[b];
    AuditedNativeWords audited;NativeWordReadbackFailure failure;
    assert(auditNativeWordReadback(output.staging,readback.data(),40,audited,failure) && audited.count==2);
    readback[16]^=1;assert(!auditNativeWordReadback(output.staging,readback.data(),40,audited,failure));
    std::printf("NATIVE_COLOR_CHAIN_EVENTS: %u rejection controls; literal BE staging only, cache flush NOT executed PASS\n",rejects);
}
