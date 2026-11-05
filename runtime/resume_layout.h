#pragma once
// thor-maptap (v667, Thor edition only; included by hud_screen.h under RECOMP_THOR_EDITION and by the host preview / test): the lower
// screen's Resume control while the game is paused. Pure (no Vulkan).
// Owner 2026-10-24: "Resume should be modeled after the ps5s button that says resume, transparent, and match the color of the screen
// with text in the same font as the sign in text on the start page".
//  - background: the main screen's own presented picture, mirrored live (resume_screen.inc), cover-fit: the 1240x1080 lower screen shows
//    the centre of the main picture at the main panel's own scale (1920x1080 -> its x 340..1580), so every colour matches;
//  - the control: the PS5 home screen's "Resume Activity" pill (thor-edition/resume/owner-ref-ps5-resume.png, 1080x607: rows 471-507 =
//    37 rows, fully rounded; inside 31,35,38 over a 12,17,20 backdrop; the label white, medium weight, its cap
//    ~0.3 of the pill's height; side padding ~1.1 heights): here 192 rows high (v675, owner "make the resume game button bigger and
//    center it too": 1.6x v667's 120, the label rendered at the matching 58-px cap), centred on the lower screen; fill ResumePillRgb at ResumePillAlpha = exactly the PS5's 31,35,38 over its 12,17,20
//    backdrop, and still a translucent dark pill behind white text over the bright outdoor blur (v667 preview: white at 0.08, the first
//    reading of the reference, vanished there); pressed (the library's Close request): the PS5's focused look, opaque light, dark text;
//  - the label "Resume Game" (map_tap_label.h: Helvetica Neue Medium, the closest available face to the game's "Sign In" heading).
#include "map_tap_label.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace recomp::gpu::maptap {
constexpr float ResumePillH=192,ResumePadK=1.1f,ResumeCentreY=0.5f,ResumePillAlpha=0.55f; // (v675, owner: "bigger and center it": 1.6x, centred)
constexpr std::uint8_t ResumePillRgb[3]{47,50,53};
constexpr std::uint8_t ResumeText=255,ResumePressedFill=242,ResumePressedText=16;
// v681 (owner: "make the border of it have a slight shadow for contrast"): a soft dark shadow round the pill in both states, drawn under it
// and never over it: black at ResumeShadowAlpha at the pill's edge fading to 0 over ResumeShadowPx, the whole shape ResumeShadowDy lower.
constexpr float ResumeShadowAlpha=0.40f,ResumeShadowPx=12.0f,ResumeShadowDy=3.0f;
struct ResumeRect {float x0=0,y0=0,x1=0,y1=0;};
struct ResumeLayout {bool ok=false;float s=1;ResumeRect pill,label;float radius=0;};
// The pill (centred, its centre at ResumeCentreY of the height) and the label's rectangle (its texels 1:1 at s = 1).
inline ResumeLayout resumeLayout(float w,float h) noexcept {
    ResumeLayout L;
    if(!(w>0) || !(h>0))return L;
    const float pw=float(LabelW)+2.0f*ResumePadK*ResumePillH;
    L.s=std::min(1.0f,std::min(w/(pw+80.0f),h/(ResumePillH*3.0f)));
    const float ph=std::round(ResumePillH*L.s),pwr=std::round(pw*L.s);
    L.pill.x0=std::round((w-pwr)*0.5f);L.pill.x1=L.pill.x0+pwr;
    L.pill.y0=std::round(h*ResumeCentreY-ph*0.5f);L.pill.y1=L.pill.y0+ph;
    L.radius=ph*0.5f;
    // the cap centred on the pill's middle row
    const float capTop=std::round((L.pill.y0+L.pill.y1)*0.5f-float(LabelCap)*L.s*0.5f);
    const float lw=std::round(float(LabelW)*L.s),lh=std::round(float(LabelH)*L.s);
    L.label.x0=std::round((w-lw)*0.5f);L.label.x1=L.label.x0+lw;
    L.label.y0=capTop-std::round(float(LabelCapTop)*L.s);L.label.y1=L.label.y0+lh;
    L.ok=true;
    return L;
}
// The mirror: the source rectangle (texels of a sw x sh present image) the w x h lower screen shows, cover-fit and centred.
inline ResumeRect mirrorCrop(float sw,float sh,float w,float h) noexcept {
    if(!(sw>0) || !(sh>0) || !(w>0) || !(h>0))return {};
    const float k=std::max(w/sw,h/sh); // (the source scaled to cover the lower screen)
    const float cw=w/k,ch=h/k;
    return {(sw-cw)*0.5f,(sh-ch)*0.5f,(sw+cw)*0.5f,(sh+ch)*0.5f};
}
} // namespace recomp::gpu::maptap
