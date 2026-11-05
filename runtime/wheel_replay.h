#pragma once
// thor-wheel (Thor edition only, RECOMP_THOR_EDITION): the game's weapon wheel moved to the Thor's lower screen. The plain arithmetic
// (no Vulkan, no renderer state), so the host tests run this text (local-analysis/agents/thor-wheel/tests). The renderer side is
// wheel_replay_renderer.inc; the lower screen's side hud_screen.h (publishWheel) and this file's layout.
//
// What the game draws while LB holds the wheel open (Thor censuses n=10 wheel-1 and n=11 wheel-2 vs n=8 wheel-0, v600, 2026-10-22;
// local-analysis/agents/thor-wheel/tests/ops):
//  - the frame's last post-process (ps 08dfab6c into 720:0:1280, its resolve to 05ea2000) carries the wheel's green, desaturated
//    look: it is the same draw as in a frame without the wheel (n=8 D1945), with other constants. There is NO separate tint draw;
//  - the BACKDROP: one rect list on the main frame (pitch 1280, 720:3) sampling that resolve (ps 816a99bc, scissor 0,0,1280,720:
//    n=8 D1947, n=10 D1789, n=11 D1794), present in every frame;
//  - right after it, the WHEEL LAYER: Scaleform draws on the main frame with the full-screen movie viewport 0,0,1280,720:
//    16 PS_GFill_ColorPremult ring segments first (228 / 174 / 264 indices), then glyphs (PS_TexturedSprite_Alpha, the 1024x1024
//    glyph cache) and weapon icons (PS_GFill_1Texture, a 512x256 atlas); with a weapon highlighted (n=11) the stats panel follows
//    (a 210-index panel, 12 bars, 4 labels; top right of the safe zone). n=10: 24 operations (D1790-1813), n=11: 43 (D1795-1837).
//    No clear and no other surface inside it. Then the radar pass and the minimap box (hud_replay.h) as in every frame.
//    n=8 has no full-screen-viewport Scaleform draw at all.
//  - geometry (screenshots glitches/20261022-183440-wheel-1-snap.png, -183716-wheel-2-snap.png, 1920x1080 = 1.5 x guest): the
//    ring's centre (640, 298) guest, outer radius ~200, inner ~124; 8 slots, 0 at the top (pistol), clockwise; RS straight up
//    highlighted the top slot. The stats panel ~(1090,69)-(1215,192).
// v619 (the owner on v615: "you did the weapon wheel terribly ... the resolution is weird, it shows the main screen ... It can look way
// cleaner"; setting "Weapon wheel: Lower screen", the default; files/settings/second-screen.txt "wheel=main" keeps the game's own):
//  - MAIN SCREEN: no wheel in any frame, the first included. The ring's operations are held back (copied, not executed) while they
//    match; when the ring is confirmed (16 + a non-ring 17th op, or the layer's end after 16) the whole wheel layer is never drawn
//    there; when a candidate stops matching, the held copies are executed in order on the real surfaces before the operation that
//    broke the match (the frame is then exactly the game's). The green look is the frame's own post-process and stays.
//  - LOWER SCREEN: only the game's wheel, re-rendered from its own draws at the lower screen's scale (k/16, the ring's square
//    filling Fill of the height), over a clean background: a dark shade of the game's green grading (Background; no copy of the
//    game's picture any more: v601-v615 drew the backdrop, the world, under the ring). The ring's square texel for texel and centred
//    (its centre = the screen's centre: WheelGesture's touch directions start there); the stats panel in the top right corner as
//    large as it can be without touching the ring.
// Which operations: the RING is exactly the weapon wheel's: 16 consecutive PS_GFill_ColorPremult draws in 8 (segment, border) pairs,
// segments of RingFill / RingFillCurrent indices (the equipped slot's segment carries the green arc), borders of RingBorder indices,
// and the 17th layer operation not PS_GFill_ColorPremult (exactly 8 slots). Every census frame on the Thor so far (109 distinct, scan
// local-analysis/agents/thor-wheel/tests/scan-*.txt) matches only in the wheel frames; the wanted stars (1212 + pairs of 264, 84
// indices, also ColorPremult; v604) and every other full-screen-movie layer do not. Other UI of the same movie can come BEFORE the
// ring (auto-073206 n=17: a panel, text and bars, then the wheel): it stays on the main screen. The wheel's operations are the ring
// and every layer operation after it (glyphs, icons, the stats panel) to the layer's end.
// In a car (run d9osgqok, v615, 23:06:32-33, telemetry veh=1, LB held 1.1 s) no layer matched: whatever the car shows stays on the
// main screen; the 24-op wheel at 23:06:36.25 came after LB was pressed again while Franklin got out (veh=0 at 36.41).
#include "hud_replay.h" // hudrep::Rect, overlayFor, edge, hostBox
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include "phone_pov.h" // v824 (thor-radio): the phone's floor backdrop (here, not in generic_renderer.cpp: its line numbers stay the normal build's)
extern "C" int recomp_trainer_v1(int action,int a0,int a1,int a2); // v834 (trainer_glue.inc): the Quick Save button's native save menu

