#include "xenos_mrt_blend.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    const auto replace=decodeXenosMrtBlend(0x10001),accumulate=decodeXenosMrtBlend(0x7010001),fade=decodeXenosMrtBlend(0x7000706);
    assert(replace && accumulate && fade && !replace->enabled && accumulate->enabled && fade->enabled);
    assert(replace->colorSource==1 && replace->colorDestination==0 && replace->alphaSource==1 && replace->alphaDestination==0);
    assert(accumulate->colorSource==1 && accumulate->colorDestination==0 && accumulate->alphaSource==1 && accumulate->alphaDestination==7);
    assert(fade->colorSource==6 && fade->colorDestination==7 && fade->alphaSource==0 && fade->alphaDestination==7);
    for(unsigned mask=0;mask<16;++mask) {
        assert(!replace->readsDestination(mask));assert(accumulate->readsDestination(mask)==bool(mask&8));
        assert(fade->readsDestination(mask)==bool(mask));
    }
    for(unsigned word:{0u,0x10002u,0x10021u,0x87010001u,0x7000707u,0xFFFFFFFFu})assert(!decodeXenosMrtBlend(word));
    std::puts("XENOS_MRT_BLEND_PASS controls=3 channel_masks=16 unknowns_rejected=1");
}
