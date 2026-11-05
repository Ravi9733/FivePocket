#include "gta5_native_tile_submission_capture.h"
#include <cassert>
#include <cstdio>
#include <map>
using namespace recomp::gpu;
int main() {
    constexpr unsigned device=0x10000000,ring=0xA0001000,fence=0xA0002000,ib=0xA0003000,mask=127;
    std::map<unsigned,std::vector<std::uint8_t>> memory{{device,std::vector<std::uint8_t>(15308)},
        {ring,std::vector<std::uint8_t>((mask+1)*4)},{fence+60,std::vector<std::uint8_t>(4)},
        {ib,{0,0,5,0xC8,0,2,0,0}}};
    const auto set=[&](unsigned at,unsigned offset,unsigned value) {for(unsigned i=0;i<4;++i)memory[at][offset+i]=value>>(24-i*8);};
    const auto read=[&](unsigned address,unsigned bytes)->std::optional<std::vector<std::uint8_t>> {
        if(address<0x20000000 && address!=device)address+=0xA0000000;
        const auto p=memory.find(address);if(p==memory.end() || p->second.size()!=bytes)return std::nullopt;return p->second;
    };
    const auto reset=[&] {
        set(device,15300,ring);set(device,15304,mask);set(device,11024,fence);
        set(device,11088,126);set(fence+60,0,126);
        set(ring,126*4,0xC0013F00);set(ring,127*4,ib-0xA0000000);set(ring,0,2);
    };
    reset();auto c=Gta5NativeTileSubmissionCapture::begin(device,read,read);assert(c);
    set(device,11088,1);const auto unchanged=memory;
    assert(c->capture(1,read,read) && memory==unchanged && c->entryReadWord()==126 && c->lastWriteWord()==1);
    assert(c->submissions().size()==1 && c->submissions()[0].ringWords.size()==3 &&
        c->submissions()[0].indirects[0].bytes==memory.at(ib));
    assert(c->seal(read,read) && memory==unchanged && !c->capture(1,read,read));
    assert(c->matchesCurrent(read,read));set(fence+60,0,127);assert(!c->matchesCurrent(read,read));set(fence+60,0,126);
    assert(c->matchesCurrent(read,read));
    for(unsigned failure=0;failure<6;++failure) {
        reset();auto bad=Gta5NativeTileSubmissionCapture::begin(device,read,read);assert(bad);set(device,11088,1);
        if(failure==0)set(fence+60,0,127);
        if(failure==1)set(device,15300,ring+4);
        if(failure==2)set(ring,126*4,0xC0013F01);
        if(failure==3)set(ring,127*4,0x1000); // Ring storage alias.
        if(failure==4)set(ring,0,3); // Unavailable bytes, never padded.
        if(failure==5)set(device,11088,2);
        const auto before=memory;assert(!bad->capture(1,read,read) && memory==before &&
            bad->phase()==Gta5NativeTileSubmissionCapture::Phase::Failed && bad->submissions().empty());
    }
    reset();auto stale=Gta5NativeTileSubmissionCapture::begin(device,read,read);assert(stale);set(device,11088,1);
    assert(stale->capture(1,read,read));memory[ib][7]^=1;assert(!stale->seal(read,read));memory[ib][7]^=1;
    reset();set(fence+60,0,125);assert(!Gta5NativeTileSubmissionCapture::begin(device,read,read));
    std::puts("NATIVE_TILE_ENQUEUE_CAPTURE_PASS wrap=1 accepted_not_completed=1 read_pointer_unchanged=1 unknown_or_changed_inputs_rejected=1 sealed_commands_checked=1 gpu_execution=0");
}