namespace recomp::gpu::wheelrep {
using hudrep::Rect;

constexpr std::uint64_t PsColorPremult=0x0c7341b616776c5aull; // ui_texture_tracking.h's names
constexpr unsigned RingOps=16,RingFill=228,RingFillCurrent=264,RingBorder=174,MinFill=96,MaxOps=192;
#if defined(RECOMP_WHEEL_TEST_PS) // the renderer harness (local-analysis/agents/thor-wheel): its own pixel shader stands in for the ring's
inline std::uint64_t testColorPremult=0;
inline bool ringShader(std::uint64_t ps) noexcept {return ps==PsColorPremult || (testColorPremult && ps==testColorPremult);}
#else
constexpr bool ringShader(std::uint64_t ps) noexcept {return ps==PsColorPremult;}
#endif
// The ring's square and the stats panel's rectangle (guest pixels, with a margin), and the area the replay renders (their union).
constexpr int RingCentreX=640,RingCentreY=298,RingHalf=216;
constexpr Rect RingCrop{RingCentreX-RingHalf,RingCentreY-RingHalf,RingCentreX+RingHalf,RingCentreY+RingHalf};
constexpr Rect StatsCrop{1082,61,1224,200};
constexpr Rect Area{std::min(RingCrop.x0,StatsCrop.x0),std::min(RingCrop.y0,StatsCrop.y0),std::max(RingCrop.x1,StatsCrop.x1),std::max(RingCrop.y1,StatsCrop.y1)};

// What the collector needs of one operation (the renderer reads it from the operation's state before executing it).
struct Op {
    bool clear=false;      // a clear (of any kind)
    bool main=false;       // on the main frame (RB_SURFACE_INFO pitch 1280 and its colour format)
    bool scaleform=false;  // its pixel shader is one of the Scaleform shaders
    bool fullMovie=false;  // viewport 0,0,1280,720
    std::uint64_t ps=0;unsigned indices=0;
    float vp[4]{};         // thor-char: PA_CL_VPORT_XSCALE, XOFFSET, YSCALE, YOFFSET (the operation's viewport)
    bool colour=true;      // thor-char: RB_COLOR_MASK writes render target 0 (false: a stencil mask draw)
};
// Execute as usual / copy and hold back (a ring candidate) / copy and never execute on the main frame (the confirmed wheel).
// thor-char: Copy = copied for the replay AND executed on the main frame (the character wheel's stencil masks and clears).
enum class Act : std::uint8_t {Draw,Defer,Skip,Copy};
// flush: execute the held-back copies first, in order (the candidate was not the weapon wheel); then `act` for this operation.
struct Decision {bool flush=false;Act act=Act::Draw;};
enum class Phase : std::uint8_t {Before,Layer,After};
// One per renderer: every executed operation of the frame in queue order while the lower screen shows the wheel, then present().
// v634 (owner-visible on v632: the wheel FLASHED on the main screen for single frames while LB was held; 60 fps recording
// thor-edition/wheel-video/main-042506.mp4, frames 101/171/174/183/204/223 of 472, run u93dqnyr flushes 503 -> 507 over the holds):
//  - a frame's layer may come in several runs (other main-frame operations between them): a run without the ring no longer ends the
//    search (v619-v632: phase After at the first run's end, so a ring in a later run went to the main screen whole, uncounted);
//  - once the ring was confirmed (the 16-op pattern), the wheel counts as open for ActiveFrames frames: in them every layer run is
//    taken as the wheel from its first ring-shaped operation on (skipped on the main screen, copied for the replay), whatever the
//    ring's exact shape that frame (a highlight, an animation step, another fill count) - nothing ring-shaped is held and flushed
//    onto the main screen while the wheel is open. A loose frame keeps the wheel open only when it had a ring's worth (16) of
//    ring-shaped operations, and is replayed only with at least half of that; else the lower screen keeps its last replay.
constexpr unsigned ActiveFrames=8,LooseReplayMin=RingOps/2;
struct Collector {
    Phase phase=Phase::Before;unsigned pos=0,n=0;bool wheel=false,broken=false;
    bool loose=false,skipRun=false;unsigned ringLike=0,active=0; // (v634)
    std::uint64_t looseFrames=0,looseOps=0; // (v634, cumulative: RENDER_WHEEL loose_frames / loose_ops)
    // Whether layer op `o` is the ring's operation number `pos` (0-15), or (pos 16) the operation right after the ring.
    static bool ringAt(unsigned pos,const Op& o) noexcept {
        const bool cp=ringShader(o.ps);
        if(pos>=RingOps)return !cp;
        if(!cp)return false;
        // v624: a segment's fill varies with its state (228 plain, 264 with the equipped arc; on the device v623 held and then executed
        // 263 ring-like ops in 29 frames: other fills are likely, RENDER_WHEEL_FLUSH logs them); the 8 borders are the ring's fixed outline
        return pos%2?o.indices==RingBorder:o.indices>=MinFill;
    }
    // v634: an operation that looks like part of the ring (its shader, a fill or the border's size).
    static bool ringShaped(const Op& o) noexcept {return ringShader(o.ps) && (o.indices>=MinFill || o.indices==RingBorder);}
    bool open() const noexcept {return active>0;}
    unsigned held() const noexcept {return wheel?0u:pos;}  // copies held back (not executed yet)
    // The held candidate stops here: the ring complete (16) makes it the wheel, anything shorter is flushed.
    bool settle() noexcept {
        if(!pos)return false;
        if(wheel){pos=0;return false;}
        if(pos==RingOps){wheel=true;n=pos;pos=0;return false;}
        lastPos=pos;pos=0;return true;
    }
    unsigned lastPos=0; // v624: the ring position at the last candidate that stopped matching (RENDER_WHEEL_FLUSH)
    // v813 (owner 2026-10-24 ~23:15: "the weapon wheel ... can occasionally show for a couple frames before going to the bottom screen";
    // v811 run tz4peuin RENDER_WHEEL_FLUSH at 00:42:38 / 00:42:53 device time: the opening frames' ring came in 8 ops - held, then
    // executed on the main frame by a guest work item (why=work, held 5 / 8) and by the op after the 8th (why=op, held 8): the ring's
    // 16-op pattern never completed in those frames, so its candidate reached the main screen). lb = the weapon wheel's button (LB) held
    // within WeaponGraceMs (weaponButtonRecent): then the first ring-shaped operation of a run already counts as the wheel - skipped on
    // the main frame and copied, as while the wheel is open (v634) - so nothing ring-shaped is ever held and flushed while LB is held.
    Decision op(const Op& o,bool lb=false) noexcept {
        if(!o.main)return {}; // (other surfaces: the radar pass, post-processing targets; never part of the layer)
        const bool layerOp=!o.clear && o.scaleform && o.fullMovie;
        if(phase==Phase::Before) {
            if(!layerOp)return {};
            phase=Phase::Layer;
        }
        if(phase==Phase::After)return {};
        if(!layerOp) { // a run of layer operations ends here
            skipRun=false;
            if(wheel && !loose){phase=Phase::After;return {};} // (the confirmed wheel's run: one per frame)
            const bool f=settle();
            if(wheel && !loose){phase=Phase::After;return {false,Act::Draw};}
            phase=Phase::Before;return {f,Act::Draw}; // (v634: a later run may still hold the ring)
        }
        if(skipRun) {
            if(broken || ++n>MaxOps){broken=true;return {};} // (too long: drawn from here on, not replayed)
            if(loose){const bool r=ringShaped(o);ringLike+=r;looseOps+=r;}
            return {false,Act::Skip};
        }
        if((open() || lb) && ringShaped(o) && !pos) { // (v634: the wheel is open: this run is the wheel from here on; v813: or LB is held)
            wheel=true;loose=true;skipRun=true;++ringLike;++looseOps;
            if(++n>MaxOps){broken=true;return {};}
            return {false,Act::Skip};
        }
        if(ringAt(pos,o)) {
            if(pos==RingOps){wheel=true;loose=false;skipRun=true;n+=pos+1;pos=0;return {false,Act::Skip};} // (the 17th op, not a ring segment: the weapon wheel)
            ++pos;return {false,Act::Defer};
        }
        const bool flush=pos>0;lastPos=pos;pos=0;
        if(ringAt(0,o)){pos=1;return {flush,Act::Defer};}
        return {flush,Act::Draw};
    }
    // A held or skipped operation could not be copied: the renderer executes it (after flushing the held ones); the frame is not replayed.
    void fail() noexcept {if(!wheel)pos=0;broken=true;}
    // Before a resolve (it may read the main frame): a held candidate is executed first, so the resolve sees what the game drew.
    bool beforeResolve() noexcept {if(wheel || !pos)return false;lastPos=pos;pos=0;return true;}
    // The frame's present: flush = execute the held copies first; replay = the wheel was drawn (and not broken).
    struct End {bool flush=false,replay=false;};
    End present() noexcept {
        End e;e.flush=settle();
        const bool strict=wheel && !loose;
        e.replay=wheel && !broken && (strict || ringLike>=LooseReplayMin);
        if(loose)++looseFrames;
        if(strict || (loose && ringLike>=RingOps))active=ActiveFrames;else if(active)--active;
        phase=Phase::Before;pos=n=0;wheel=broken=loose=skipRun=false;ringLike=0;
        return e;
    }
};

// ---- the radio wheel (v651, thor-radio; owner 2026-10-23: "when character is in car, long pressing radio button to select the wheel goes
// to bottom screen"). Same model as the weapon wheel (Draw / Defer / Skip; the renderer's wheelList holds the copies; replay at the
// present), with the radio's own layer shape and an early gate, because the radio's distinctive draw comes LAST:
//  - census thor-play-1021/census/radio-wheel-2 n=2 (the only radio frame; v649, in Michael's car, D-pad LEFT held): right after the
//    backdrop copy (ps 816a99bc, D1769) one run of full-movie Scaleform draws D1770-1793: 6 x PS_TexturedSprite_Alpha (the station /
//    artist / title text at the centre: 108, 108, 60, 60, 216, 216 indices), 17 x PS_GFill_1Texture station logos (282 indices; one
//    273 and one 300: the current / highlighted ones), then one PS_GFill_ColorPremult of 612 indices (the Radio Off icon), then the
//    radar pass as in every frame. 207 distinct census frames on the Thor and the Fold (thor-play-1021, fold-glitch-1022,
//    .rendering-audit, .perf-audit) have no other frame with even two full-movie PS_GFill_1Texture draws of 240-340 indices (the
//    weapon wheel's icons with every weapon: one of 312; scan in local-analysis/agents/thor-radio).
//  - geometry (thor-edition/radio3-main.png, radio-ls.png, radio-stickR-*.png: the blue highlight ring fitted at three slots,
//    Rock Radio, Radio Off, Rebel Radio; 1920x1080 = 1.5 x guest): the slots' centres on a circle centred (639.6, 329.7) guest,
//    radius 190; the highlight ring's radius 34; the text at the centre (x 579-700, y 293-343).
// The gate: a candidate is held only while the radio's button (D-pad LEFT, XINPUT 0x0004) was held within RadioGraceMs (the pad
// reads note it: radioLeftMs) or while the radio was taken within RadioActiveFrames frames - so no other frame ever holds anything.
// Held: the run's text (before the first logo) and logos; confirmed at the LogoConfirm-th logo (Skip from there to the run's end:
// the Radio Off icon and anything after); any other layer operation before that ends the candidate (the held ops executed first, in
// order). Everything taken goes to the lower screen; RadioCrop is the replay's square, centred on the wheel's centre.
constexpr std::uint64_t PsTextSprite=0x545eb7f306ebe941ull,PsTexture1=0x91683f8ba9c9d426ull; // PS_TexturedSprite_Alpha, PS_GFill_1Texture
constexpr unsigned LogoMin=240,LogoMax=340,LogoConfirm=8,RadioMaxOps=96,RadioActiveFrames=8;
constexpr std::uint16_t PadLeft=0x0004;
constexpr std::int64_t RadioGraceMs=600;
constexpr int RadioCentreX=640,RadioCentreY=330,RadioHalf=240;
constexpr Rect RadioCrop{RadioCentreX-RadioHalf,RadioCentreY-RadioHalf,RadioCentreX+RadioHalf,RadioCentreY+RadioHalf};
// The last pad read (user 0, steady-clock milliseconds) that had the radio's button down; trace_runner's pad reader writes it.
inline std::atomic<std::int64_t>& radioLeftMs() noexcept {static std::atomic<std::int64_t> t{-(std::int64_t(1)<<40)};return t;}
// thor-char: the character wheel's button (D-pad DOWN, XINPUT 0x0002), noted the same way.
constexpr std::uint16_t PadDown=0x0002;
inline std::atomic<std::int64_t>& charDownMs() noexcept {static std::atomic<std::int64_t> t{-(std::int64_t(1)<<40)};return t;}
// v815 (thor-radio; owner 2026-10-25: "I like the phone idea"): the game's phone on the lower screen. Census phone-1 / pa-1..5
// (2026-10-25, Michael's hall): the phone's screen is a Scaleform movie drawn into its own 256 x 256 target (pitch 320) and resolved
// (caller 82388a2c, the generic movie resolve; 0464e000 there); the phone body is a 3D model drawn into a side target (scissor
// 920,376..1280,720), composited onto the main frame (ps e2649e6b, sampling that side target's resolve), then the screen quad
// (ps 325cb5da, sampling the screen's resolve, depth-tested) and the glass (ps 196a64a9). The screen quad and the glass appear in no
// other census (27 / 27 frames: the phone frames); e2649e6b is a generic composite (also full-screen and at the map tab's 206,158).
// The lower screen shows the screen's own resolved image (crisp, the game's drawing); the main frame skips the screen quad, the glass and
// the body's composite while the phone is up (seen within PhoneActiveFrames, or D-pad UP read within PhoneUpGraceMs: the first frame's
// composite comes before its screen quad).
constexpr std::uint16_t PadUp=0x0001;constexpr std::int64_t PhoneUpGraceMs=1000;constexpr unsigned PhoneActiveFrames=8;
constexpr std::uint64_t PhoneScreenPs=0x325cb5da85796d1bull,PhoneGlassPs=0x196a64a9be25a4bfull,PhoneCompositePs=0xe2649e6bc8ee4db8ull;
// v822: census phone-land-1 (2026-10-25, the Email app held sideways): the body's composite at 842,258..1280,712 (portrait 922,378): v815's
// y0 >= 300 missed it and the body stayed on the main frame. y0 >= 200 takes both; x0 >= 640 still leaves the map tab's 206,158 and the
// full-screen composite alone. A composite whose y0 < PhoneLandscapeY0 is the landscape phone (its screen movie holds the app rotated).
constexpr int PhoneBoxX0=640,PhoneBoxY0=200,PhoneLandscapeY0=318; // (the body's composite: a scissor inside the main frame's right half, not full-screen)
inline std::atomic<std::int64_t>& phoneUpMs() noexcept {static std::atomic<std::int64_t> t{-(std::int64_t(1)<<40)};return t;}
enum class PhoneAct : std::uint8_t {Draw,Skip,SkipCapture};
// One main-frame draw: its pixel shader, its scissor; active = the phone was up recently (or UP was read).
constexpr PhoneAct phoneAct(std::uint64_t ps,const Rect& sc,bool active) noexcept {
    if(ps==PhoneScreenPs)return PhoneAct::SkipCapture;
    if(!active)return PhoneAct::Draw;
    if(ps==PhoneGlassPs)return PhoneAct::Skip;
    if(ps==PhoneCompositePs && sc.x0>=PhoneBoxX0 && sc.y0>=PhoneBoxY0 && (sc.x0>0 || sc.y0>0))return PhoneAct::Skip;
    return PhoneAct::Draw;
}
// v822: whether a main-frame draw is the landscape phone's body composite (the frame's screen quad then holds the app turned a quarter).
constexpr bool phoneLandscape(std::uint64_t ps,const Rect& sc) noexcept {
    return ps==PhoneCompositePs && sc.x0>=PhoneBoxX0 && sc.y0>=PhoneBoxY0 && sc.y0<PhoneLandscapeY0;
}
struct PhoneTracker {unsigned left=0;std::uint64_t frames=0;bool landscape=false; // (v822: the last composite seen was the landscape one)
    void composite(bool sideways) noexcept {landscape=sideways;}
    void screen() noexcept {left=PhoneActiveFrames+1;}   // (a screen quad this frame)
    void present() noexcept {if(left){--left;++frames;}}
    bool active(bool upRecent) const noexcept {return left>0 || upRecent;}
    bool up() const noexcept {return left>0;}};
// v818 (thor-radio, mission text on the lower screen): the game's help box. Census text-1 (2026-10-25, Michael's living room, "Press
// [dpad] to sit on the couch.", IS_HELP_MESSAGE_BEING_DISPLAYED set): the box is full-movie Scaleform draws on the main frame - its
// background (PS_GFill_ColorPremult 138/32), the text (PS_TexturedSprite_Alpha 132/88), the button icon (ColorPremult 210/48 x 2, 927/206,
// 48/12) - at guest 65,38..277,67 (the 1920 x 1080 shot's 97,57..415,100). While the game says a help box is shown, every full-movie
// Scaleform draw of the frame is COPIED (also drawn on the main frame: the help box stays there) and replayed within TextArea at TextK
// (2 x) into the text lane's own target; the lower screen shows it over the map. At most TextMaxOps a frame (more: no replay).
constexpr Rect TextArea{0,0,640,200};constexpr int TextK=32;constexpr unsigned TextMaxOps=96;
constexpr bool textCopy(const Op& o,bool helpShown) noexcept {return helpShown && o.main && !o.clear && o.scaleform && o.fullMovie;}
// v813: the weapon wheel's button (LB, XINPUT 0x0100), noted the same way.
constexpr std::uint16_t PadLB=0x0100;constexpr std::int64_t WeaponGraceMs=600;
inline std::atomic<std::int64_t>& weaponLbMs() noexcept {static std::atomic<std::int64_t> t{-(std::int64_t(1)<<40)};return t;}
inline void notePad(std::uint16_t buttons,std::int64_t nowMs) noexcept {
    if(buttons&PadLB)weaponLbMs().store(nowMs,std::memory_order_relaxed); // (v813)
    if(buttons&PadUp)phoneUpMs().store(nowMs,std::memory_order_relaxed); // (v815)
    if(buttons&PadLeft)radioLeftMs().store(nowMs,std::memory_order_relaxed);
    if(buttons&PadDown)charDownMs().store(nowMs,std::memory_order_relaxed); // (thor-char)
}
#if defined(RECOMP_RADIO_TEST_GATE) // (the renderer harness, agents/thor-radio: the radio's button held in every frame)
inline bool radioButtonRecent(std::int64_t) noexcept {return true;}
#else
inline bool radioButtonRecent(std::int64_t nowMs) noexcept {const auto d=nowMs-radioLeftMs().load(std::memory_order_relaxed);return d>=0 && d<RadioGraceMs;}
#endif
constexpr bool radioText(const Op& o) noexcept {return o.ps==PsTextSprite;}
constexpr bool radioLogo(const Op& o) noexcept {return o.ps==PsTexture1 && o.indices>=LogoMin && o.indices<=LogoMax;}
struct RadioCollector {
    unsigned held=0,logos=0,n=0,active=0;bool radio=false,done=false,broken=false;
    unsigned lastHeld=0,lastLogos=0;     // the last candidate executed on the main frame (RENDER_RADIO_FLUSH)
    std::uint64_t flushedLogos=0;        // cumulative: logo operations a flush executed on the main frame (a radio frame not taken)
    bool open() const noexcept {return active>0;}
    unsigned heldOps() const noexcept {return radio?0u:held;}
    bool busy() const noexcept {return radio || held>0;}   // (the renderer then never asks the weapon wheel's collector)
    bool drop() noexcept {if(radio || !held)return false;lastHeld=held;lastLogos=logos;flushedLogos+=logos;held=logos=0;return true;}
    // gate: the radio's button held recently (radioButtonRecent); only read when no candidate is held.
    Decision op(const Op& o,bool gate) noexcept {
        if(!o.main)return {};
        const bool layerOp=!o.clear && o.scaleform && o.fullMovie;
        if(radio) {
            if(done)return {};
            if(!layerOp){done=true;return {};}                    // (the radio's run ended: the rest of the frame is the game's)
            if(broken || ++n>RadioMaxOps){broken=true;return {};} // (too long: drawn from here on, not replayed)
            return {false,Act::Skip};
        }
        if(!layerOp || broken)return {drop(),Act::Draw}; // (broken: a copy failed this frame; nothing more is taken)
        const bool can=held>0 || gate || open();
        if(can && radioLogo(o)) {
            ++held;++logos;
            if(logos>=LogoConfirm){radio=true;n=held;held=0;return {false,Act::Skip};} // (the held text and logos are the radio's first ops)
            return {false,Act::Defer};
        }
        if(can && radioText(o) && !logos && held<RadioMaxOps){++held;return {false,Act::Defer};}
        const bool f=drop();
        if(can && radioText(o)){held=1;return {f,Act::Defer};}  // (text after a candidate ended: a new candidate)
        return {f,Act::Draw};
    }
    // A held or skipped operation could not be copied: the renderer executes the held ones and it; the frame is not replayed.
    void fail() noexcept {if(!radio){lastHeld=held;lastLogos=logos;flushedLogos+=logos;held=logos=0;}broken=true;}
    bool beforeResolve() noexcept {return drop();}
    struct End {bool flush=false,replay=false;};
    End present() noexcept {
        End e;e.flush=drop();e.replay=radio && !broken;
        if(radio)active=RadioActiveFrames;else if(active)--active;
        held=logos=n=0;radio=done=broken=false;
        return e;
    }
};

// ---- the character switch wheel (thor-char; owner 2026-10-24: "the character select wheel stats on left side, wheel on right side of the
// bottom screen"). Census thor-play-1021/census/char-wheel-1 n=1 (v681, Michael in his house, D-pad DOWN held; shots
// agents/thor-radio/shots/cw1-main.png): after the backdrop copy the game draws the wheel's two Scaleform clips with their OWN viewports,
// not the full-movie one:
//  - the stats panel: viewport 1088,282 180x270 (scissor 1088,282-1268,552): per bar a stencil clear, a PS_SolidFill mask draw without
//    colour writes, three PS_GFill_ColorPremult bar quads, a PS_TexturedSprite_Alpha label (D770-D818, 8 bars);
//  - the wheel: viewport 1089,556 125x126 (scissor 1089,556-1214,682): PS_GFill_ColorPremult rings / quadrants, PS_GFill_1Texture
//    portraits, one text sprite (D819-D832).
// No other census frame on the Thor (thor-play-1021, agents/thor-maptap) has an operation with either viewport except earlier frames of
// the same wheel. So no candidate is held: an operation with one of the two viewports (exact floats) while the button was seen within
// RadioGraceMs (or the wheel was taken within CharActiveFrames frames) is the wheel's: colour draws are skipped on the main frame and
// copied, stencil masks and clears are executed AND copied (the main frame's stencil stays exactly the game's). Replayed from CharArea.
enum class CharPart : std::uint8_t {None,Stats,Wheel};
constexpr float CharStatsVp[4]{90.0f,1178.0f,-135.0f,417.0f},CharWheelVp[4]{62.5f,1151.5f,-63.0f,619.0f};
// The clips' scissors (a clear of the wheel's is known by its rectangle), and what the lower screen shows: the stats panel's own extent
// inside its viewport (the dark panel at guest 1088,304-1213,552 in cw1-main.png; 2 px margin left / top / right) and the wheel's square.
constexpr Rect CharStatsClip{1088,282,1268,552},CharWheelClip{1089,556,1214,682};
constexpr Rect CharStatsCrop{1086,302,1216,552},CharWheelCrop{1089,556,1214,682},CharArea{1086,302,1216,682};
constexpr unsigned CharActiveFrames=8,CharMaxOps=256;
inline CharPart charPart(const float vp[4]) noexcept {
    const auto same=[&](const float* w){for(unsigned i=0;i<4;++i)if(vp[i]!=w[i])return false;return true;};
    return same(CharStatsVp)?CharPart::Stats:same(CharWheelVp)?CharPart::Wheel:CharPart::None;
}
#if defined(RECOMP_RADIO_TEST_GATE) // (the renderer harness: the buttons held in every frame)
inline bool charButtonRecent(std::int64_t) noexcept {return true;}
inline bool weaponButtonRecent(std::int64_t) noexcept {return true;}
#else
inline bool charButtonRecent(std::int64_t nowMs) noexcept {const auto d=nowMs-charDownMs().load(std::memory_order_relaxed);return d>=0 && d<RadioGraceMs;}
// v813: LB held within WeaponGraceMs (the weapon wheel's gate; the collector's lb).
inline bool weaponButtonRecent(std::int64_t nowMs) noexcept {const auto d=nowMs-weaponLbMs().load(std::memory_order_relaxed);return d>=0 && d<WeaponGraceMs;}
#endif
inline bool phoneUpRecent(std::int64_t nowMs) noexcept {const auto d=nowMs-phoneUpMs().load(std::memory_order_relaxed);return d>=0 && d<PhoneUpGraceMs;}
struct CharCollector {
    unsigned n=0,active=0;bool taken=false,broken=false;
    std::uint64_t frames=0,ops=0,copies=0; // (cumulative: RENDER_CHAR)
    bool open() const noexcept {return active>0;}
    // part: charPart of the operation (None = not the wheel's); gate: the button seen recently.
    Decision op(const Op& o,CharPart part,bool gate) noexcept {
        if(!o.main || part==CharPart::None || !(gate || open()))return {};
        if(broken || ++n>CharMaxOps){broken=true;return {};} // (too many: the rest drawn on the main frame, nothing replayed)
        taken=true;++ops;
        if(o.clear || !o.colour){++copies;return {false,Act::Copy};}
        return {false,Act::Skip};
    }
    void fail() noexcept {broken=true;}
    struct End {bool replay=false;};
    End present() noexcept {
        End e;e.replay=taken && !broken;
        if(taken){active=CharActiveFrames;++frames;}else if(active)--active;
        n=0;taken=broken=false;
        return e;
    }
};

// ---- geometry
inline constexpr int One=16,MaxK=96;
inline constexpr float Fill=0.95f;        // the ring's square: 95 % of the lower screen's height (a 2.5 % margin above and below)
// The lower screen's background while the wheel is shown: a dark shade of the game's green grading (census n=10 probes of the graded
// frame: p3 0.078,0.122,0.031 / p5 0.094,0.129,0.039 / p10 0.122,0.173,0.071, values of the main frame's format; x0.7 of p3).
inline constexpr float Background[4]{0.055f,0.085f,0.022f,1.0f};
// v624: the background from the game's graded frame: its mean colour (rgb, the frame's own values) scaled to Background's luminance -
// the same hue (channel ratios) as the main screen, dark. ok=false for a black frame (then the previous / constant colour stays).
struct Hue {bool ok=false;float rgb[3]{};};
inline float luminance(float r,float g,float b) noexcept {return 0.2126f*r+0.7152f*g+0.0722f*b;}
inline Hue hueBackground(float r,float g,float b) noexcept {
    Hue h;const float l=luminance(r,g,b),l0=luminance(Background[0],Background[1],Background[2]);
    if(!(l>1e-4f) || !std::isfinite(l))return h;
    const float s=l0/l;h.rgb[0]=std::clamp(r*s,0.0f,1.0f);h.rgb[1]=std::clamp(g*s,0.0f,1.0f);h.rgb[2]=std::clamp(b*s,0.0f,1.0f);h.ok=true;
    return h;
}
// The replay's scale (sixteenths) for a lower screen of W x H: the ring's square fills Fill of the smaller side.
// v651: crop = the replayed square's side in guest pixels (the radio's RadioCrop; default the weapon wheel's RingCrop).
constexpr int scaleFor(std::uint32_t w,std::uint32_t h,int crop=RingCrop.y1-RingCrop.y0) noexcept {
    if(!w || !h || crop<=0)return One;
    const float side=float(std::min(w,h))*Fill;
    const int k=static_cast<int>(16.0f*side/float(crop));
    return std::clamp(k,One,MaxK);
}
struct Box {float x0=0,y0=0,x1=0,y1=0;constexpr float w() const noexcept {return x1-x0;}constexpr float h() const noexcept {return y1-y0;}};
// The lower screen: the ring's square (ring: host texels of the overlay) texel for texel when it fits (else scaled down, one scale),
// centred on the screen - so the ring's centre is the screen's centre (WheelGesture.java's touch directions rely on it) - and the
// stats panel's rectangle (stats: host texels) in the top right corner, StatsGap from the edges, as large as it can be (at most the
// ring's scale) while staying RingClear outside the ring's outer circle (radius RingRadius guest pixels at the ring's scale).
inline constexpr float StatsGap=8.0f,RingRadius=202.0f,RingClear=6.0f;
struct Layout {bool ok=false;Box ring,stats;float scale=1,statsScale=1;};
inline Layout layout(std::uint32_t width,std::uint32_t height,const Rect& ring,const Rect& stats,int k) noexcept {
    Layout L;
    const float W=float(width),H=float(height),rw=float(ring.x1-ring.x0),rh=float(ring.y1-ring.y0);
    if(width<64 || height<64 || !(rw>=8.0f) || !(rh>=8.0f) || k<=0)return L;
    L.scale=std::min({1.0f,W/rw,H/rh});
    const float w=std::round(rw*L.scale),h=std::round(rh*L.scale);
    const float left=std::floor((W-w)*0.5f),top=std::floor((H-h)*0.5f);
    L.ring={left,top,left+w,top+h};
    const float cx=(L.ring.x0+L.ring.x1)*0.5f,cy=(L.ring.y0+L.ring.y1)*0.5f,r=RingRadius*float(k)/16.0f*L.scale+RingClear;
    const float sw0=float(stats.x1-stats.x0),sh0=float(stats.y1-stats.y0);
    float lo=0.05f,hi=L.scale; // (the panel's scale relative to its host texels: the largest that keeps it clear of the ring)
    const auto clear=[&](float s){const float x0=W-StatsGap-std::round(sw0*s),y1=StatsGap+std::round(sh0*s);
        const float nx=std::clamp(cx,x0,W-StatsGap),ny=std::clamp(cy,StatsGap,y1);return x0>=0 && y1<=H && std::hypot(nx-cx,ny-cy)>=r;};
    if(clear(hi))lo=hi;else for(int i=0;i<24;++i){const float m=(lo+hi)*0.5f;(clear(m)?lo:hi)=m;}
    L.statsScale=lo;
    L.stats={W-StatsGap-std::round(sw0*lo),StatsGap,W-StatsGap,StatsGap+std::round(sh0*lo)};
    L.ok=true;
    return L;
}

// v628: the lower screen's picture per HUD frame (wheel_screen.inc's wheelFrame; the renderer harness runs the same rule per present).
// The wheel while its replica is current = a new replay within StaleFrames HUD frames; else the HUD picture (the minimap replica). LB is
// not an input: with LB held and no ring drawn (the game's wheel closed, blocked or not yet open) nothing is replayed, so the minimap
// shows from the second frame after the last ring on (the frame after it may still show the wheel's last replay: 1 frame late is allowed).
constexpr unsigned StaleFrames=2;
struct ScreenChoice {
    std::uint64_t seen=0;unsigned age=1000000u;
    // on: the setting Lower and a HUD swapchain; serial: the newest published wheel replay (0 = none yet); valid: its image is usable
    bool frame(bool on,std::uint64_t serial,bool valid) noexcept {
        if(serial!=seen){seen=serial;age=0;}else if(age<1000000u)++age;
        return on && serial && valid && age<StaleFrames;
    }
};
} // namespace recomp::gpu::wheelrep
