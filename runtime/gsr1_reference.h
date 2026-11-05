#pragma once
//============================================================================================================
//
//
//                  Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
//                              SPDX-License-Identifier: BSD-3-Clause
//
//============================================================================================================
// v460 (agent gsr1): what the presenter's Snapdragon GSR 1 scaler (files/private-trace/diag-v460-scale-gsr1) must write, as
// plain C++ for its audit (RENDER_DIRECT_AUDIT ... gsr=N) and for the host tests. pixel() is a PORT of Qualcomm's SGSR 1
// fragment shaders (sgsr/v1/include/glsl/sgsr1_shader_mobile.frag; with edgeDirection, sgsr1_shader_mobile_edge_direction.frag
// with UseEdgeDirection), BSD-3-Clause (notice above; full text: THIRD-PARTY-NOTICES.md of the gsr1 lane; in the repository
// third_party/snapdragon-gsr/LICENSE and the app's assets/licenses/Qualcomm-Snapdragon-GSR1-BSD-3-Clause.txt, added at the merge), through
// our Vulkan form of them, direct_present_gsr.glsl: the same fp32 operations in the same order, written per component. The
// twelve taps are fetched by integer position instead of through the shader's four gathers, and the base colour is the
// Vulkan bilinear formula in fp32: the host test (tests/gsr1_test.cpp) runs direct_present_gsr.glsl itself through a
// GLSL-as-C++ shim with Vulkan gather / bilinear rules and requires bit-identical fp32 results, so it checks that plumbing.
// Choices where GLSL leaves freedom (the device audit decides whether Adreno agrees): min / max return the non-NaN operand,
// division is IEEE, no fused multiply-add (the compiled SPIR-V does contain Fma: covered by the audit's tolerance), UNORM8
// texels convert as k / 255, results as round(v * 255) with NaN -> 0, inversesqrt = 1 / sqrt, every operation in fp32 (the
// shader is `mediump`: Adreno may run it in 16-bit floats; pixel<F> takes the float type so the host test can measure that).
//
// The device audit cannot be bit-exact (16-bit floats, fixed-point bilinear weights, fused multiply-adds), and GSR has one
// discontinuity: a pixel is sharpened only when its edge vote exceeds the threshold, so a pixel whose vote is within
// FlipMargin of the threshold may legitimately come out either way. Expected bytes therefore carry a range (lo, hi):
//  - not sharpened, vote clear of the threshold: the bilinear colour, +/- 1;
//  - sharpened, vote clear of the threshold: the reference, +/- (SoftTolerance + 1) (counted as `soft` beyond +/- 1);
//  - vote within FlipMargin of the threshold, or a kernel whose weight sum is small against its terms (the quotient
//    aWY.y / aWY.x is then ill-conditioned): anything the shader can produce there, base - 23 - 1 .. base + 23 + 1 (the
//    shader clamps its correction to +/- 23/255; counted as `soft`).
// Alpha must match exactly (the border's too). The border's colour bytes (expected 0) follow the rule of every other colour
// byte with no range: within 1 (v458's FSR comparison has the same rule). Anything else is a mismatch.
#include "direct_present_reference.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace recomp::gpu::direct::gsr1 {
// Qualcomm's defaults (README: threshold 8.0/255.0 for mobile, 4.0/255.0 suggested for VR; sharpness 2.0, range [1.0, 2.0]).
constexpr float DefaultThreshold255=8.0f,DefaultSharpness=2.0f;
constexpr float MinThreshold255=1.0f,MaxThreshold255=64.0f,MinSharpness=1.0f,MaxSharpness=2.0f;
// The audit's rules (see above). SoftTolerance: host estimate of 16-bit-float + fused arithmetic on sharpened pixels
// (tests/gsr1_test.cpp prints the measured distribution); FlipMargin in units of 1/255; IllConditioned: |sum w| / sum |w|.
constexpr int SoftTolerance=3;
constexpr float FlipMargin255=1.0f,IllConditioned=1.0f/16.0f;
constexpr int DeltaLimit=23;

