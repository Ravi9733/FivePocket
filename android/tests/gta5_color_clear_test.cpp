#include "gta5_color_clear.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    std::vector<std::uint8_t> state(13128),surface(32),rect(16),rgba(16);
    const auto put=[](auto& b,unsigned at,std::uint32_t value) {
        for(unsigned i=0;i<4;++i)b[at+i]=static_cast<std::uint8_t>(value>>(24-i*8));
    };
    put(state,10368,0x14000500);put(state,10372,0xC02D0);put(state,12816,0x5000);
    put(surface,28,0xC02D0);
    put(state,13032,colorBits(1280));put(state,13036,colorBits(720));
    put(rect,8,1280);put(rect,12,720);put(rgba,12,colorBits(1));
    const Gta5ResourceReader heap=[&](auto a,auto n)->std::optional<std::vector<std::uint8_t>> {
        if(a==0x1000 && n==state.size())return state;
        if(a==0x5000 && n==surface.size())return surface;return std::nullopt;
    };
    const Gta5ResourceReader argument=[&](auto a,auto n)->std::optional<std::vector<std::uint8_t>> {
        if(n!=16)return std::nullopt;
        if(a==0x2000)return rect;if(a==0x3000)return rgba;return std::nullopt;
    };
    const auto capture=[&](unsigned flags=15){return captureGta5ColorClear(0x1000,flags,0x2000,0x3000,heap,argument);};
    auto result=capture();assert(result && result->target==0x5000 && result->packedColor==0xC0000000);
    assert((result->color==std::array<float,4>{0,0,0,1}));
    put(state,10372,0x302D0);assert(capture()); // Temporary clear shadow isn't resource format.
    put(surface,28,0x302D0);assert(capture()); // Canonical storage format3 aliases view12.
    put(surface,28,0x202D0);assert(!capture());put(surface,28,0xC02D0);
    state[11068]=0x20;assert(capture()); // Tile cache differs: direct path.
    put(state,13104,0x5000);assert(!capture()); // Every bound target cached: binned.
    state[11068]=0x10;assert(!capture());
    state[11068]=0;assert(!capture(0x11));assert(!capture(2));
    put(state,12820,0x6000);assert(!capture());assert(capture(1));put(state,12820,0);
    put(rect,8,1279);assert(!capture());put(rect,8,1280);
    put(state,12288,1);put(state,13060,1280);put(state,13064,720);assert(capture());
    put(state,13052,1);assert(!capture());put(state,13052,0);
    put(rgba,0,0x7FC00000);assert(!capture());put(rgba,0,0);
    assert(!captureGta5ColorClear(0x1000,15,0,0x3000,heap,argument));
    std::puts("GTA5_COLOR_CLEAR: color, flags, viewport/scissor and conditional binning PASS");
}
