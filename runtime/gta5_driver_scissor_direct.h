#pragma once
#include <array>
#include <cstdint>
#include <optional>

namespace recomp::gpu {
// Attempt v278: planGta5DriverScissor's `direct` for a device header whose scissor is not binned, read in place from the
// header's first 13128 bytes (the planner reads only bytes 11068, and the words 12816-12836 / 13104-13124 when flag 0x20 is
// set, for that decision); nullopt when the planner would bin it (the caller then plans from a copy as before). The same
// decision and values as the planner, without the 13 KB copy and the packet-word vector (v275 outdoor render-thread
// profile: the scissor hook 2.1%, its header copy 0.58%).
inline std::optional<std::array<std::uint32_t,3>> gta5DriverScissorDirect(const std::uint8_t* state,const std::array<std::uint32_t,4>& rectangle) {
    if(!state)return std::nullopt;
    const auto word=[&](unsigned at) {
        return (std::uint32_t{state[at]}<<24)|(std::uint32_t{state[at+1]}<<16)|(std::uint32_t{state[at+2]}<<8)|state[at+3];
    };
    const auto pack=[](std::uint32_t x,std::uint32_t y){return (x&0x3FFFu)|((y&0x3FFFu)<<16);};
    const auto flags=state[11068];
    bool binned=(flags&0x10)!=0;
    if(!binned && (flags&0x20)) {
        binned=true;
#if defined(RECOMP_V278_MUTANT_SCISSOR_ONE_SLOT) // test mutant: only the first surface slot compared
        for(unsigned i=0;i<1;++i)
#else
        for(unsigned i=0;i<5;++i)
#endif
            if(word(12816+i*4) && word(12816+i*4)!=word(13104+i*4))binned=false;
    }
    if(binned)return std::nullopt;
    return std::array<std::uint32_t,3>{0,pack(rectangle[0],rectangle[1]),pack(rectangle[2],rectangle[3])};
}
}
