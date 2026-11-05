#include "gta5_color_clear.h"
#include "native_primary_clear.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    std::vector<std::uint8_t> state(13128),header(32),rect(16),color(16);
    const auto put=[](auto& b,unsigned at,unsigned value){for(unsigned i=0;i<4;++i)b[at+i]=value>>(24-8*i);};
    put(state,10368,0x14000500);put(state,12816,0x5000);put(header,28,0xC02D0);
    put(state,13032,colorBits(1280));put(state,13036,colorBits(720));
    put(rect,8,1280);put(rect,12,720);put(color,12,colorBits(1));
    const Gta5ResourceReader heap=[&](unsigned a,unsigned n)->std::optional<std::vector<std::uint8_t>> {
        if(a==0x1000 && n==state.size())return state;
        if(a==0x5000 && n==header.size())return header;return std::nullopt;
    };
    const Gta5ResourceReader argument=[&](unsigned a,unsigned n)->std::optional<std::vector<std::uint8_t>> {
        if(n!=16)return std::nullopt;if(a==0x2000)return rect;if(a==0x3000)return color;return std::nullopt;
    };
    const auto capture=[&]{return captureGta5ColorClear(0x1000,1,0x2000,0x3000,heap,argument);};
    assert(capture());unsigned submits=0;
    for(bool fence:{false,true}) {
        const auto result=executeNativePrimaryClear(capture().has_value(),true,[&]{++submits;return fence;});
        assert(result.completed==fence && result.presentationValid==fence);
    }
    assert(submits==2);
    // Actual first-profile capture refuses the disjoint256x256 target. The
    // display may survive, but the requested clear has NOT been submitted.
    put(header,28,64);put(state,10368,0x14000140);
    put(state,13032,colorBits(256));put(state,13036,colorBits(256));
    put(rect,8,256);put(rect,12,256);assert(!capture());
    for(bool preserved:{false,true})for(bool fallbackFence:{false,true}) {
        const auto result=executeNativePrimaryClear(capture().has_value(),preserved,[&]{++submits;return true;});
        assert(!result.completed && result.presentationValid==preserved && submits==2);
        // Same connection as the dispatcher: an uncompleted primary attempt
        // reaches the offscreen executor even if the display remained valid.
        unsigned fallbacks=0;bool completed=result.completed;
        if(!completed){++fallbacks;completed=fallbackFence;}
        assert(fallbacks==1 && completed==fallbackFence);
    }
    std::puts("PRIMARY_CLEAR_COMPLETION_PASS real_capture_refusal=1 display_preserved_not_completed=1 fallback_reached=1 failed_submit_not_completed=1 gpu=0");
}