// The push constants of direct_present_gsr*.frag after the rect: viewport (Qualcomm's ViewportInfo[0]) and params.
struct Constants {
    std::uint32_t width=0,height=0;   // the image rect
    float viewport[4]{};              // 1/1280, 1/720, 1280, 720
    float params[4]{};                // 1/width, 1/height, edge threshold, edge sharpness
    float threshold255=DefaultThreshold255,sharpness=DefaultSharpness;
    bool edgeDirection=false;
    std::uint32_t srcWidth=SourceWidth,srcHeight=SourceHeight; // (lane fsr-stage-b: the picture GSR reads; the viewport's size)
};
inline Constants constants(std::uint32_t width,std::uint32_t height,float threshold255=DefaultThreshold255,
                           float sharpness=DefaultSharpness,bool edgeDirection=false,std::uint32_t srcWidth=SourceWidth,
                           std::uint32_t srcHeight=SourceHeight) noexcept {
    Constants c;c.width=width;c.height=height;c.threshold255=threshold255;c.sharpness=sharpness;c.edgeDirection=edgeDirection;
    c.srcWidth=srcWidth;c.srcHeight=srcHeight; // (lane fsr-stage-b: Qualcomm's ViewportInfo[0] = 1 / input size, input size)
    c.viewport[0]=1.0f/float(srcWidth);c.viewport[1]=1.0f/float(srcHeight);
    c.viewport[2]=float(srcWidth);c.viewport[3]=float(srcHeight);
    c.params[0]=1.0f/float(width);c.params[1]=1.0f/float(height);
#ifdef RECOMP_GSR1_MUTANT_THRESHOLD
    c.params[2]=threshold255;
#else
    c.params[2]=threshold255/255.0f;
#endif
    c.params[3]=sharpness;
    return c;
}
// A tuning file's value, or the default when it is absent / unreadable / outside [lo, hi].
inline float tuned(bool have,float value,float lo,float hi,float fallback) noexcept {
    return have && value>=lo && value<=hi?value:fallback;
}

