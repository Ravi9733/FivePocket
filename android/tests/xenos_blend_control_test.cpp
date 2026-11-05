#include "xenos_blend_control.h"
#include "xenos_mrt_blend.h"
#include <array>
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    unsigned fields=0;
    for(unsigned shift:{0u,8u,16u,24u})for(unsigned factor=0;factor<32;++factor) {
        const auto word=(0x10001u&~(31u<<shift))|(factor<<shift);
        const auto b=decodeXenosBlendControl(word);
        const bool valid=factor==0 || factor==1 || (factor>=4 && factor<=16);
        assert(bool(b)==valid);
        if(b)assert((shift==0?b->colorSource:shift==8?b->colorDestination:shift==16?b->alphaSource:b->alphaDestination)==factor);
        assert(bool(planNativeRegisterBlend(word))==(valid && !(factor>=12 && factor<=15)));++fields;
    }
    for(unsigned shift:{5u,21u})for(unsigned op=0;op<8;++op) {
        const auto b=decodeXenosBlendControl(0x10001u|(op<<shift));assert(bool(b)==(op<=4));
        if(b)assert((shift==5?b->colorOperation:b->alphaOperation)==op);++fields;
    }
    for(unsigned bit:{13u,14u,15u,29u,30u,31u})assert(!decodeXenosBlendControl(0x10001u|(1u<<bit)));
    for(unsigned word:{0x10001u,0x7010001u,0x7000706u}) {
        const auto a=decodeXenosMrtBlend(word);const auto b=decodeXenosBlendControl(word);assert(a && b);
        assert(a->colorSource==b->colorSource && a->colorDestination==b->colorDestination &&
            a->alphaSource==b->alphaSource && a->alphaDestination==b->alphaDestination && a->enabled==b->enabled);
        for(unsigned mask=0;mask<16;++mask)assert(a->readsDestination(mask)==b->readsDestination(mask));
    }
    const auto live=planNativeRegisterBlend(0x07060706);assert(live && live->enabled);
    assert(live->colorSource==6 && live->colorDestination==7 && live->colorOperation==0 &&
        live->alphaSource==6 && live->alphaDestination==7 && live->alphaOperation==0);
    // This is deliberately NOT the legacy RGB-alpha-blend / alpha-replace mode.
    const auto legacy=planNativeRegisterBlend(0x00010706);assert(legacy && legacy->alphaSource==1 && legacy->alphaDestination==0);
    const float sourceAlpha=.25f,destinationAlpha=.5f;
    assert(sourceAlpha*sourceAlpha+destinationAlpha*(1-sourceAlpha)==.4375f);
    assert(.4375f!=sourceAlpha);
    for(unsigned mask=0;mask<16;++mask)assert(live->readsDestination(mask)==bool(mask));
    // A ZERO destination term may still read destination through its source
    // factor; MIN/MAX read it independently of blend factors.
    assert(planNativeRegisterBlend(0x1000A)->readsDestination(7));
    assert(planNativeRegisterBlend(0x10040)->readsDestination(7));
    assert(!planNativeRegisterBlend(0x10001)->readsDestination(15));
    for(unsigned f=0;f<32;++f) {
        const unsigned expected=f==4?6:f==5?7:f==8?10:f==9?11:f==12?14:f==13?15:f==16?1:f;
        assert(xenosAlphaBlendFactor(f)==expected);
    }
    assert(fields==144);
    std::puts("XENOS_BLEND_CONTROL_PASS field_cases=144 reserved_rejected=6 mrt_legacy=3 alpha_independent=1 missing_constants_rejected=1 draw_admission=0");
}
