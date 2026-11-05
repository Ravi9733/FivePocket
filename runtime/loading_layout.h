#pragma once
// thor-char (v685, Thor edition only; included by resume_layout.h's users under RECOMP_THOR_EDITION and by the host test / preview):
// the lower screen while the game runs a character SWITCH (owner 2026-10-24: "when transitioning we should get a screen very similar to
// the resume game screen but that says loading, maybe with GTA's loading spinning wheel icon"). Pure (no Vulkan).
//  - v687 (owner on v685's preview: "I actually like the spinning wheel by itself, do that with the blurred background with no pill, just
//    center the wheel and make it slightly larger"): the main screen's picture heavily blurred (map_tap_renderer.inc's mirror, as the
//    Resume screen) and ONLY the spinner, centred, 1.5 x v685's (v689, owner: "the spinning wheel doesn't need the shadow": no halo); no pill,
//    no text (the "Loading" label stays in the image, unused). Not tappable. (v685: the Resume pill with the spinner and "Loading".)
//  - the spinner is DRAWN HERE, not the game's: a white ring arc of SpinArcDeg with round caps whose opacity fades from the head to the
//    tail, turning clockwise once per SpinPeriodMs - the look of GTA V's own busy spinner (the rotating ring beside "Loading" / "Saving"
//    in the bottom right corner), reproduced; SpinFrames pre-rendered frames in the label image, texel for texel (KindLabel).
#include "loading_label.h"
#include "resume_layout.h"
#include "touch_context.h" // RC1.1: IS_PAUSE_MENU_ACTIVE / IS_PLAYER_CONTROL_ON (the QoL line's decoder, identical)
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace recomp::gpu::maptap {
constexpr unsigned SpinPx=114,SpinFrames=32,SpinCols=8,SpinRows=SpinFrames/SpinCols; // (v687: 1.5 x v685's 76)
constexpr float SpinRadius=45.0f,SpinThick=12.0f,SpinArcDeg=280.0f,SpinTailAlpha=0.08f,SpinGap=26.0f;
constexpr std::int64_t SpinPeriodMs=1000;
// The spinner's coverage at pixel (x, y) of a SpinPx cell for frame f (0..SpinFrames-1): 0..255. The arc's head at angle
// f * 360 / SpinFrames (0 = up, clockwise), the tail SpinArcDeg behind it; 4x4 supersampled.
inline std::uint8_t spinCoverage(unsigned f,unsigned x,unsigned y) noexcept {
    constexpr float Pi=3.14159265358979f;
    const float c=float(SpinPx)*0.5f,head=float(f%SpinFrames)*360.0f/float(SpinFrames);
    float sum=0;
    for(unsigned sy=0;sy<4;++sy)for(unsigned sx=0;sx<4;++sx) {
        const float px=float(x)+(float(sx)+0.5f)*0.25f-c,py=float(y)+(float(sy)+0.5f)*0.25f-c;
        const float r=std::sqrt(px*px+py*py);
        float a=std::atan2(px,-py)*180.0f/Pi;if(a<0)a+=360.0f;      // (0 = up, clockwise)
        float behind=head-a;if(behind<0)behind+=360.0f;              // degrees behind the head
        float alpha=0;
        if(behind<=SpinArcDeg) {
            if(std::fabs(r-SpinRadius)<=SpinThick*0.5f)alpha=1.0f;
        }
        // round caps at the head and the tail
        for(const float capAt:{head,head-SpinArcDeg}) {
            const float t=capAt*Pi/180.0f,cx=SpinRadius*std::sin(t),cy=-SpinRadius*std::cos(t);
            if((px-cx)*(px-cx)+(py-cy)*(py-cy)<=SpinThick*SpinThick*0.25f)alpha=1.0f;
        }
        if(alpha>0) { // the fade from the head (1) to the tail (SpinTailAlpha)
            const float k=std::clamp(behind/SpinArcDeg,0.0f,1.0f);
            alpha*=behind>SpinArcDeg?SpinTailAlpha:1.0f-(1.0f-SpinTailAlpha)*k;
        }
        sum+=alpha;
    }
    return std::uint8_t(std::lround(std::clamp(sum/16.0f,0.0f,1.0f)*255.0f));
}
// v689: when the lower screen shows Loading (map_tap_renderer.inc mapTapLoading runs it once per present). raw = the game's switch flag;
// hudSerial = the renderer's minimap replay serial (advances when the game draws its HUD); startMs = the player's last START press.
// From the flag's rise to its fall: Loading. After the fall it HOLDS (the owner: "until the character is fully loaded in (it goes black
// first)") until the game's minimap is replayed again ("hud"), the player presses START ("start": a real pause gets its Resume screen)
// or HoldMaxMs ("cap"). A switch longer than MaxMs shows the usual lower screen from then on.
// v691 (device v689, run 3p3i3lv8, Franklin -> Michael: the game's minimap came back 109 ms after the fall while the switch's last blurred
// frames still passed pausedFrame: the Resume pill for 12 frames): "hud" ends the hold only once the frames no longer look paused (or
// TailMs after the fall: v687's tail), so the switch's blurred tail stays Loading.
struct LoadingState {
    static constexpr std::int64_t MaxMs=60000,HoldMaxMs=15000,TailMs=4000;
    bool was=false,hold=false;std::int64_t since=0,end=0;std::uint64_t holdSerial=0,episodes=0;
    const char* why=nullptr; // (set on the step that ended a hold)
    struct Out {bool on=false,show=false,rose=false,fell=false;};
    Out step(bool raw,std::int64_t now,std::uint64_t hudSerial,std::int64_t startMs,bool paused=false) noexcept {
        Out o;why=nullptr;
        if(raw && !was){since=now;++episodes;hold=false;o.rose=true;}
        if(!raw && was){end=now;hold=true;holdSerial=hudSerial;o.fell=true;}
        if(hold && !o.fell) {
            why=hudSerial!=holdSerial && (!paused || now-end>=TailMs)?"hud":startMs>end?"start":now-end>=HoldMaxMs?"cap":nullptr;
            if(why)hold=false;
        }
        was=raw;
        o.on=raw || hold;
        o.show=o.on && (raw?now-since<MaxMs:true);
        return o;
    }
};
// RC1.1 (thor-radio, owner 2026-10-24: "when dying or busted the loading screen with the spinner should be active"): Loading through death
// (WASTED) and arrest (BUSTED) until the respawn has the player in control again. raw = dead || arrested (hud_game_state.h readPlayerFate);
// after it falls (the respawn) Loading HOLDS until the player has control (touch_context.h IS_PLAYER_CONTROL_ON) and the game's minimap is
// replayed again (hudSerial advances), or CapMs from the start (whatever the state).
struct RespawnState {
    static constexpr std::int64_t CapMs=30000;
    bool was=false,hold=false;std::int64_t since=0,end=0;std::uint64_t holdSerial=0,episodes=0;
    const char* why=nullptr; // (set on the step that ended a hold)
    struct Out {bool on=false,show=false,rose=false,fell=false;};
    Out step(bool raw,bool controlOn,std::int64_t now,std::uint64_t hudSerial) noexcept {
        Out o;why=nullptr;
        if(raw && !was){since=now;++episodes;hold=false;o.rose=true;}
        if(!raw && was){end=now;hold=true;holdSerial=hudSerial;o.fell=true;}
        if(hold && !o.fell) {
            why=controlOn && hudSerial!=holdSerial?"control":now-since>=CapMs?"cap":nullptr;
            if(why)hold=false;
        }
        was=raw;
        o.on=raw || hold;
        o.show=o.on && now-since<CapMs;
        return o;
    }
};
// RC1.1 (owner 2026-10-24: "also sometimes the resume screen flashes randomly?"): the lower screen's Resume is shown on GAME STATE only - the
// pause menu open (touch_context.h IS_PAUSE_MENU_ACTIVE: any of its four bytes) or the pause map the map tap opened - never on the frame
// looking paused (v661-v806: map_tap.h pausedFrame, the pause blur without the tonemap, which other blurred screens pass too). A real
// pause wins over Loading (a pause during a switch / a respawn shows Resume).
// v811 (owner 2026-10-24 ~22:50: "make the resume pill only appear when in the map screen"): the Resume screen (the blurred backdrop) on
// every pause screen, its PILL only while the full pause map is on screen (the map tap's map or the MAP tab's full-map view: the
// full-map scissor, map_tap.h mapShown); the other tabs show the backdrop alone (and a tap there does nothing: map_tap.h tapIgnored).
struct LowerPick {bool resume=false,pill=false,loading=false;};
constexpr LowerPick lowerPick(bool pauseMenu,bool pauseMap,bool switchLoading,bool respawnLoading) noexcept {
    const bool pause=pauseMenu || pauseMap;
    return {pause,pauseMap,!pause && (switchLoading || respawnLoading)};
}
// v693 (owner on v691: "either blur the background much much more, or make the background the same as the pause menu. try both see which
// looks cleaner. or maybe a hybrid approach"): files/private-trace/diag-loading-bg = blur | pause | hybrid (read once a second; absent or
// anything else = DefaultBg):
//   blur   the mirror from MirrorHeavy (5 x 2 texels of the 1280x720 frame) back up to 320 x 180: soft colour fields only;
//   pause  the game's pause-menu look: its lighter blur (from 40 x 22) and PauseTint (below);
//   hybrid the heavy blur and PauseTint.
// PauseTint: the pause menu's backdrop as a function of the game's frame, fitted on the game's own pause / unpaused screenshot pairs at
// the same view (agents/thor-char/loading/pause_fit.py, 64-px blocks of the backdrop the menu leaves visible; thor-radio shots ch0 / ch1,
// Michael's hall): out_c = Gain * (Sat * x_c + (1 - Sat) * luma) + Offset_c. One scene does not pin the gain (rms 14.9 at 1.0 .. 20.4 at
// 0.6: the game's pause blur differs from ours), so the gain is set to 0.8 (rms 17.0; 1.0 would crush the darks) and the rest fitted:
// saturation 0.14, offsets -8.9 / -0.8 / +4.7 levels - darker, nearly grey, a little blue.
enum class LoadingBg : std::uint8_t {Blur,Pause,Hybrid};
constexpr LoadingBg DefaultBg=LoadingBg::Hybrid;
constexpr float PauseTintGain=0.8f,PauseTintSat=0.14f,PauseTintOffset[3]{-8.9f/255.0f,-0.8f/255.0f,4.7f/255.0f};
inline LoadingBg parseLoadingBg(const char* s,std::size_t n) noexcept {
    std::size_t i=0;while(i<n && (s[i]==' ' || s[i]=='\n' || s[i]=='\r' || s[i]=='\t'))++i;
    std::size_t e=i;while(e<n && s[e]!=' ' && s[e]!='\n' && s[e]!='\r' && s[e]!='\t')++e;
    const auto is=[&](const char* w){std::size_t k=0;for(;w[k];++k)if(i+k>=e || s[i+k]!=w[k])return false;return i+k==e;};
    return is("blur")?LoadingBg::Blur:is("pause")?LoadingBg::Pause:is("hybrid")?LoadingBg::Hybrid:DefaultBg;
}
constexpr bool tinted(LoadingBg b) noexcept {return b!=LoadingBg::Blur;}
constexpr bool heavy(LoadingBg b) noexcept {return b!=LoadingBg::Pause;}
inline unsigned spinFrame(std::int64_t ms) noexcept {
    const std::int64_t m=((ms%SpinPeriodMs)+SpinPeriodMs)%SpinPeriodMs;
    return unsigned(m*SpinFrames/SpinPeriodMs)%SpinFrames;
}
// Where the Loading pieces sit in the label image (below the Resume label's two halves): the text at row TextRow, the spinner frames in
// a SpinCols x SpinRows grid from row SpinRow.
constexpr unsigned LoadingTextRow=2*LabelH,LoadingSpinRow=LoadingTextRow+LoadingH;
constexpr unsigned LabelTexW=std::max(LabelW,std::max(LoadingW,SpinCols*SpinPx)),LabelTexH=LoadingSpinRow+SpinRows*SpinPx;
struct LoadingLayout {bool ok=false;float s=1;ResumeRect pill,text,spin;float radius=0;};
// v687: the spinner alone, centred (texel for texel); `pill` = its halo's circle (the ring's outer edge).
constexpr float SpinHaloAlpha=0.40f,SpinHaloPx=14.0f;
inline LoadingLayout spinnerLayout(float w,float h) noexcept {
    LoadingLayout L;
    if(!(w>=float(SpinPx)) || !(h>=float(SpinPx)))return L;
    L.spin.x0=std::round((w-float(SpinPx))*0.5f);L.spin.y0=std::round((h-float(SpinPx))*0.5f);L.spin.x1=L.spin.x0+float(SpinPx);L.spin.y1=L.spin.y0+float(SpinPx);
    const float cx=(L.spin.x0+L.spin.x1)*0.5f,cy=(L.spin.y0+L.spin.y1)*0.5f,r=SpinRadius+SpinThick*0.5f;
    L.pill={cx-r,cy-r,cx+r,cy+r};L.radius=r;
    L.ok=true;return L;
}
inline LoadingLayout loadingLayout(float w,float h) noexcept {
    LoadingLayout L;
    if(!(w>0) || !(h>0))return L;
    const float content=float(SpinPx)+SpinGap+float(LoadingW);
    const float pw=content+2.0f*ResumePadK*ResumePillH;
    L.s=std::min(1.0f,std::min(w/(pw+80.0f),h/(ResumePillH*3.0f)));
    if(L.s<1.0f)return L; // (texel for texel only: the lower screen is 1240 wide; a smaller one shows the plain blur)
    const float ph=ResumePillH;
    L.pill.x0=std::round((w-pw)*0.5f);L.pill.x1=L.pill.x0+pw;
    L.pill.y0=std::round(h*ResumeCentreY-ph*0.5f);L.pill.y1=L.pill.y0+ph;
    L.radius=ph*0.5f;
    const float mid=(L.pill.y0+L.pill.y1)*0.5f,x0=std::round((w-content)*0.5f);
    L.spin.x0=x0;L.spin.y0=std::round(mid-float(SpinPx)*0.5f);L.spin.x1=L.spin.x0+float(SpinPx);L.spin.y1=L.spin.y0+float(SpinPx);
    const float capTop=std::round(mid-float(LoadingCap)*0.5f);
    L.text.x0=x0+float(SpinPx)+SpinGap;L.text.x1=L.text.x0+float(LoadingW);
    L.text.y0=capTop-float(LoadingCapTop);L.text.y1=L.text.y0+float(LoadingH);
    L.ok=true;
    return L;
}
} // namespace recomp::gpu::maptap