template<class F> inline F absF(F x) noexcept {return F(std::fabs(float(x)));}
template<class F> inline F minF(F a,F b) noexcept {return F(std::fmin(float(a),float(b)));} // GLSL min as minNum
template<class F> inline F maxF(F a,F b) noexcept {return F(std::fmax(float(a),float(b)));}
template<class F> inline F clampF(F x,F lo,F hi) noexcept {return minF(maxF(x,lo),hi);}
// A mapped (tables applied) RGBA8 picture's texel as the shader samples it: clamped to the edge, k / 255.
inline float texel(const std::uint8_t* rgba,int width,int height,int x,int y,int c) noexcept { // (lane fsr-stage-b: any picture size)
    x=std::clamp(x,0,width-1);y=std::clamp(y,0,height-1);
    return float(rgba[(std::size_t(y)*unsigned(width)+unsigned(x))*4+unsigned(c)])/255.0f;
}
inline float texel(const std::uint8_t* rgba,int x,int y,int c) noexcept {return texel(rgba,int(SourceWidth),int(SourceHeight),x,y,c);}
// fastLanczos2
template<class F> inline F fastLanczos2(F x) noexcept {
#pragma clang fp contract(off)
    F wA=x-F(4.0f);
    F wB=x*wA-wA;
    wA*=wA;
    return wB*wA;
}
// weightY, accumulated: aW += w, aY += w * c. data = std (mobile) or std, dir.x, dir.y (edge direction).
template<class F> inline void weightY(F& aW,F& aY,F& aAbs,F dx,F dy,F c,const F data[3],bool edgeDirection) noexcept {
#pragma clang fp contract(off)
    F x;
    if(edgeDirection) {
        const F edgeDis=((dx*data[2])+(dy*data[1]));
        x=(((dx*dx)+(dy*dy))+((edgeDis*edgeDis)*((clampF(((c*c)*data[0]),F(0.0f),F(1.0f))*F(0.7f))+F(-1.0f))));
    } else {
#ifdef RECOMP_GSR1_MUTANT_KERNEL
        x=((dx*dx)+(dy*dy))*F(0.5f)+clampF(absF(c)*data[0],F(0.0f),F(1.0f));
#else
        x=((dx*dx)+(dy*dy))*F(0.55f)+clampF(absF(c)*data[0],F(0.0f),F(1.0f));
#endif
    }
    const F w=fastLanczos2(x);
    aW+=w;aY+=w*c;aAbs+=absF(w);
}
// One output pixel. r, g, b: the shader's result in [0, 1]; base: the bilinear colour before the correction; sharpened: the
// edge branch ran; vote: the edge vote; conditioning: |sum w| / sum |w| of the kernel (1 when not sharpened).
struct Pixel {float r=0,g=0,b=0,base[3]{},vote=0,conditioning=1;bool sharpened=false;};
// directGsr(q) of direct_present_gsr.glsl at output pixel (x, y) of the image rect, from the 1280x720 mapped picture.
// F: the type of the shader's `mediump` values (float = the reference; the host test also runs _Float16).
// weightBits (host tests only; 0 = exact): the bilinear weights rounded to that many fractional bits, as GPU samplers do.
template<class F=float> inline Pixel pixel(const std::uint8_t* mapped,const Constants& con,int x,int y,unsigned weightBits=0) noexcept {
#pragma clang fp contract(off)
    constexpr int Mode=1; // OperationMode RGBA: the green channel
    // highp vec2 texCoord = (vec2(q) + 0.5) * pc.params.xy
    const float tcX=(float(x)+0.5f)*con.params[0],tcY=(float(y)+0.5f)*con.params[1];
    const F edgeThreshold=F(con.params[2]),edgeSharpness=F(con.params[3]);
    // vec4 color = textureLod(source, texCoord, 0): LINEAR, CLAMP_TO_EDGE (Vulkan: u - 0.5, the 2x2 at its floor, weights its fraction)
    const int W=int(con.srcWidth),H=int(con.srcHeight); // (lane fsr-stage-b: 1280x720 unless the constants say otherwise)
    const float su=tcX*float(con.srcWidth)-0.5f,sv=tcY*float(con.srcHeight)-0.5f;
    const float fu=std::floor(su),fv=std::floor(sv);
    float wa=su-fu,wb=sv-fv;
    if(weightBits){const float q=float(1u<<weightBits);wa=std::floor(wa*q+0.5f)/q;wb=std::floor(wb*q+0.5f)/q;}
    const int bi=int(fu),bj=int(fv);
    F color[4];
    for(int c=0;c<3;++c) {
        const float t00=texel(mapped,W,H,bi,bj,c),t10=texel(mapped,W,H,bi+1,bj,c),t01=texel(mapped,W,H,bi,bj+1,c),t11=texel(mapped,W,H,bi+1,bj+1,c);
#ifdef RECOMP_GSR1_MUTANT_BILINEAR
        color[c]=F((1.0f-wa)*(1.0f-wb)*t00+wa*(1.0f-wb)*t10+(1.0f-wa)*wb*t01+wa*wa*t11);
#else
        color[c]=F((1.0f-wa)*(1.0f-wb)*t00+wa*(1.0f-wb)*t10+(1.0f-wa)*wb*t01+wa*wb*t11);
#endif
    }
    Pixel out;
    for(int c=0;c<3;++c)out.base[c]=float(color[c]);
    // highp: imgCoord = texCoord * viewport.zw + (-0.5, 0.5); imgCoordPixel = floor(imgCoord); pl = imgCoord - imgCoordPixel
    const float icX=((tcX*con.viewport[2])+(-0.5f)),icY=((tcY*con.viewport[3])+0.5f);
    const float ipX=std::floor(icX),ipY=std::floor(icY);
    const F plX=F(icX+(-ipX)),plY=F(icY+(-ipY));
    const int px=int(ipX),py=int(ipY);
    // The gathers at the texel corners (px, py), (px + 2, py), (px + 1, py - 1), (px + 1, py + 1) return, as x y z w, the texels
    // (i0, j1) (i1, j1) (i1, j0) (i0, j0) of the 2x2 whose bottom-right texel is the corner's: by integer position here.
    const auto tap=[&](int ox,int oy){return F(texel(mapped,W,H,px+ox,py+oy,Mode));};
    F left[4]{tap(-1,0),tap(0,0),tap(0,-1),tap(-1,-1)};
    const F vote=absF(left[2]-left[1])+absF(color[Mode]-left[1])+absF(color[Mode]-left[2]);
    out.vote=float(vote);
    if(vote>edgeThreshold) {
        out.sharpened=true;
        F right[4]{tap(1,0),tap(2,0),tap(2,-1),tap(1,-1)};
#ifdef RECOMP_GSR1_MUTANT_UPDOWN
        F upDown[4]{tap(1,-2),tap(0,-2),tap(1,1),tap(0,1)}; // upGather.z, .w (exchanged)
#else
        F upDown[4]{tap(0,-2),tap(1,-2),tap(1,1),tap(0,1)}; // upGather.w, upGather.z, downGather.y, downGather.x
#endif
        const F mean=(left[1]+left[2]+right[0]+right[3])*F(0.25f);
        for(int k=0;k<4;++k)left[k]=left[k]-mean;
        for(int k=0;k<4;++k)right[k]=right[k]-mean;
        for(int k=0;k<4;++k)upDown[k]=upDown[k]-mean;
        color[3]=color[Mode]-mean;
        const F sum=(((((absF(left[0])+absF(left[1]))+absF(left[2]))+absF(left[3]))+(((absF(right[0])+absF(right[1]))+absF(right[2]))+absF(right[3])))+
                     (((absF(upDown[0])+absF(upDown[1]))+absF(upDown[2]))+absF(upDown[3])));
        F data[3]{F(0.0f),F(0.0f),F(0.0f)};
        if(con.edgeDirection) {
            const F sumMean=F(1.014185e+01f)/sum;
            data[0]=(sumMean*sumMean);
            // edgeDirection(left, right)
            const F RxLz=(right[0]+(-left[2]));
            const F RwLy=(right[3]+(-left[1]));
            const F deltaX=(RxLz+RwLy);
            const F deltaY=(RxLz+(-RwLy));
            const F lengthInv=F(1.0f/std::sqrt(float((deltaX*deltaX+F(3.075740e-05f))+(deltaY*deltaY))));
            data[1]=(deltaX*lengthInv);
            data[2]=(deltaY*lengthInv);
        } else data[0]=F(2.181818f)/sum;
        F aW=F(0.0f),aY=F(0.0f),aAbs=F(0.0f);
        const bool e=con.edgeDirection;
        weightY(aW,aY,aAbs,plX,plY+F(1.0f),upDown[0],data,e);
        weightY(aW,aY,aAbs,plX-F(1.0f),plY+F(1.0f),upDown[1],data,e);
        weightY(aW,aY,aAbs,plX-F(1.0f),plY-F(2.0f),upDown[2],data,e);
        weightY(aW,aY,aAbs,plX,plY-F(2.0f),upDown[3],data,e);
        weightY(aW,aY,aAbs,plX+F(1.0f),plY-F(1.0f),left[0],data,e);
        weightY(aW,aY,aAbs,plX,plY-F(1.0f),left[1],data,e);
        weightY(aW,aY,aAbs,plX,plY,left[2],data,e);
        weightY(aW,aY,aAbs,plX+F(1.0f),plY,left[3],data,e);
        weightY(aW,aY,aAbs,plX-F(1.0f),plY-F(1.0f),right[0],data,e);
        weightY(aW,aY,aAbs,plX-F(2.0f),plY-F(1.0f),right[1],data,e);
        weightY(aW,aY,aAbs,plX-F(2.0f),plY,right[2],data,e);
        weightY(aW,aY,aAbs,plX-F(1.0f),plY,right[3],data,e);
        out.conditioning=float(aAbs)>0.0f?std::fabs(float(aW))/float(aAbs):0.0f;
        F finalY=aY/aW;
        const F maxY=maxF(maxF(left[1],left[2]),maxF(right[0],right[3]));
        const F minY=minF(minF(left[1],left[2]),minF(right[0],right[3]));
#ifdef RECOMP_GSR1_MUTANT_NO_CLAMP
        finalY=edgeSharpness*finalY;(void)minY;(void)maxY;
#else
        finalY=clampF(edgeSharpness*finalY,minY,maxY);
#endif
        F deltaY=finalY-color[3];
        // smooth high contrast input
#ifndef RECOMP_GSR1_MUTANT_NO_DELTA_CLAMP
        deltaY=clampF(deltaY,F(-23.0f/255.0f),F(23.0f/255.0f));
#endif
#ifdef RECOMP_GSR1_MUTANT_SIGN
        deltaY=-deltaY;
#endif
#ifdef RECOMP_GSR1_MUTANT_NO_SATURATE
        color[0]=(color[0]+deltaY);color[1]=(color[1]+deltaY);color[2]=(color[2]+deltaY);
#else
        color[0]=clampF((color[0]+deltaY),F(0.0f),F(1.0f));
        color[1]=clampF((color[1]+deltaY),F(0.0f),F(1.0f));
        color[2]=clampF((color[2]+deltaY),F(0.0f),F(1.0f));
#endif
    }
    out.r=float(color[0]);out.g=float(color[1]);out.b=float(color[2]);
    return out;
}
// A shader result in [0, 1] as the UNORM8 attachment stores it (round to nearest; NaN -> 0).
inline std::uint8_t unormByte(float v) noexcept {
    if(!(v>0.0f))return 0;
    if(v>=1.0f)return 255;
    return std::uint8_t(int(std::floor(v*255.0f+0.5f)));
}
// The map pass's picture: the source through the tables (1280x720 RGBA8; alpha unused).
inline void mappedPicture(const std::uint8_t* source,const Lut& lut,std::vector<std::uint8_t>& out,
                          std::size_t pixels=std::size_t{SourceWidth}*SourceHeight) { // (lane fsr-stage-b: any picture size)
    out.assign(pixels*4,255);
    for(std::size_t p=0;p<pixels;++p)
        for(unsigned c=0;c<3;++c)out[p*4+c]=lut[c*256+source[p*4+c]];
}
// Expected bytes with the accepted range of each (see the head of this file); soft: pixels with a range wider than the value.
struct Expected {std::vector<std::uint8_t> value,lo,hi;std::size_t sharpened=0,uncertain=0;};
// The window (width x height; black border, alpha 255; BGRA byte order when bgra) GSR must write for a mapped picture.
inline bool window(const std::uint8_t* mapped,const Constants& c,std::uint32_t width,std::uint32_t height,bool bgra,Expected& out) {
    const auto l=layout(width,height);
    if(!l || !mapped || l->imageWidth!=c.width || l->imageHeight!=c.height)return false;
    out.value.assign(std::size_t{width}*height*4,0);
    for(std::size_t i=3;i<out.value.size();i+=4)out.value[i]=255;
    out.lo=out.value;out.hi=out.value;out.sharpened=out.uncertain=0;
    const float threshold=c.params[2],margin=FlipMargin255/255.0f;
    for(std::uint32_t y=0;y<l->imageHeight;++y)for(std::uint32_t x=0;x<l->imageWidth;++x) {
        const Pixel p=pixel<float>(mapped,c,int(x),int(y));
        const auto d=(std::size_t(y+l->top)*width+x+l->left)*4;
        const float v[3]{p.r,p.g,p.b};
        const bool uncertain=std::fabs(p.vote-threshold)<=margin || (p.sharpened && p.conditioning<IllConditioned);
        out.sharpened+=p.sharpened;out.uncertain+=uncertain;
        for(int k=0;k<3;++k) {
            const int value=unormByte(v[k]),base=unormByte(p.base[k]);
            int lo=value,hi=value;
            if(uncertain){lo=std::min(value,base-DeltaLimit);hi=std::max(value,base+DeltaLimit);}
            else if(p.sharpened){lo=value-SoftTolerance;hi=value+SoftTolerance;}
            const auto o=d+unsigned(bgra?2-k:k);
            out.value[o]=std::uint8_t(value);out.lo[o]=std::uint8_t(std::clamp(lo,0,255));out.hi[o]=std::uint8_t(std::clamp(hi,0,255));
        }
    }
    return true;
}
// The audit's comparison: a colour byte may differ from the value by 1, or lie in [lo - 1, hi + 1] (counted in `soft`);
// alpha (and so the border's alpha) must be equal; a colour byte with no range (the border's included) may differ by 1;
// anything else is a mismatch.
struct Diff {std::size_t bytes=0,mismatches=0,offByOne=0,soft=0,first=0;unsigned worst=0,got=0,expected=0;};
inline Diff compare(const std::uint8_t* got,const Expected& e) noexcept {
    Diff r;r.bytes=e.value.size();
    for(std::size_t b=0;b<r.bytes;++b) {
        const unsigned d=unsigned(std::abs(int(got[b])-int(e.value[b])));
        if(!d)continue;
        r.worst=std::max(r.worst,d);
        if(d==1 && b%4!=3)++r.offByOne;
        else if(b%4!=3 && int(got[b])>=int(e.lo[b])-1 && int(got[b])<=int(e.hi[b])+1)++r.soft;
        else if(!r.mismatches++){r.first=b;r.got=got[b];r.expected=e.value[b];}
    }
    return r;
}
// Which scaler a frame uses, from the switches' state (the presenter's precedence, in one place so the host test can check
// it): GSR 1 wins over FSR 1, which wins over the cubic / linear / nearest modes; a scaler that failed (audit or resources)
// is skipped. The result is the presenter's `fsr` value: 0 none of them, 1 FSR EASU + RCAS, 2 FSR EASU only, Mode = GSR 1.
constexpr unsigned Mode=3;
constexpr unsigned frameScaler(bool gsrWant,bool gsrFailed,unsigned fsrWant,bool fsrFailed) noexcept {
#ifdef RECOMP_GSR1_MUTANT_PRECEDENCE
    return fsrWant && !fsrFailed?fsrWant:gsrWant && !gsrFailed?Mode:0u;
#elif defined(RECOMP_GSR1_MUTANT_DEFAULT_ON)
    return !gsrFailed?Mode:fsrWant && !fsrFailed?fsrWant:0u;
#else
    return gsrWant && !gsrFailed?Mode:fsrWant && !fsrFailed?fsrWant:0u;
#endif
}
}
