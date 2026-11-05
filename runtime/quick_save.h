#pragma once
// v820 (thor-radio, Thor edition only; included by map_tap.h and hud_screen.h under RECOMP_THOR_EDITION and by the host test). Owner
// 2026-10-25 (item 3 of the Thor feature batch): "A small Quick Save button on the lower screen. It runs the game's own quick save via
// the phone". Pure (no Vulkan, no Android).
//  - The button: a small dark box with "Quick Save" (quick_save_label.h) in the lower screen's bottom-right corner, above the bars band,
//    over the map (the game's help-box look: black at QsFillAlpha, white text, a small radius); pressed (the macro runs): opaque light,
//    dark text. Shown only while the lower screen shows the HUD map (not a wheel, the phone, a pause screen, Loading or a hidden HUD).
//  - A tap on it (the app: REQUEST | TOUCH with SaveCode in the right stick, WheelGesture.java SAVE_CODE) runs QuickSave: D-pad UP
//    (opens the phone), then - once the renderer has seen the phone up for SettleMs (or, when the phone is not mirrored below and the
//    library cannot see it, BlindMs after UP) - D-pad LEFT (Quick Save: device 2026-10-25, the phone always reopens on Contacts, the
//    centre; LEFT = the Quick Save icon), then A: the game's own Save Game list opens on the main screen. The player picks the slot and
//    confirms; the macro never does. Abandoned (nothing more pressed) when the phone does not come up within WaitMaxMs, goes away before
//    A, or the player presses B / START meanwhile.
#include "quick_save_icons.h" // v832: the floppy disk (owner: "make the quick save button like a actual disc")
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace recomp::gpu::maptap {
inline constexpr char SaveMarker[]="recomp-quick-save-v1"; // (WheelTouch.java SAVE_MARKER: the app sends SaveCode only with it)
constexpr std::int16_t SaveCode=-32764; // (the right stick of a Request | Touch; never a stick position, see PhoneTapCode)
// Layout at 1080 rows (scaled by h / 1080, at most 1; the label is drawn texel for texel, so below QsMinScale the button is not shown).
// v832: a square button with the floppy icon (IconPx texel for texel); the app's trainer button sits QsGap1080 left of it (same size).
constexpr float QsW1080=88,QsH1080=88,QsRight1080=32,QsBottom1080=184,QsRadius1080=10,QsMinScale=0.95f,QsHitPad1080=8,QsGap1080=16;
constexpr float QsFillAlpha=0.78f;
constexpr std::uint8_t QsText=255,QsPressedFill=242,QsPressedText=16;
struct QsRect {float x0=0,y0=0,x1=0,y1=0;};
struct QsLayout {bool ok=false;QsRect box,label;float radius=0;};
inline QsLayout quickSaveLayout(float w,float h) noexcept {
    QsLayout L;
    if(!(w>0) || !(h>0))return L;
    const float s=std::min(1.0f,h/1080.0f);
    if(s<QsMinScale)return L;
    L.box.x1=w-std::round(QsRight1080*s);L.box.x0=L.box.x1-std::round(QsW1080*s);
    L.box.y1=h-std::round(QsBottom1080*s);L.box.y0=L.box.y1-std::round(QsH1080*s);
    if(L.box.x0<0 || L.box.y0<0)return L;
    L.radius=std::round(QsRadius1080*s);
    L.label.x0=std::round((L.box.x0+L.box.x1-float(IconPx))*0.5f);L.label.x1=L.label.x0+float(IconPx); // (v832: the icon)
    L.label.y0=std::round((L.box.y0+L.box.y1-float(IconPx))*0.5f);L.label.y1=L.label.y0+float(IconPx);
    L.ok=true;
    return L;
}
// The rectangle the app treats as the button (WheelGesture.java saveRect): the box grown by QsHitPad1080 on every side.
inline QsRect quickSaveHit(float w,float h) noexcept {
    const auto L=quickSaveLayout(w,h);
    if(!L.ok)return {};
    const float p=std::round(QsHitPad1080*std::min(1.0f,h/1080.0f));
    return {L.box.x0-p,L.box.y0-p,L.box.x1+p,L.box.y1+p};
}

// The macro (Machine::read runs it on the pad reads, under its lock).
// v834 (owner 2026-10-25: "can the script for the quick save be sped up to near frame perfect?"):
//  1. NATIVE: the tap asks the trainer engine for OpenSaveMenu (trainer.h: SET_SAVE_MENU_ACTIVE at the script thread's next safe point, the
//     game's own Save Game menu, no phone); the macro only waits for the menu (the renderer's paused frame) - NativeWaitMs at most.
//  2. STATE-DRIVEN fallback (no trainer export, or it refused the request): no fixed sleeps. UP held KeyFrames presents; then the first
//     frame the renderer sees the phone's home grid WITH a highlight (accepting input: phone_touch.h gridSelection) -> LEFT held KeyFrames;
//     the first frame the highlight is on Quick Save (SaveCell) -> A held KeyFrames. LEFT not taken (the highlight unchanged after
//     LeftRetryMs: the phone was not accepting yet, e.g. the first open after a load) -> LEFT again, LeftRetries at most. Every wait has a
//     stall timeout. Without the phone's picture (phone=main) the v824 timings (blind).
struct QuickSave {
    static constexpr std::uint16_t Up=0x0001,Left=0x0004,A=0x1000,B=0x2000,Start=0x0010;
    static constexpr std::int64_t UpMs=150,WaitMaxMs=6000,SettleMs=600,BlindMs=1100,KeyMs=150,GapMs=800,NativeWaitMs=2500;
    static constexpr std::int64_t KeyMinMs=40,KeyMaxMs=400,LeftRetryMs=500,SelWaitMs=2500; // (KeyMaxMs: frames stalled - the key goes up anyway)
    static constexpr unsigned KeyFrames=2,LeftRetries=2;
    static constexpr int SaveCell=3,HomeCell=4; // (phone_touch.h: row 1 column 0 = Quick Save; the phone opens on Contacts, row 1 column 1)
    enum class Step:std::uint8_t {Idle,Native,PressUp,Wait,PressLeft,WaitSel,Gap,PressA};
    enum class End:std::uint8_t {None,Done,NoPhone,PhoneGone,Player,Menu,NoMenu,Highlight};
    Step step=Step::Idle;End last=End::None;std::int64_t t=0,upSince=-1,started=0,doneMs=-1;bool known=false,native=false;
    std::uint64_t runs=0,ends=0,f0=0;unsigned lefts=0;int selAtLeft=-1;
    bool busy() const noexcept {return step!=Step::Idle;}
    // A tap on the shown button. phoneKnown: the renderer can see the phone; nativeOk: the trainer engine took OpenSaveMenu.
    bool start(std::int64_t now,bool phoneKnown,bool nativeOk=false) noexcept {
        if(step!=Step::Idle)return false;
        step=nativeOk?Step::Native:Step::PressUp;t=started=now;upSince=-1;known=phoneKnown;native=nativeOk;lefts=0;selAtLeft=-1;doneMs=-1;f0=0;++runs;
        return true;
    }
    // Whether the renderer should read the phone's highlight for this macro (state-driven steps only).
    bool wantsSel() const noexcept {return known && !native && (step==Step::Wait || step==Step::PressLeft || step==Step::WaitSel);}
    // One pad read: the buttons to add (0 = none). phoneUp: the renderer sees the phone up; player: the player's own buttons;
    // sel: the phone grid's highlight (-1 none / unknown) and selFresh: read from a frame after the last key went up; frame: presents so
    // far; paused: the renderer's paused frame (a frontend menu: the Save Game menu).
    std::uint16_t buttons(std::int64_t now,bool phoneUp,std::uint16_t player,int sel=-1,std::uint64_t frame=0,bool paused=false) noexcept {
        if(step==Step::Idle)return 0;
        if(step!=Step::PressUp && step!=Step::Native && (player&(B|Start))){finish(End::Player,now);return 0;}
        const auto held=[&]{return (frame>=f0+KeyFrames && now-t>=KeyMinMs) || now-t>=KeyMaxMs;};
        switch(step) {
            case Step::Native:
                if(paused){finish(End::Menu,now);return 0;}
                if(now-t>=NativeWaitMs)finish(End::NoMenu,now);
                return 0;
            case Step::PressUp:
                if(f0==0)f0=frame;
                if(known?!held():now-t<UpMs)return Up;
                step=Step::Wait;t=now;return 0;
            case Step::Wait:
                if(known) {
                    if(phoneUp) {
                        if(upSince<0)upSince=now;
                        if(sel>=0){step=Step::PressLeft;t=now;f0=frame;selAtLeft=sel;++lefts;return Left;} // (v834: the first frame it accepts input)
                        if(now-upSince>=WaitMaxMs){finish(End::Highlight,now);return 0;}
                        return 0;
                    }
                    if(upSince>=0){finish(End::PhoneGone,now);return 0;}
                    if(now-t>=WaitMaxMs){finish(End::NoPhone,now);return 0;}
                    return 0;
                }
                if(now-t>=BlindMs){step=Step::PressLeft;t=now;return Left;}
                return 0;
            case Step::PressLeft:
                if(known && !phoneUp){finish(End::PhoneGone,now);return 0;}
                if(known?!held():now-t<KeyMs)return Left;
                step=known?Step::WaitSel:Step::Gap;t=now;return 0;
            case Step::WaitSel:
                if(!phoneUp){finish(End::PhoneGone,now);return 0;}
                if(sel==SaveCell){step=Step::PressA;t=now;f0=frame;return A;}
                if(sel==selAtLeft && now-t>=LeftRetryMs && lefts<=LeftRetries){step=Step::PressLeft;t=now;f0=frame;++lefts;return Left;} // (not taken)
                if(sel>=0 && sel!=selAtLeft && sel!=SaveCell){finish(End::Highlight,now);return 0;} // (moved elsewhere: never A)
                if(now-t>=SelWaitMs){finish(End::Highlight,now);return 0;}
                return 0;
            case Step::Gap:
                if(known && !phoneUp){finish(End::PhoneGone,now);return 0;}
                if(now-t<GapMs)return 0;
                step=Step::PressA;t=now;return A;
            case Step::PressA:
                if(known?!held():now-t<KeyMs)return A;
                finish(End::Done,now);return 0;
            default:return 0;
        }
    }
    void finish(End e,std::int64_t now=0) noexcept {step=Step::Idle;last=e;++ends;doneMs=now-started;}
};
inline const char* quickSaveEndName(QuickSave::End e) noexcept {
    switch(e){case QuickSave::End::Done:return "save-list";case QuickSave::End::NoPhone:return "no-phone";case QuickSave::End::PhoneGone:return "phone-gone";
        case QuickSave::End::Player:return "player";case QuickSave::End::Menu:return "native-menu";case QuickSave::End::NoMenu:return "native-no-menu";
        case QuickSave::End::Highlight:return "highlight";default:return "-";}
}
} // namespace recomp::gpu::maptap
