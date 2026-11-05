#include "native_fragment7e3_depth.h"
#include <cassert>
#include <cstdio>
#include <limits>
using namespace recomp::gpu;
static unsigned packed(float r,float g,float b,float a) {
    unsigned word=0;assert(packColor7e3({colorBits(r),colorBits(g),colorBits(b),colorBits(a)},word));return word;
}
int main() {
    const unsigned old=packed(.25f,.5f,1.f,1.f),half=*encodeExactXenosFloat24(.5f);
    const auto checkUnchanged=[&](auto r,NativeFragmentDisposition d,unsigned depth) {
        assert(r && r->disposition==d && r->color==old && r->depthStencil==depth);
    };
    for(float alpha:{-1.f,-0.f,0.f})
        checkUnchanged(mergeNativeFragment7e3Depth({{1,1,1,alpha},1.f},old,0xA5),
            NativeFragmentDisposition::AlphaRejected,0xA5);
    checkUnchanged(mergeNativeFragment7e3Depth({{1,1,1,1},.25f},old,(half<<8)|0xA5),
        NativeFragmentDisposition::DepthRejected,(half<<8)|0xA5);
    for(unsigned stencil=0;stencil<256;++stencil) {
        const auto equal=mergeNativeFragment7e3Depth({{.5f,1.f,2.f,1.f},.5f},old,(half<<8)|stencil);
        assert(equal && equal->disposition==NativeFragmentDisposition::Written &&
            equal->color==packed(.5f,1.f,2.f,1.f) && equal->depthStencil==((half<<8)|stencil));
        // A float32 just above the previous20e4 cell must NOT pass against
        // the NEXT20e4 destination cell before being quantized.
        const float above=std::nextafter(.5f,1.f);
        checkUnchanged(mergeNativeFragment7e3Depth({{1,1,1,1},above},old,((half+1)<<8)|stencil),
            NativeFragmentDisposition::DepthRejected,((half+1)<<8)|stencil);
    }
    const auto blend=mergeNativeFragment7e3Depth({{.5f,1.f,2.f,.5f},.75f},old,0xA5);
    assert(blend && blend->color==packed(.375f,.75f,1.5f,.75f) &&
        blend->depthStencil==((*encodeExactXenosFloat24(.75f)<<8)|0xA5));
    const auto highAlpha=mergeNativeFragment7e3Depth({{.5f,1,2,4},1},old,0xFF);
    assert(highAlpha && highAlpha->color==packed(.5f,1,2,1));
    // A later farther fragment must see the earlier depth write; a nearer
    // one must blend against the quantized earlier color, not original color.
    const auto farther=mergeNativeFragment7e3Depth({{8,8,8,1},.5f},blend->color,blend->depthStencil);
    assert(farther && farther->disposition==NativeFragmentDisposition::DepthRejected && farther->color==blend->color);
    const auto nearer=mergeNativeFragment7e3Depth({{.25f,.5f,1.f,.5f},1},blend->color,blend->depthStencil);
    assert(nearer && nearer->color==packed(.3125f,.625f,1.25f,7.f/12.f) && (nearer->depthStencil&255)==0xA5);
    // All24-bit destination codes are valid (some decode >1); no clamp of the
    // existing owner is allowed to turn such a depth rejection into a pass.
    checkUnchanged(mergeNativeFragment7e3Depth({{1,1,1,1},1},old,0xFFFFFFFF),
        NativeFragmentDisposition::DepthRejected,0xFFFFFFFF);
    for(float bad:{-1.f,2.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
        assert(!mergeNativeFragment7e3Depth({{1,1,1,1},bad},old,0));
    for(unsigned lane=0;lane<4;++lane)for(float bad:{std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        NativeFragment7e3Depth f{{1,1,1,1},.5f};f.color[lane]=bad;
        assert(!mergeNativeFragment7e3Depth(f,old,0));
    }
    const auto tiny=mergeNativeFragment7e3Depth({{1,1,1,1},0x1p-34f},old,0x5A);
    assert(tiny && tiny->depthStencil==0x15A);
    // Source alpha is tested before its2-bit packing: tiny positive alpha
    // still writes depth even if the final stored alpha is zero.
    const auto tinyAlpha=mergeNativeFragment7e3Depth({{0,0,0,0x1p-20f},.5f},0,0x9F);
    assert(tinyAlpha && tinyAlpha->disposition==NativeFragmentDisposition::Written &&
        tinyAlpha->depthStencil==((half<<8)|0x9F));
    std::puts("FRAGMENT7E3_DEPTH_PASS alpha_order=1 quantized_ge=1 ordered_writes=1 stencil_preserved=256 invalid_inputs_rejected=1 live_draw=0");
}
