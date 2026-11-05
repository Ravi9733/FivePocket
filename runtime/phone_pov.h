#pragma once
// v824 (thor-radio, Thor edition only; included by wheel_replay_renderer.inc's users under RECOMP_THOR_EDITION and by the host test).
// Owner 2026-10-25: "whatever ground is in front of the character, add that as the background zoomed as though you really have a POV of
// his phone"; then: "where the character is standing in that second, and like a zoom in of the floor, as though they are holding their
// phone as the background". Pure (no Vulkan).
// While the phone is shown on the lower screen, its backdrop is a patch of the game's own main frame (the present image of this frame,
// before the fps overlay; the phone itself is not on it): on foot the floor right beside the player's feet (the on-foot chase camera keeps
// the feet at a near-constant screen spot: x 0.40-0.50, y 0.86-0.93 of the frame in every 2026-10-25 shot), in a vehicle the wider view
// ahead (preview D: the floor beside a car is plain dark asphalt). The patch is blitted into the phone image's atlas beside the screen
// movie (PovW x PovH texels at most; a smaller destination = a softer picture after the lower screen's bilinear magnification) and drawn
// over the whole lower screen at PovDim, under the phone's shell and screen.
// Tunables: files/private-trace/diag-phone-pov (read once a second while the phone is up): "cx= cy= w= sharp= carcx= carcy= carw=
// carsharp= dim= off=1" (shares of the frame; w = the patch width / the frame width; sharp = the destination size / PovW).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
namespace recomp::gpu::phonepov {
constexpr std::uint32_t PovW=320,PovH=279,PovGap=4; // (the lower screen's 1240 x 1080 aspect; the gap keeps bilinear reads apart)
struct Params {
    float cx=0.61f,cy=0.89f,w=0.10f,sharp=0.5f;            // on foot: the floor beside the feet (preview floor-grid column 1)
    float carCx=0.50f,carCy=0.74f,carW=0.43f,carSharp=0.2f; // in a vehicle: preview D (zoom 1.5, soft)
    float dim=0.85f;bool off=false;
};
inline void parse(const char* text,Params& p) noexcept {
    if(!text)return;
    const char* s=text;
    while(*s) {
        while(*s==' ' || *s=='\n' || *s=='\t' || *s=='\r')++s;
        char key[12]{};double v=0;int used=0;
        if(std::sscanf(s,"%11[a-z]=%lf%n",key,&v,&used)==2 && used>0 && std::isfinite(v) && v>=0.0 && v<=1.0) {
            const auto is=[&](const char* k){for(int i=0;;++i){if(key[i]!=k[i])return false;if(!k[i])return true;}};
            const float f=float(v);
            if(is("cx"))p.cx=f;else if(is("cy"))p.cy=f;else if(is("w") && f>=0.02f)p.w=f;else if(is("sharp") && f>=0.05f)p.sharp=f;
            else if(is("carcx"))p.carCx=f;else if(is("carcy"))p.carCy=f;else if(is("carw") && f>=0.02f)p.carW=f;
            else if(is("carsharp") && f>=0.05f)p.carSharp=f;else if(is("dim"))p.dim=f;else if(is("off"))p.off=f>=0.5f;
            s+=used;
        } else {while(*s && *s!=' ' && *s!='\n')++s;}
    }
}
struct Rect {std::int32_t x0=0,y0=0,x1=0,y1=0;bool empty() const noexcept {return x1<=x0 || y1<=y0;}};
// The source patch in a pw x ph present image: centre (cx, cy) as shares, width w of the frame, the lower screen's aspect (PovW : PovH),
// moved inside the frame (never cut).
inline Rect patch(std::uint32_t pw,std::uint32_t ph,float cx,float cy,float w) noexcept {
    if(pw<16 || ph<16)return {};
    float sw=std::round(w*float(pw)),sh=std::round(sw*float(PovH)/float(PovW));
    if(sh>float(ph)){sh=float(ph);sw=std::round(sh*float(PovW)/float(PovH));}
    if(sw>float(pw))sw=float(pw);
    if(sw<4 || sh<4)return {};
    const float x0=std::clamp(std::round(cx*float(pw)-sw*0.5f),0.0f,float(pw)-sw),y0=std::clamp(std::round(cy*float(ph)-sh*0.5f),0.0f,float(ph)-sh);
    return {std::int32_t(x0),std::int32_t(y0),std::int32_t(x0+sw),std::int32_t(y0+sh)};
}
// The destination in the atlas: the region right of the screen movie (x from screenW + PovGap), its size sharp x PovW (at least 16 wide).
inline Rect dest(std::uint32_t screenW,float sharp) noexcept {
    const float s=std::clamp(sharp,0.05f,1.0f);
    const std::int32_t w=std::max<std::int32_t>(16,std::int32_t(std::lround(float(PovW)*s))),h=std::max<std::int32_t>(14,std::int32_t(std::lround(float(PovH)*s)));
    const std::int32_t x0=std::int32_t(screenW+PovGap);
    return {x0,0,x0+w,h};
}
inline std::uint32_t atlasW(std::uint32_t screenW) noexcept {return screenW+PovGap+PovW;}
inline std::uint32_t atlasH(std::uint32_t screenH) noexcept {return std::max(screenH,PovH);}
struct Choice {Rect src,dst;bool car=false;};
// This frame's patch: on foot (or unknown) the floor, in a vehicle the view ahead; empty when off.
inline Choice choose(const Params& p,bool inVehicle,std::uint32_t pw,std::uint32_t ph,std::uint32_t screenW) noexcept {
    Choice c;
    if(p.off)return c;
    c.car=inVehicle;
    c.src=inVehicle?patch(pw,ph,p.carCx,p.carCy,p.carW):patch(pw,ph,p.cx,p.cy,p.w);
    c.dst=dest(screenW,inVehicle?p.carSharp:p.sharp);
    if(c.src.empty())c.dst={};
    return c;
}
} // namespace recomp::gpu::phonepov
