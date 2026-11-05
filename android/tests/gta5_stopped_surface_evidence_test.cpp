#include "gta5_stopped_surface_evidence.h"
#include <cassert>
#include <cstdio>
#include <map>
using namespace recomp::gpu;
namespace {
void put(std::vector<std::uint8_t>& v,unsigned at,std::uint32_t w) {
    for(unsigned i=0;i<4;++i)v[at+i]=w>>(24-8*i);
}
std::vector<std::uint8_t> word(std::uint32_t w) {std::vector<std::uint8_t> b(4);put(b,0,w);return b;}
}
int main() {
    std::vector<std::uint8_t> state(13860,0),header(40,0);
    put(state,12816,0x640AF9E0);put(state,12820,0x640AFA10);
    put(state,12824,0xFFFFFFFC);put(state,12828,0x640AF9E1);
    put(state,12832,0x10001200);put(header,24,0x14000500);
    std::map<std::uint32_t,std::vector<std::uint8_t>> memory{
        {0x640AF9E0,header},{0x640AFA10,std::vector<std::uint8_t>(39)},
        {0x640AF800,word(0x640AF900)},{0x640AF8F8,word(0x82312340)},
        {0x640AF900,word(0x640AFA00)},{0x640AF9F8,word(0x82345670)},
        {0x640AFA00,word(0x640AF800)}};
    unsigned calls=0;
    const auto read=[&](unsigned at,unsigned n)->std::optional<std::vector<std::uint8_t>> {
        ++calls;assert(n==4 || n==40);
        assert(at!=0xFFFFFFFC && at!=0x640AF9E1); // Reject before calling a reader.
        const auto it=memory.find(at);return it==memory.end()?std::nullopt:std::optional{it->second};
    };
    const auto before=state;
    const auto result=captureGta5StoppedSurfaceEvidence(state,0x640AF800,0x82399880,read);
    assert(state==before && result.stack==0x640AF800 && result.link==0x82399880);
    assert(result.surfaces[0].argumentHeader==header);
    for(unsigned i=1;i<5;++i)assert(!result.surfaces[i].argumentHeader);
    assert(result.frames.size()==2 && result.frames[0].savedLink==0x82312340);
    assert(result.frames[1].parent==0x640AFA00 && calls==8);
    const auto unavailable=captureGta5StoppedSurfaceEvidence(state,0x640AF800,1,{});
    assert(unavailable.frames.empty() && !unavailable.surfaces[0].argumentHeader && calls==8);
    // Invalid/short state must not create surface-pointer evidence.
    state.resize(13123);const auto shortState=captureGta5StoppedSurfaceEvidence(state,0,0,read);
    assert(shortState.frames.empty() && !shortState.surfaces[0].object && calls==8);
    // Bounded traversal even if every frame is well-formed.
    for(unsigned i=0;i<10;++i){memory[0x640AF000+16*i]=word(0x640AF010+16*i);memory[0x640AF008+16*i]=word(0x82000000+4*i);}
    assert(captureGta5StoppedSurfaceEvidence({},0x640AF000,0,read).frames.size()==8);
    memory[0x640AF000]=word(0x640CF000); // Oversized frame gap.
    assert(captureGta5StoppedSurfaceEvidence({},0x640AF000,0,read).frames.empty());
    memory[0x640AF000]=word(0x640AF011); // Unaligned backchain.
    assert(captureGta5StoppedSurfaceEvidence({},0x640AF000,0,read).frames.empty());
    memory[0x640AF000]=word(0x640AF010);memory.erase(0x640AF008); // Missing saved LR.
    assert(captureGta5StoppedSurfaceEvidence({},0x640AF000,0,read).frames.empty());
    std::puts("STOPPED_SURFACE_EVIDENCE_PASS current_argument_header=1 bounded_frames=1 short_unowned_wrap_alignment_rejected=1 no_mutation=1 gpu=0");
}
