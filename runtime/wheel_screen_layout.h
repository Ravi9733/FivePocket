#pragma once
// thor-wheel (v601, Thor edition only): what the lower screen draws while the weapon wheel's replica is current (hud_screen.h's
// wheel_screen.inc records it). Plain arithmetic; the host test runs it (local-analysis/agents/thor-wheel/tests).
#include "hud_screen_layout.h"
#include "wheel_replay.h"

namespace recomp::gpu::hud {
// Three KindReplica quads over the wheel's overlay (the background, then) (texW x texH host texels): the ring's square (ring: its host texels) at the screen's
// centre, texel for texel when it fits; the stats panel's rectangle (stats) in the top right corner (wheelrep::layout; k = the replay's scale). color2 =
// 1 / the overlay's size (hud_screen.frag's kind 4), full brightness, no apron.
// thor-char: the character wheel on the lower screen (owner: "stats on left side, wheel on right side"): the stats panel's rectangle
// (stats: host texels of the overlay) centred in the left half, the wheel's (wheel) centred in the right half - its centre at
// (0.75 W, 0.5 H), where WheelGesture.java's touch directions start while D-pad DOWN is held - each texel for texel when it fits its half
// (CharMargin from the edges and the middle), else scaled down (aspect kept).
inline constexpr float CharMargin=24.0f;
struct CharLayout {bool ok=false;wheelrep::Box stats,wheel;};
inline CharLayout charLayout(std::uint32_t width,std::uint32_t height,const float stats[4],const float wheel[4]) noexcept {
    CharLayout L;const float W=float(width),H=float(height);
    if(width<64 || height<64)return L;
    const auto place=[&](const float* r,float cx,float cy,wheelrep::Box& out) {
        const float w=r[2]-r[0],h=r[3]-r[1];
        if(!(w>=4.0f) || !(h>=4.0f))return false;
        const float s=std::min({1.0f,(W*0.5f-2.0f*CharMargin)/w,(H-2.0f*CharMargin)/h});
        const float bw=std::round(w*s),bh=std::round(h*s),x0=std::floor(cx-bw*0.5f),y0=std::floor(cy-bh*0.5f);
        out={x0,y0,x0+bw,y0+bh};return true;
    };
    L.ok=place(stats,W*0.25f,H*0.5f,L.stats) && place(wheel,W*0.75f,H*0.5f,L.wheel);
    return L;
}
// The quads: the background (one texel inside the overlay's BOTTOM-RIGHT corner: guest 1215,681, right of the wheel's scissor
// 1089-1214 and below the stats panel's 282-552, so cleared to the wheel background and never drawn), the stats panel, the wheel.
inline DrawList charDrawList(std::uint32_t width,std::uint32_t height,const float wheel[4],const float stats[4],std::uint32_t texW,std::uint32_t texH) noexcept {
    DrawList out;
    const auto L=charLayout(width,height,stats,wheel);
    if(!L.ok || !texW || !texH)return out;
    const auto quad=[&](const wheelrep::Box& b,const float* shape) {
        auto& d=out.add();d.rect[0]=b.x0;d.rect[1]=b.y0;d.rect[2]=b.x1;d.rect[3]=b.y1;
        for(unsigned i=0;i<4;++i)d.shape[i]=shape[i];
        d.color2[0]=1.0f/float(texW);d.color2[1]=1.0f/float(texH);d.param[0]=KindReplica;d.param[2]=1.0f;d.param[3]=0;
    };
    {const float bg[4]{float(texW)-1.75f,float(texH)-1.75f,float(texW)-1.25f,float(texH)-1.25f};quad(wheelrep::Box{0,0,float(width),float(height)},bg);}
    quad(L.stats,stats);quad(L.wheel,wheel);
    return out;
}
// v815 (thor-radio): the game's phone screen (its own 256 x 256 movie target, ring = its texels) on the lower screen: black, a dark
// rounded shell, the screen's image at the aspect the phone model shows it with (PhoneAspect: 237 x 343 of the 1920 x 1080 main frame),
// as tall as the screen allows (PhoneMargin), centred; bilinear through the display tables. WheelGesture.java's phone taps use the same
// rectangle (phoneLayout): its bottom PhoneBarShare is the phone's soft-key bar, the right PhoneBackShare of it = Back.
inline constexpr float PhoneAspect=237.0f/343.0f,PhoneMargin1080=56.0f,PhoneShell1080=26.0f,PhoneBarShare=0.1f,PhoneBackShare=0.4f;
struct PhoneLayout {bool ok=false;wheelrep::Box screen,shell;float radius=0;};
// v822: turned = a landscape app (the phone held sideways): the screen at the inverse aspect (343 x 237), as wide as the screen allows.
inline PhoneLayout phoneLayout(std::uint32_t width,std::uint32_t height,bool turned=false) noexcept {
    PhoneLayout L;const float W=float(width),H=float(height);
    if(width<64 || height<64)return L;
    const float m=std::round(PhoneMargin1080*H/1080.0f),sh=std::round(PhoneShell1080*H/1080.0f);
    const float aspect=turned?1.0f/PhoneAspect:PhoneAspect;
    float h=H-2.0f*m,w=std::round(h*aspect);
    if(w>W-2.0f*m){w=W-2.0f*m;h=std::round(w/aspect);}
    const float x0=std::floor((W-w)*0.5f),y0=std::floor((H-h)*0.5f);
    L.screen={x0,y0,x0+w,y0+h};L.shell={x0-sh,y0-sh,x0+w+sh,y0+h+sh};L.radius=sh*1.6f;L.ok=true;
    return L;
}
// v822: turned = the movie holds a landscape app turned a quarter clockwise (census phone-land-1, shot e5): the screen is drawn with
// KindReplica flag 32 (hud_screen.frag: the texture read turned back, texel u = 1 - screen v, texel v = screen u).
constexpr float PhoneTurnFlag=32.0f;
// v824: pov = the floor patch's texels in the same image (phone_pov.h; none = a black backdrop): drawn over the whole screen at PovDim,
// bilinear (magnified: soft), under the shell.
constexpr float PovDim=0.85f;
inline DrawList phoneDrawList(std::uint32_t width,std::uint32_t height,const float tex[4],std::uint32_t texW,std::uint32_t texH,bool turned=false,
                              const float* pov=nullptr) noexcept {
    DrawList out;
    const auto L=phoneLayout(width,height,turned);
    if(!L.ok || !texW || !texH || !(tex[2]>tex[0]) || !(tex[3]>tex[1]))return out;
    if(pov && pov[2]>pov[0] && pov[3]>pov[1]) { // v824: the floor
        auto& d=out.add();d.rect[0]=0;d.rect[1]=0;d.rect[2]=float(width);d.rect[3]=float(height);
        d.shape[0]=pov[0]+0.5f;d.shape[1]=pov[1]+0.5f;d.shape[2]=pov[2]-0.5f;d.shape[3]=pov[3]-0.5f; // (half a texel in: no bleed from the movie)
        d.color2[0]=1.0f/float(texW);d.color2[1]=1.0f/float(texH);d.param[0]=KindReplica;d.param[1]=0;d.param[2]=PovDim;d.param[3]=0;
    } else {auto& d=out.add();d.rect[0]=0;d.rect[1]=0;d.rect[2]=float(width);d.rect[3]=float(height);for(unsigned i=0;i<4;++i)d.shape[i]=d.rect[i];
     d.color[0]=d.color[1]=d.color[2]=0;d.color[3]=1;d.param[0]=KindBox;d.param[3]=0;}
    {auto& d=out.add();d.rect[0]=L.shell.x0;d.rect[1]=L.shell.y0;d.rect[2]=L.shell.x1;d.rect[3]=L.shell.y1;for(unsigned i=0;i<4;++i)d.shape[i]=d.rect[i];
     d.color[0]=d.color[1]=0.09f;d.color[2]=0.10f;d.color[3]=1;d.param[0]=KindBox;d.param[1]=L.radius;d.param[3]=1;}
    {auto& d=out.add();d.rect[0]=L.screen.x0;d.rect[1]=L.screen.y0;d.rect[2]=L.screen.x1;d.rect[3]=L.screen.y1;for(unsigned i=0;i<4;++i)d.shape[i]=tex[i];
     d.color2[0]=1.0f/float(texW);d.color2[1]=1.0f/float(texH);d.param[0]=KindReplica;d.param[1]=turned?PhoneTurnFlag:0.0f;d.param[2]=1.0f;d.param[3]=0;}
    return out;
}
// v818 (thor-radio): the game's help box over the lower screen's map - the text replay's area (box: its host texels, top-left = guest
// TextArea's top-left) texel for texel from the top-left inside the frame (Hud12FramePx1080 + TextInset1080), cropped to the screen
// (TextMaxShare of its height at most); premultiplied (KindReplica flag 1: only the box's own pixels over the map).
inline constexpr float TextInset1080=16.0f,TextMaxShare=0.45f;
struct TextLayout {bool ok=false;wheelrep::Box rect,tex;};
inline TextLayout textLayout(std::uint32_t width,std::uint32_t height,const float box[4]) noexcept {
    TextLayout L;const float W=float(width),H=float(height);
    if(width<64 || height<64 || !(box[2]-box[0]>=8.0f) || !(box[3]-box[1]>=8.0f))return L;
    const float o=std::round((hudrep::Hud12FramePx1080+TextInset1080)*H/1080.0f);
    const float w=std::min(box[2]-box[0],W-2.0f*o),h=std::min(box[3]-box[1],std::round(H*TextMaxShare));
    if(!(w>=8.0f) || !(h>=8.0f))return L;
    L.rect={o,o,o+w,o+h};L.tex={box[0],box[1],box[0]+w,box[1]+h};L.ok=true;
    return L;
}
inline DrawList textDrawList(std::uint32_t width,std::uint32_t height,const float box[4],std::uint32_t texW,std::uint32_t texH) noexcept {
    DrawList out;
    const auto L=textLayout(width,height,box);
    if(!L.ok || !texW || !texH)return out;
    auto& d=out.add();d.rect[0]=L.rect.x0;d.rect[1]=L.rect.y0;d.rect[2]=L.rect.x1;d.rect[3]=L.rect.y1;
    d.shape[0]=L.tex.x0;d.shape[1]=L.tex.y0;d.shape[2]=L.tex.x1;d.shape[3]=L.tex.y1;
    d.color2[0]=1.0f/float(texW);d.color2[1]=1.0f/float(texH);d.param[0]=KindReplica;d.param[1]=1;d.param[2]=1.0f;d.param[3]=0;
    return out;
}
inline DrawList wheelDrawList(std::uint32_t width,std::uint32_t height,const float ring[4],const float stats[4],std::uint32_t texW,std::uint32_t texH,int k) noexcept {
    DrawList out;
    const auto rect=[](const float* r){return hudrep::Rect{std::int32_t(r[0]),std::int32_t(r[1]),std::int32_t(r[2]),std::int32_t(r[3])};};
    const auto L=wheelrep::layout(width,height,rect(ring),rect(stats),k);
    if(!L.ok || !texW || !texH)return out;
    const auto quad=[&](const wheelrep::Box& b,const float* shape) {
        auto& d=out.add();d.rect[0]=b.x0;d.rect[1]=b.y0;d.rect[2]=b.x1;d.rect[3]=b.y1;
        for(unsigned i=0;i<4;++i)d.shape[i]=shape[i];
        d.color2[0]=1.0f/float(texW);d.color2[1]=1.0f/float(texH);d.param[0]=KindReplica;d.param[2]=1.0f;d.param[3]=0;
    };
    // v619: first the whole screen in the overlay's background (one texel inside its top-left corner, outside the ring's and the stats
    // panel's rectangles: cleared to wheelrep::Background and never drawn by the wheel), so the screen is one clean colour through the
    // same display path as the wheel's texels (no seam at the quads' edges); then the ring and the stats panel.
    {const float bg[4]{1.25f,1.25f,1.75f,1.75f};quad(wheelrep::Box{0,0,float(width),float(height)},bg);}
    quad(L.ring,ring);if(stats[2]>stats[0] && stats[3]>stats[1])quad(L.stats,stats); // (v651: the radio wheel has no stats panel)
    return out;
}
}
