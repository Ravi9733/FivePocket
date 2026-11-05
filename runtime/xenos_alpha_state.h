#pragma once
// Attempt v85: the Xenos alpha test, alpha-to-mask and depth/stencil write state as the generic renderer
// maps them (pure functions; host-tested against hand-computed register values).
//
// RB_COLORCONTROL (0x2202): bits 0-2 alpha_func (0 NEVER, 1 LESS, 2 EQUAL, 3 LEQUAL, 4 GREATER, 5 NOTEQUAL,
// 6 GEQUAL, 7 ALWAYS: a less/equal/greater bit mask), bit 3 alpha_test_enable, bit 4 alpha_to_mask_enable,
// bits 24-31 alpha_to_mask_offset0..3 (2 bits each, pixels (0,0), (0,1), (1,0), (1,1) of a quad).
// RB_ALPHA_REF (0x210E) is the reference as a float. The test reads oC0.w and applies only when the pixel
// shader exports colour 0.
//
// The translated shaders kill on x < 0 (spirv_alpha_test.h): x = d with d = alpha - threshold for the
// pack's own module, x = S*d + E*|d| + C for a generalized one. Per function:
//   GEQUAL   threshold ref                 (pack module)
//   GREATER  threshold nextUp(ref)         (pack module; alpha > ref <=> alpha >= next float above ref)
//   NEVER    threshold +inf                (pack module; kills every finite alpha)
//   LESS     S=-1 threshold nextDown(ref)  (generalized)
//   LEQUAL   S=-1 threshold ref            (generalized)
//   EQUAL    E=-1 threshold ref            (generalized: -|d| >= 0 <=> alpha == ref)
//   NOTEQUAL E=1 C=-FLT_MIN threshold ref  (generalized: |d| >= FLT_MIN <=> alpha != ref for normal floats)
// nextUp/nextDown skip denormals (GPUs flush them: alpha - FLT_MIN_denormal would read as alpha - 0).
//
// Alpha to mask with one sample per pixel (GTA V's scene targets are not multisampled): a sample is
// covered when alpha >= 1 - offset/4 (Xenia's reading of the Adreno 200 measurements; offset per quad
// pixel). The pack shaders have no pixel position here, so the four thresholds are averaged:
// alpha >= 1 - (o0+o1+o2+o3)/16 (0.5 for the undithered 2,2,2,2; 0.625 for the dithered 3,1,0,2 GTA V's
// draw 902 sets), combined with a GEQUAL/GREATER test by the larger threshold. With LESS/EQUAL/LEQUAL/
// NOTEQUAL it is dropped (counted: the one formula cannot express the conjunction).
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <limits>

namespace recomp::gpu::generic::state {

// Shared-constant words (4-byte units) of the generalized test's S, E, C (byte 384, 388, 392). 77 (byte 308)
// is g_AlphaThreshold; 64-80 are used by the translator; 96-98 are otherwise unread.
constexpr unsigned AlphaWordS=96,AlphaWordE=97,AlphaWordC=98,AlphaWordThreshold=77;

inline float nextUpNormal(float x) {
    float y=std::nextafter(x,std::numeric_limits<float>::infinity());
    if(y>0 && y<FLT_MIN)y=FLT_MIN;           // 0 -> smallest normal
    else if(y<0 && y>-FLT_MIN)y=0.0f;       // -FLT_MIN -> 0
    return y;
}
inline float nextDownNormal(float x) {
    float y=std::nextafter(x,-std::numeric_limits<float>::infinity());
    if(y<0 && y>-FLT_MIN)y=-FLT_MIN;
    else if(y>0 && y<FLT_MIN)y=0.0f;
    return y;
}

enum class AlphaMode : unsigned {Off=0,Threshold=1,Generic=2};
struct AlphaTest {
    AlphaMode mode=AlphaMode::Off;   // Off: spec bit clear; Threshold: pack module; Generic: generalized module
    float threshold=0,s=1,e=0,c=0;   // shared words 77, 96, 97, 98
    unsigned func=7;                 // the function applied (7 when only alpha-to-mask or nothing)
    bool a2c=false;                  // alpha to mask folded into the threshold
    bool a2cDropped=false;           // alpha to mask requested but not expressible with this function
    float a2cThreshold=0;
    bool kills()const{return mode!=AlphaMode::Off;}
};
inline float alphaToMaskThreshold(std::uint32_t colorControl) {
    const unsigned sum=((colorControl>>24)&3)+((colorControl>>26)&3)+((colorControl>>28)&3)+((colorControl>>30)&3);
    return 1.0f-float(sum)/16.0f;
}
// legacy: v71 (every enabled function as GEQUAL on ref, alpha to mask ignored, regardless of oC0).
// genericAvailable: the draw's pixel shader has a generalized module. a2cOn: alpha to mask is mapped.
inline AlphaTest alphaTestState(std::uint32_t colorControl,float ref,bool exportsColor0,bool genericAvailable,bool legacy,bool a2cOn=true) {
    AlphaTest a;
    const unsigned func=colorControl&7;const bool test=(colorControl&8) && func!=7;
    if(legacy) {
        if(test){a.mode=AlphaMode::Threshold;a.threshold=ref;a.func=6;}
        return a;
    }
    if(!exportsColor0)return a; // Xenos: the test and alpha to mask read oC0.w; without it neither applies
    const bool mask=a2cOn && (colorControl&16);
    if(mask)a.a2cThreshold=alphaToMaskThreshold(colorControl);
    if(!test) {
        if(mask){a.mode=AlphaMode::Threshold;a.threshold=a.a2cThreshold;a.a2c=true;}
        return a;
    }
    a.func=func;
    switch(func) {
    case 6: case 4: case 0: {
        a.mode=AlphaMode::Threshold;
        a.threshold=func==6?ref:func==4?nextUpNormal(ref):std::numeric_limits<float>::infinity();
        if(mask && func!=0){a.threshold=std::max(a.threshold,a.a2cThreshold);a.a2c=true;}
        return a;
    }
    default: break;
    }
    a.a2cDropped=mask;
    if(!genericAvailable) { // v71's reading of the function: alpha >= ref
        a.mode=AlphaMode::Threshold;a.threshold=ref;return a;
    }
    a.mode=AlphaMode::Generic;a.threshold=ref;a.s=0;a.e=0;a.c=0;
    switch(func) {
    case 1: a.s=-1;a.threshold=nextDownNormal(ref);break;   // LESS
    case 3: a.s=-1;break;                                     // LEQUAL
    case 2: a.e=-1;break;                                     // EQUAL
    case 5: a.e=1;a.c=-FLT_MIN;break;                         // NOTEQUAL
    }
    return a;
}

// Whether a draw can write depth or stencil (then a discarding pixel shader needs late tests).
// RB_DEPTHCONTROL (0x2200): bit 0 stencil_enable, 1 z_enable, 2 z_write_enable, 7 backface_enable,
// bits 11-19 front stencil fail/zpass/zfail ops, 23-31 back ops. RB_STENCILREFMASK(_BF) bits 16-23 write mask.
inline bool writesDepthStencil(std::uint32_t depthControl,std::uint32_t refMask,std::uint32_t refMaskBack,bool depthBound,bool backSeparate) {
    if(!depthBound)return false;
    if((depthControl&2) && (depthControl&4))return true;
    if(!(depthControl&1))return false;
    const bool front=((refMask>>16)&0xFF) && ((depthControl>>11)&0x1FF);
    const bool back=backSeparate?(((refMaskBack>>16)&0xFF) && ((depthControl>>23)&0x1FF)):front;
    return front || back;
}

} // namespace recomp::gpu::generic::state
