#include "gta5_bound_surface_capture.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    std::vector<std::uint8_t> state(13860,0);
    auto word=[&](unsigned at,std::uint32_t value){for(unsigned i=0;i<4;++i)state[at+i]=value>>(24-8*i);};
    unsigned calls=0;
    Gta5ResourceReader heap=[&](std::uint32_t address,std::uint32_t size)->std::optional<std::vector<std::uint8_t>> {
        ++calls;assert(size==32);
        if(address==0x1000)return std::vector<std::uint8_t>(32,0xA5);
        if(address==0x2000)return std::vector<std::uint8_t>(31,0xBB);
        if(address==0x3000)return std::nullopt;
        if(address==0x4000)return std::vector<std::uint8_t>(32,0xCC);
        if(address==0x5000)return std::nullopt;
        assert(false);return std::nullopt;
    };
    auto empty=captureGta5BoundSurfaces(state,heap);assert(empty && calls==0);
    for(const auto& s:empty->surfaces)assert(!s.object && !s.tileCacheObject && !s.header);
    word(12816,0x1000);word(13104,0x1000);
    word(12820,0x2000);word(13108,0x4000);
    word(12824,0x3000);word(12828,0xFFFFFFF0);word(12832,0x1000);word(13120,0x5000);
    const auto before=state;auto actual=captureGta5BoundSurfaces(state,heap);
    assert(actual && calls==7 && state==before);
    const auto& rows=actual->surfaces;
    assert(rows[0].header && *rows[0].header==std::vector<std::uint8_t>(32,0xA5));
    assert(rows[0].object==rows[0].tileCacheObject);
    assert(rows[0].tileCacheHeader==rows[0].header);
    assert(rows[1].tileCacheHeader==std::vector<std::uint8_t>(32,0xCC));
    assert(!rows[4].tileCacheHeader);
    assert(rows[1].object==0x2000 && rows[1].tileCacheObject==0x4000 && !rows[1].header);
    assert(!rows[2].header && !rows[3].header && rows[3].object==0xFFFFFFF0);
    assert(rows[4].header && rows[4].tileCacheObject==0x5000);
    assert(!captureGta5BoundSurfaces(state,{}));
    state.resize(13123);assert(!captureGta5BoundSurfaces(state,heap));assert(calls==7);
    state.assign(13860,0);word(12816,0xABF01000);
    unsigned graphicsCalls=0;
    const Gta5ResourceReader graphics=[&](unsigned at,unsigned n)->std::optional<std::vector<std::uint8_t>> {
        ++graphicsCalls;assert(at==0xABF01000 && n==40);return std::vector<std::uint8_t>(40,0xA4);
    };
    auto full=captureGta5BoundSurfaces(state,heap,graphics,40);
    assert(full && full->surfaces[0].header->size()==40 && graphicsCalls==1 && calls==7);
    assert(!captureGta5BoundSurfaces(state,heap,graphics,36));
    assert(!captureGta5BoundSurfaces(state,heap,{},40)->surfaces[0].header && calls==7);
    state.assign(13860,0);word(12816,0x640AF9E0);word(13104,0x1000);
    unsigned argumentCalls=0,physicalCalls=0;
    const Gta5ResourceReader heap40=[](auto at,auto n)->std::optional<std::vector<std::uint8_t>> {
        assert(at==0x1000 && n==40);return std::vector<std::uint8_t>(40,0xCC);
    };
    const Gta5ResourceReader argument=[&](auto at,auto n)->std::optional<std::vector<std::uint8_t>> {
        ++argumentCalls;assert(at==0x640AF9E0 && n==40);return std::vector<std::uint8_t>(40,0xAA);
    };
    const Gta5ResourceReader noPhysical=[&](auto at,auto n)->std::optional<std::vector<std::uint8_t>> {
        ++physicalCalls;assert(at==0x640AF9E0 && n==40);return std::nullopt;
    };
    auto stack=captureGta5ArgumentBoundSurfaces(state,heap40,noPhysical,argument);assert(stack);
    assert(stack->surfaces[0].header==std::vector<std::uint8_t>(40,0xAA));
    assert(stack->surfaces[0].tileCacheHeader==std::vector<std::uint8_t>(40,0xCC));
    assert(argumentCalls==1 && physicalCalls==0);
    auto unavailable=captureGta5ArgumentBoundSurfaces(state,heap40,noPhysical,{});assert(unavailable);
    assert(!unavailable->surfaces[0].header && unavailable->surfaces[0].tileCacheHeader);
    assert(physicalCalls==1); // Never substitutes the readable cache for the stack.
    std::puts("ARGUMENT_BOUND_SURFACE_PASS current_stack=1 cache_independent=1 unavailable_refused=1 graphics_not_widened=1");
    std::puts("BOUND_SURFACE_CAPTURE_PASS owned=2 short=1 unavailable=1 overflow=1 no_mutation=1");
}
