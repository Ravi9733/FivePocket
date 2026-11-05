#pragma once
// hud4 (v597): the second screen's minimap as the game draws it. The plain arithmetic only (no Vulkan, no renderer state), so the
// host tests run this text (local-analysis/agents/hud4/tests). The renderer side is hud_replay_renderer.inc.
//
// What the game draws (Thor censuses hud-blips-1 = census-03 events 1278-1313, auto-094421 census-01 2477-2526):
//  - the RADAR PASS into EDRAM 1440, pitch 560 (+ depth/stencil 1664): a clear of 0..512 to 0, a clear of the map rectangle to the
//    water colour, the map (a 128x128 texture, the vector tiles), a stencil clear and a stencil mask, Scaleform fills (interiors),
//    and an alpha-only quad (the opacity mask); then the resolve of 0..512 (caller 824126fc) into a 512x512 texture;
//  - on the MAIN FRAME (pitch 1280), right after that resolve: the composite (a rect list sampling the radar texture through a
//    several-tap shader, f0ff7fe255dbc811), then, all with the scissor of the minimap box (59,548..249,683 at the default safe
//    zone): a stencil clear and mask, the dark frame, the health / armour / ability bars, the player arrow and the blips (the
//    textured quads of the 512x256 atlas 0f2a0000). None of those is in the radar texture: v596 copied that texture and had
//    neither arrow nor blips.
// v597 keeps a copy of every one of those operations as it executes (Collector below decides which) and replays them, at the
// frame's present, through the renderer's own draw() into shadow targets k/16 times the console's size: the radar pass into a
// shadow radar target, the composite and the box's draws into a shadow overlay target with the composite's texture swapped for
// the shadow radar. The second screen shows that overlay 1:1. Nothing the main frame uses is written.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <optional>

namespace recomp::gpu::hudrep {

// The radar pass's surfaces (EDRAM tile bases, pitch) and its resolve. The host tests override them with defines, so that a
// harness scene's own surfaces can stand in for the radar.
#if defined(RECOMP_HUD4_TEST_RADAR_PITCH)
constexpr unsigned RadarPitch=RECOMP_HUD4_TEST_RADAR_PITCH,RadarColorBase=RECOMP_HUD4_TEST_RADAR_COLOR,RadarDepthBase=RECOMP_HUD4_TEST_RADAR_DEPTH;
constexpr unsigned MainPitch=RECOMP_HUD4_TEST_MAIN_PITCH;
#else
constexpr unsigned RadarPitch=560,RadarColorBase=1440,RadarDepthBase=1664;
constexpr unsigned MainPitch=1280;
#endif
constexpr unsigned MaxOps=192;          // per list (the radar pass has ~20 operations, the box ~15); more = something else: refused
constexpr int Slack=2;                  // guest pixels: a box operation's rectangle may exceed the composite's scissor by this much

struct Rect {std::int32_t x0=0,y0=0,x1=0,y1=0;
    constexpr bool empty() const noexcept {return x1<=x0 || y1<=y0;}
    constexpr bool operator==(const Rect&) const=default;};
constexpr bool inside(const Rect& r,const Rect& box,int slack=Slack) noexcept {
    return !r.empty() && r.x0>=box.x0-slack && r.y0>=box.y0-slack && r.x1<=box.x1+slack && r.y1<=box.y1+slack;
}

// RB_SURFACE_INFO (0x2000) pitch, RB_COLOR_INFO (0x2001) and RB_DEPTH_INFO (0x2002) EDRAM bases.
struct Surfaces {unsigned pitch=0,colorBase=0,depthBase=0;};
constexpr Surfaces surfaces(std::uint32_t surfaceInfo,std::uint32_t colorInfo,std::uint32_t depthInfo) noexcept {
    return {surfaceInfo&0x3FFFu,colorInfo&0xFFFu,depthInfo&0xFFFu};
}
constexpr bool radarSurface(const Surfaces& s) noexcept {
    return s.pitch==RadarPitch && (s.colorBase==RadarColorBase || s.depthBase==RadarDepthBase);
}

// A draw's scissor rectangle as draw() computes it from (PA_SC_WINDOW_OFFSET, PA_SC_WINDOW_SCISSOR_TL, _BR): the window offset
// added unless TL bit 31; no scissor words = the whole 8192 square.
constexpr std::int32_t offset15(std::uint32_t v) noexcept {return static_cast<std::int32_t>(v<<17)>>17;}
constexpr Rect drawScissor(const std::optional<std::array<std::uint32_t,3>>& s) noexcept {
    if(!s)return {0,0,8192,8192};
    const auto& r=*s;
    const std::int32_t ox=offset15(r[0]),oy=offset15(r[0]>>16);
    Rect x{std::int32_t(r[1]&0x7FFF),std::int32_t((r[1]>>16)&0x7FFF),std::int32_t(r[2]&0x7FFF),std::int32_t((r[2]>>16)&0x7FFF)};
    if(!(r[1]>>31)){x.x0+=ox;x.y0+=oy;x.x1+=ox;x.y1+=oy;}
    return x;
}
// A clear's rectangle as clear() computes it: the viewport (x, y, w, h floats), cut by the clear's own rectangle when it has one
// and by the scissor when its test is enabled.
constexpr Rect clearRect(const float viewport[4],const std::optional<std::array<std::int32_t,4>>& own,bool scissored,const Rect& scissor) noexcept {
    Rect r{std::int32_t(viewport[0]),std::int32_t(viewport[1]),std::int32_t(viewport[0]+viewport[2]),std::int32_t(viewport[1]+viewport[3])};
    if(own){r.x0=std::max(r.x0,(*own)[0]);r.y0=std::max(r.y0,(*own)[1]);r.x1=std::min(r.x1,(*own)[2]);r.y1=std::min(r.y1,(*own)[3]);}
    if(scissored){r.x0=std::max(r.x0,scissor.x0);r.y0=std::max(r.y0,scissor.y0);r.x1=std::min(r.x1,scissor.x1);r.y1=std::min(r.y1,scissor.y1);}
    return r;
}
// The window-offset word (PA_SC_WINDOW_OFFSET: x in bits 0-14, y in bits 16-30, two's complement).
constexpr std::uint32_t offsetWord(std::int32_t x,std::int32_t y) noexcept {
    return (static_cast<std::uint32_t>(x)&0x7FFFu)|((static_cast<std::uint32_t>(y)&0x7FFFu)<<16);
}

// Which operations of a frame are kept. One Collector per renderer; every executed draw / clear while the second screen wants
// its HUD goes through it in queue order, then the radar resolve, then the present.
//   Idle     --radar clear of the whole pass (the pass's first operation)-->  Radar  (radar list restarted)
//   Radar    --radar operation-->  Radar (kept)       --the radar resolve-->  Resolved
//   Resolved --main-frame draw sampling the resolved radar (the composite)-->  Box (box = its scissor; overlay list restarted)
//   Box      --main-frame operation inside the box-->  Box (kept)   --main-frame operation outside it-->  Closed
//   any      --the present-->  Idle; the frame is replayed when it reached Box or Closed with both lists complete.
// Operations on other surfaces never change the phase (the game may draw elsewhere in between). A radar clear of the whole pass
// in any phase restarts the radar list (a second radar pass before the present replaces the first, as its resolve would).
enum class Phase : std::uint8_t {Idle,Radar,Resolved,Box,Closed};
enum class Keep : std::uint8_t {No,Radar,Overlay};
struct Collector {
    Phase phase=Phase::Idle;Rect box{};std::uint32_t radarAddress=0;
    unsigned radarOps=0,overlayOps=0;bool overflow=false,broken=false;
    // A draw or clear on the radar's surfaces. startClear: a colour clear of the whole resolved area (0..512 square).
    Keep radarOp(bool startClear) noexcept {
        if(startClear){phase=Phase::Radar;radarOps=0;overlayOps=0;overflow=false;broken=false;}
        if(phase!=Phase::Radar)return Keep::No;
        if(radarOps>=MaxOps){overflow=true;return Keep::No;}
        ++radarOps;return Keep::Radar;
    }
    void radarResolved(std::uint32_t address) noexcept {
        if(phase!=Phase::Radar || !radarOps)return;
        phase=Phase::Resolved;radarAddress=address;
    }
    // A draw or clear on the main frame (pitch MainPitch). rect: its scissor (draw) / clear rectangle (clear); samplesRadar: a draw
    // whose texture fetches name the resolved radar address.
    Keep mainOp(const Rect& rect,bool samplesRadar) noexcept {
        const Keep k=peekMain(rect,samplesRadar);
        if(phase==Phase::Resolved) {
            if(k==Keep::Overlay){phase=Phase::Box;box=rect;overlayOps=1;}
            return k;
        }
        if(phase!=Phase::Box)return Keep::No;
        if(!inside(rect,box)){phase=Phase::Closed;return Keep::No;}
        if(overlayOps>=MaxOps){overflow=true;phase=Phase::Closed;return Keep::No;}
        ++overlayOps;return Keep::Overlay;
    }
    // hud5 (Thor edition): what mainOp would answer for this operation, without changing anything. The renderer asks it BEFORE a
    // main-frame operation executes: in the "Bottom screen only" mode an operation it keeps for the overlay is the game's minimap
    // box (the composite and every operation inside the box) and is not drawn on the main frame; mainOp still copies it after.
    Keep peekMain(const Rect& rect,bool samplesRadar) const noexcept {
        if(phase==Phase::Resolved)return samplesRadar && !rect.empty()?Keep::Overlay:Keep::No;
        if(phase!=Phase::Box || !inside(rect,box) || overlayOps>=MaxOps)return Keep::No;
        return Keep::Overlay;
    }
    // A kept operation that could not be copied (budget, unexpected input): the frame is not replayed.
    void fail() noexcept {broken=true;}
    // The present: whether this frame is replayed; the collector restarts either way.
    bool present() noexcept {
        const bool replay=(phase==Phase::Box || phase==Phase::Closed) && radarOps && overlayOps && !overflow && !broken;
        phase=Phase::Idle;radarOps=overlayOps=0;overflow=broken=false;
        return replay;
    }
};

// ---- geometry of the replay
inline constexpr int One=16;
constexpr std::uint32_t roundUp16(std::int32_t v) noexcept {return static_cast<std::uint32_t>((std::max(v,0)+15)&~15);}
// The overlay target: the box moved to the origin by a window offset of whole 16-pixel tiles (the render scale's edges then fall on
// whole host pixels exactly as the main frame's would: render_scale.h), and the guest size that holds the moved box.
struct Overlay {std::int32_t ox=0,oy=0;std::uint32_t w=0,h=0;};
constexpr Overlay overlayFor(const Rect& box) noexcept {
    Overlay o;
    o.ox=-(box.x0>=0?(box.x0&~15):0);o.oy=-(box.y0>=0?(box.y0&~15):0);
    o.w=roundUp16(box.x1+Slack+o.ox);o.h=roundUp16(box.y1+Slack+o.oy);
    return o;
}
// The radar pass's area: what its operations' rectangles reach (guest pixels, whole tiles), at most the resolved size.
constexpr std::uint32_t radarArea(std::int32_t reach,std::uint32_t resolved) noexcept {return std::min(roundUp16(reach),resolved);}
// The scale (sixteenths) for a second screen of W x H showing a box of bw x bh guest pixels: the largest that keeps the box inside
// 94 % of the width and, with a star row above it (StarRowK of the box's height), of the height. At least 16 (1:1), at most MaxK.
inline constexpr int MaxK=192; // (hud6 v618: the full-screen map needs ~10x: 159 at 1240x1080)
inline constexpr float StarRowK=0.16f,FillK=0.94f;
constexpr int scaleFor(std::uint32_t screenW,std::uint32_t screenH,std::int32_t bw,std::int32_t bh) noexcept {
    if(bw<=0 || bh<=0 || !screenW || !screenH)return One;
    const float kw=FillK*float(screenW)/float(bw),kh=FillK*float(screenH)/(float(bh)*(1.0f+StarRowK));
    const int k=static_cast<int>(16.0f*std::min(kw,kh));
    return std::clamp(k,One,MaxK);
}
// render_scale.h's edge rule, for a guest edge g at scale k (floor((g*k + 7) / 16)).
constexpr std::int32_t edge(std::int32_t g,int k) noexcept {
    const long long v=static_cast<long long>(g)*k+7;
    return static_cast<std::int32_t>(v>=0?v/16:-((-v+15)/16));
}
// The box in the overlay image's host texels.
constexpr Rect hostBox(const Rect& box,const Overlay& o,int k) noexcept {
    return {edge(box.x0+o.ox,k),edge(box.y0+o.oy,k),edge(box.x1+o.ox,k),edge(box.y1+o.oy,k)};
}

// ---- hud5 (Thor edition, v604): the game's own wanted stars, and the Minimap screens setting
// The star draws (Thor census hud-stars-1 = run 83s_u7ay census n=6, wanted 2; n=13, wanted 1; none in n=5/7/12 at wanted 0): on the
// MAIN frame, before the radar pass, viewport and scissor the whole 1280x720 picture (the HUD movie), shader pair
// vs 21d938367b02896f / ps 0c7341b616776c5a (PS_GFill_ColorPremult: vertex-colour shapes, blend SRC_ALPHA / INV_SRC_ALPHA, alpha
// replaced), triangle lists:
//   one draw of 1212 indices / 268 vertices (the five star shapes; at wanted 1 and 2 alike), then per lit star one pair
//   264 / 60 and 84 / 20 (wanted 2: two pairs, wanted 1: one pair).
// So the game itself decides which stars are lit (it fills them from the right) and how they flash; the replay draws exactly
// those draws. Probes at y 50: the five shapes touch x 1170 and 1200 and (y 40) 1170, not 1110 / 1140 / 1230 or y 60: the row
// lies inside StarRegion below. A run is the 1212 draw and the pairs straight after it; any other main-frame operation ends it.
inline constexpr unsigned StarHeadIndices=1212,StarFillIndices=264,StarEdgeIndices=84,StarMaxOps=1+2*5;
// The guest rectangle the star replay renders (whole 16-pixel tiles round the row) and the crop shown until the replay measured
// the row itself (hud_replay_renderer.inc: one read-back of the first star frame; RENDER_HUD5 stars_bbox).
inline constexpr Rect StarRegion{1120,32,1248,64},StarCropDefault{1136,36,1236,62};
enum class StarKeep : std::uint8_t {No,Star};
struct StarCollector {
    bool open=false;unsigned ops=0;bool overflow=false;
    // A main-frame draw: its index count (0 = not indexed), primitive type (4 = triangle list), scissor rectangle.
    StarKeep draw(unsigned indices,unsigned primitive,const Rect& scissor) noexcept {
        const bool covers=scissor.x0<=StarRegion.x0 && scissor.y0<=StarRegion.y0 && scissor.x1>=StarRegion.x1 && scissor.y1>=StarRegion.y1;
        if(primitive==4 && covers && indices==StarHeadIndices){open=true;ops=1;overflow=false;return StarKeep::Star;} // (a later head restarts)
        if(!open)return StarKeep::No;
        if(primitive==4 && covers && (indices==StarFillIndices || indices==StarEdgeIndices)) {
            if(ops>=StarMaxOps){overflow=true;open=false;return StarKeep::No;}
            ++ops;return StarKeep::Star;
        }
        open=false;return StarKeep::No;
    }
    // Any other main-frame operation (a clear): the run ends.
    void other() noexcept {open=false;}
    // The present: the number of star operations of this frame (0: no stars drawn = none shown); restarts.
    unsigned present() noexcept {const unsigned n=overflow?0u:ops;open=false;ops=0;overflow=false;return n;}
};
// The window offset that moves StarRegion's corner to (0, top) of the overlay (whole tiles): stars below the box's area.
constexpr std::int32_t starOffsetX() noexcept {return -StarRegion.x0;}
constexpr std::int32_t starOffsetY(std::uint32_t top) noexcept {return std::int32_t(top)-StarRegion.y0;}
// The overlay holds the box (overlayFor) and, from row `top` = the box overlay's height, the star band (StarRegion's size).
constexpr std::uint32_t starBandTop(const Overlay& o) noexcept {return o.h;}
constexpr std::uint32_t overlayWidthWithStars(const Overlay& o) noexcept {return std::max<std::uint32_t>(o.w,roundUp16(StarRegion.x1-StarRegion.x0));}
constexpr std::uint32_t overlayHeightWithStars(const Overlay& o) noexcept {return o.h+roundUp16(StarRegion.y1-StarRegion.y0);}
// A guest crop of the star row in the overlay's host texels (the band starts at row `top`).
constexpr Rect starHostCrop(const Rect& crop,std::uint32_t top,int k) noexcept {
    return {edge(crop.x0-StarRegion.x0,k),edge(crop.y0-StarRegion.y0+std::int32_t(top),k),edge(crop.x1-StarRegion.x0,k),
            edge(crop.y1-StarRegion.y0+std::int32_t(top),k)};
}
// hud5's scale: the box as wide as the second screen allows (MarginPx each side) with the star row (StarRowGuest guest rows and
// a StarGapGuest gap, the default crop's height) above it inside the height. Replaces hud4's 94 % fill (the owner: "increase
// the size of the map a bit more"): 1240x1080 -> k 103 (box 190x135 guest -> about 1223x869), hud4 had 97.
inline constexpr int MarginPx=4,StarRowGuest=StarCropDefault.y1-StarCropDefault.y0,StarGapGuest=3;
constexpr int scaleFor5(std::uint32_t screenW,std::uint32_t screenH,std::int32_t bw,std::int32_t bh) noexcept {
    if(bw<=0 || bh<=0 || screenW<=std::uint32_t(4*MarginPx) || screenH<=std::uint32_t(4*MarginPx))return One;
    const long long kw=16ll*(std::int64_t(screenW)-2*MarginPx)/bw,kh=16ll*(std::int64_t(screenH)-2*MarginPx)/(bh+StarRowGuest+StarGapGuest);
    return int(std::clamp<long long>(std::min(kw,kh),One,MaxK));
}

// ---- hud6 (Thor edition, v618; the owner: "fill the map to the whole screen, then have the stars overlay smaller in the top right
// corner"): the lower screen shows the map INSIDE the box, cropped to the screen's aspect and rendered at a scale where that crop covers
// the screen texel for texel (no enlargement), the box's bar strip along the bottom edge, the game's stars small in the top-right corner.
// The composite (census n=6 D3207, VR line: a rect from (59,548) to (250,684) with texcoords 0..190.5 / 0..135.5 of the 512 radar
// texture) puts radar texel t at main-frame pixel (box.x0 + 1) + t: the map (radar texels RadarMap = (10,13)-(180,122), the only area
// its tiles are scissored to) is the main frame's (69,561)-(239,670); the bars run below it, to the box's bottom (rows 670..684).
inline constexpr Rect RadarMap{10,13,180,122};
constexpr Rect mapInterior(const Rect& box) noexcept {return {box.x0+1+RadarMap.x0,box.y0+1+RadarMap.y0,box.x0+1+RadarMap.x1,box.y0+1+RadarMap.y1};}
// The bar strip: rows 671..682 at the default safe zone (a Thor screenshot, 18:26:09: map to 670, a dark band, the bars 673..679, a
// dark band, the box's edge at 683), the map's width.
constexpr Rect barStrip(const Rect& box) noexcept {const Rect m=mapInterior(box);return {m.x0,m.y1+1,m.x1,std::min<std::int32_t>(box.y1,m.y1+12)};}
// The scale (sixteenths) at which the map interior covers a W x H screen in both directions (the crop is then at least W x H texels).
constexpr int scaleFor6(std::uint32_t screenW,std::uint32_t screenH,const Rect& map) noexcept {
    const std::int64_t mw=map.x1-map.x0,mh=map.y1-map.y0;
    if(mw<=0 || mh<=0 || !screenW || !screenH)return One;
    const std::int64_t kw=(16*std::int64_t(screenW)+mw-1)/mw+1,kh=(16*std::int64_t(screenH)+mh-1)/mh+1; // (+1: the edge rule's rounding)
    return int(std::clamp<std::int64_t>(std::max(kw,kh),One,MaxK));
}
// A main-frame guest rectangle in the overlay's host texels.
constexpr Rect hostRect(const Rect& r,const Overlay& o,int k) noexcept {return {edge(r.x0+o.ox,k),edge(r.y0+o.oy,k),edge(r.x1+o.ox,k),edge(r.y1+o.oy,k)};}

// ---- hud8 (Thor edition, v625; the owner on v623: "The map is a bit glitched out, and zoomed just a tad too much"). v620-v623 moved the
// radar list down 48 rows and showed the map's rows -7..142: the device frame (thor-edition/v623-lower-map-glitch.png) has a band of the
// water colour exactly where the target's rows < 0 land (screen y 0..46 = rows -6.4..0 at k 117): those rows are outside the game's
// 512x512 viewport, so the clip volume drops them; the 20 rows added below were all drawn (the game submits map tiles beyond its scissor).
// So the lower screen now shows only rows >= 0 and needs no move: the region the composite samples (radar texels 0..190 x 0..135 = the
// whole box, 10 columns each side of the map) at its full width (k 105 at 1240: 1247 texels, 3.5 off each edge), from row 0 down as far as
// the screen's height reaches (165 rows). Every draw the game confines to the map rectangle, or to the box region (0,0)-(190,135), is drawn
// with the scissor of that taller region (the tiles the game submitted beyond it); the water clear of the map rectangle covers it too.
// The box's own draws are shown in place over it (the whole box: blips, the arrow, north, the bars where the game puts them, a blip pinned
// over the bars on top of them); their faint frame shading (the inner shadow round the map, alpha ~0.2) is left out (hud_screen.frag).
inline constexpr Rect RadarRegion{0,0,190,135};
constexpr int scaleFor8(std::uint32_t screenW) noexcept {
    if(!screenW)return One;
    for(int k=One;k<=MaxK;++k)if(edge(RadarRegion.x1,k)-edge(RadarRegion.x0,k)>=std::int32_t(screenW))return k;
    return MaxK;
}
// The rows (from row 0) that cover screenH texels at scale k (at most 512: the radar target).
constexpr std::int32_t shownRows(std::uint32_t screenH,int k) noexcept {
    for(std::int32_t r=1;r<=512;++r)if(edge(r,k)-edge(0,k)>=std::int32_t(screenH))return r;
    return 512;
}
constexpr Rect tallRegion(std::int32_t rows) noexcept {return {RadarRegion.x0,0,RadarRegion.x1,std::max(rows,RadarRegion.y1)};}
// The screen's crop of the radar shadow (host texels: cx0, cy0 = 0, w, h) and where the box layer (the radar region's texels) lands on it.
struct MapCrop {std::int32_t cx0=0,cy0=0,w=0,h=0,atX=0,atY=0,bw=0,bh=0;};
constexpr MapCrop mapCrop(std::uint32_t screenW,std::uint32_t screenH,int k,std::int32_t rows) noexcept {
    const std::int32_t rw=edge(RadarRegion.x1,k)-edge(RadarRegion.x0,k),rh=edge(rows,k);
    MapCrop c;
    c.cx0=std::max<std::int32_t>(0,(rw-std::int32_t(screenW))/2);c.cy0=0;
    c.w=std::min<std::int32_t>(std::int32_t(screenW),rw-c.cx0);c.h=std::min<std::int32_t>(std::int32_t(screenH),rh);
    c.atX=-c.cx0;c.atY=0;c.bw=rw;c.bh=edge(RadarRegion.y1,k);
    return c;
}
// ---- hud8b (v626, main's decision): the bars at the screen's bottom edge, nothing below them. The window's bottom = the box's bottom
// (radar row 135); every extra row comes from ABOVE the map rectangle, including rows above the radar target's row 0. Those rows are
// outside the game's viewport (v623's band), so the replay moves the whole radar pass down by radarShift(...) rows in CLIP space
// (XenosRecomp's vertex epilogue adds g_HalfPixelOffset * w to every position: draw() adds rows / ys there while the radar list is
// replayed, generic_renderer.cpp hud8ShiftHalfPixel), its scissors and clears by the same rows: the game's tiles above row 0 then land
// inside the target and its clip volume. The crop's top is then never above the shifted target's row 0.
constexpr std::int32_t rowsAbove(std::uint32_t screenH,int k) noexcept {return std::max<std::int32_t>(0,shownRows(screenH,k)-RadarRegion.y1);}
// ---- hud9 (v631; v628 device frame thor-edition/v628-lower-1.png): the radar is a tilted plane; its far edge (the horizon: a sky
// gradient, a blue band, sand) reached the screen's top 60 px = the game's target rows about -32..-23 (k 105, shift 32). Only rows down
// from MaxRowsAbove above row 0 are taken (7 rows of margin below that horizon); the height is made up by a slightly larger scale:
// scaleFor9 = the smallest k >= scaleFor8 whose rows 0..135 plus MaxRowsAbove cover the screen (1240x1080: k 115, the radar region's
// columns 8.7..181.3 visible: the map rectangle 10..180 whole, more than v623's 10..180 at k 117).
inline constexpr std::int32_t MaxRowsAbove=16;
constexpr int scaleFor9(std::uint32_t screenW,std::uint32_t screenH) noexcept {
    for(int k=scaleFor8(screenW);k<=MaxK;++k)if(rowsAbove(screenH,k)<=MaxRowsAbove)return k;
    return MaxK;
}
constexpr std::int32_t radarShift(std::uint32_t screenH,int k) noexcept {return std::min<std::int32_t>(rowsAbove(screenH,k)+2,96);}
// ---- hud10 (v634, thor-wheel; owner on v632: "too zoomed in, the map needs to resemble the GTA map with the health bars in the right
// place"; main's option (a)): the box's full width across the screen again (scaleFor8: 1240 -> k 105, the radar region's 190 columns in
// 1247 texels, 3.5 off each edge, so the blips the game pins to the box's left/right edges show whole), the box's bottom (the bars) at
// the screen's bottom, the map rows above it only as far as real tiles reach (MaxRowsAbove above row 0, + 2 rows of the move's margin:
// guest row -18 at the top, as v631/v632), the rest of the top strip black; the game's stars over its top-right corner.
constexpr int scaleFor10(std::uint32_t screenW) noexcept {return scaleFor8(screenW);}
constexpr std::int32_t radarShift10(std::uint32_t screenH,int k) noexcept {return std::min<std::int32_t>(rowsAbove(screenH,k),MaxRowsAbove)+2;}
// ---- hud11 (v635, thor-wheel; the owner's reference: "the health bars and grey/black borders should fit the screen like normal. The stars
// can be overlayed on top of the map with no black bars"). The lower screen IS the game's minimap box: the game's own frame (its fill and
// ring, replayed into a frame band of the overlay) at the screen's four edges (9 slices: corners and top / bottom edges texel for
// texel, the side edges stretched along their uniform length); the map at k 105 (scaleFor10: the box's 190 columns across the screen,
// edge-pinned blips whole) filling the whole interior texel for texel, from Hud11TopRow (MaxRowsAbove above row 0: below the radar
// plane's horizon) down past the box's bottom (the game submits the tiles below the map rectangle; the radar pass's tall scissor
// reaches shownRows); the box's own draws in two pieces: rows < Hud11Cut in place on the map, rows >= Hud11Cut (the bars, the bottom
// border band, the blips the game pins over the bars) at the screen's bottom; the game's stars top-right inside the border.
inline constexpr std::int32_t Hud11TopRow=-MaxRowsAbove,Hud11Cut=122; // (v638 hud13: the cut at the map rectangle's bottom: the pieces stay aligned)
inline constexpr Rect Hud11Slices{12,14,178,121}; // the frame band's 9-slice inner rectangle (radar-region guest pixels)
// hud12 (v637, the owner's target picture thor-edition/owner-target-lower-2026-10-23.jpg = v636's interior 1:1 in a 14-px black frame,
// the bars band across): no frame band (the game's ring showed inset), a thin black frame, the bars band from Hud12BandRow down.
inline constexpr bool Hud11FrameBand=false;
// ---- hud13 (v638; v637 device frame thor-edition/v637-lower-out.png: the game's GPS strip, which sits at the map's bottom right above the
// bars, showed twice and split, with a dark band - the radar rows the game keeps under the strip and the bars band - between, because
// v635-v637 showed the box's rows >= 116 13.6 rows below their map place). main's choice (1) + no horizon: the box ALIGNED - the map and
// every one of the box's draws in the game's exact places, the box's bottom (the bars) at the screen's bottom, the extra rows from ABOVE -
// at the smallest k whose rows above row 0 stay within Hud13MaxRowsAbove (22: below the horizon seen at rows -23..-32); 1240x1080: k 111.
inline constexpr std::int32_t Hud13MaxRowsAbove=22;
constexpr int scaleFor13(std::uint32_t screenW,std::uint32_t screenH) noexcept {
    for(int k=scaleFor8(screenW);k<=MaxK;++k)if(rowsAbove(screenH,k)<=Hud13MaxRowsAbove)return k;
    return MaxK;
}
inline constexpr float Hud12FramePx1080=14.0f,Hud12BandRow=121.5f;
// v638 (second-screen.txt look=game): the main-frame rectangle whose pixels (the world under the minimap, captured just before the game's
// composite draws the box) are the lower screen's backdrop: the box's width at the lower screen's aspect, ending at the box's bottom.
constexpr Rect backdropRegion(const Rect& box,std::uint32_t screenW,std::uint32_t screenH) noexcept {
    const std::int32_t w=box.x1-box.x0;if(w<=0 || !screenW || !screenH)return {};
    const std::int32_t h=std::int32_t((std::int64_t(w)*screenH+screenW/2)/screenW);
    return {box.x0,std::max<std::int32_t>(0,box.y1-h),box.x1,box.y1};
}
// The frame band: guest rows below the star band of the overlay, as tall as the box.
constexpr std::uint32_t frameBandTop(const Overlay& o) noexcept {return overlayHeightWithStars(o);}
constexpr std::uint32_t overlayHeightWithFrame(const Overlay& o) noexcept {return frameBandTop(o)+o.h;}
// The screen's crop of the shifted radar shadow: rows from Hud11TopRow, the screen's height; the box's place on it.
constexpr MapCrop mapCropHud11(std::uint32_t screenW,std::uint32_t screenH,int k,std::int32_t shift) noexcept {
    const std::int32_t rw=edge(RadarRegion.x1,k)-edge(RadarRegion.x0,k);
    MapCrop c;
    c.cx0=std::max<std::int32_t>(0,(rw-std::int32_t(screenW))/2);c.w=std::min<std::int32_t>(std::int32_t(screenW),rw-c.cx0);
    c.cy0=edge(std::max<std::int32_t>(0,shift+Hud11TopRow),k);c.h=std::int32_t(screenH);
    c.atX=-c.cx0;c.atY=edge(shift,k)-c.cy0;c.bw=rw;c.bh=edge(RadarRegion.y1+shift,k)-edge(shift,k);
    return c;
}
// The rows of the shifted radar shadow the crop needs (its reach): the crop's last row.
constexpr std::int32_t hud11Reach(std::uint32_t screenH,int k,std::int32_t shift) noexcept {
    return std::max<std::int32_t>(0,shift+Hud11TopRow)+shownRows(screenH,k)+1;
}
// The clip-space term that moves a draw's window position `rows` down: the viewport maps window_y = yo + ys * ndc_y, so ndc += rows / ys;
// a screen-space draw (no viewport transform) takes pixels.
constexpr float ndcShift(float rows,float ys,bool screenSpace) noexcept {return screenSpace?rows:(ys!=0.0f?rows/ys:0.0f);}
// The crop (shifted radar host texels): the box's bottom at the screen's bottom; the box layer's place on it.
constexpr MapCrop mapCropBottom(std::uint32_t screenW,std::uint32_t screenH,int k,std::int32_t shift) noexcept {
    const std::int32_t rw=edge(RadarRegion.x1,k)-edge(RadarRegion.x0,k),bottom=edge(RadarRegion.y1+shift,k);
    MapCrop c;
    c.cx0=std::max<std::int32_t>(0,(rw-std::int32_t(screenW))/2);c.w=std::min<std::int32_t>(std::int32_t(screenW),rw-c.cx0);
    c.cy0=std::max<std::int32_t>(0,bottom-std::int32_t(screenH));c.h=bottom-c.cy0;
    c.atX=-c.cx0;c.atY=edge(shift,k)-c.cy0;c.bw=rw;c.bh=edge(RadarRegion.y1+shift,k)-edge(shift,k);
    return c;
}
// The minimap's frame on the main frame (census n=5/6/7/12/13: every box starts, after its stencil mask, with a 48-index / 12-vertex fill
// and a 396-index / 88-vertex rounded rectangle; then the bars (30/8 rects), the blips, the arrow, north): the lower screen leaves out
// the first of each (the thick black frame and its dark margin round the map; v628's picture), keeping every other box draw.
inline constexpr unsigned FrameFillIndices=48,FrameRingIndices=396;
// hud15 (v695 -> v801, thor-radio; the owner 2026-10-24: "whenever damage is taken on the thor, the bottom screen flashes red but the
// borders are off"; v693 device frame agents/thor-radio/shots/fl1-2-lower.png: a red glow exactly over the game's map rectangle, short of
// the lower screen's frame). The box: the composite, a depth/stencil clear, the stencil mask (its first 6-index draw), the map's
// backdrop fill (the slot after the mask: 48/12 outdoors - left out since v631 -, 180/40 indoors, 144/108/72 between; constant colour
// 0x333399: v697 RENDER_HUD15), the map overlays up to the frame ring (396, left out), then the bars, the damage flash and the blips.
// Refuted on the device: v695 (the flash = a non-48 slot: the indoor fill), v698 (a white centred stencil-masked overlay before the
// ring), v801 / v802 (two white 30/8 fills after the bars). v803 (run y35106ol: RENDER_HUD16 dumps the replayed frames before and after
// the health drop at 19:20:44, every radar-pass and box op): the frame with the glow (shots/fh-5..8-lower.png) adds, among others, ONE
// textured draw - 30 indices / 8 vertices, PS_GFill_1Texture, its only texture a 64 x 64 image at 1dcd2000 (the blips use the 512 x 256
// atlas at 0f2a0000) - the soft glow image stretched over the map rectangle; v698 / v801 / v802 left it in place every time.
// flashTexture picks it; the lower screen draws it alone in the flash band and stretches it to its map area inside the frame
// (replicaLayout11). Everything else is drawn in place.
// A 48-index draw after the slot is kept (v631-v693 left out the first 48 anywhere).
inline constexpr unsigned FrameMaskIndices=6;
enum class BoxRole : std::uint8_t {Keep,Drop};
struct FrameFilter {bool fill=false,ring=false,mask=false,slot=false;
    // A box operation (its index count; a clear never takes a role): what the lower screen does with it.
    constexpr BoxRole role(std::size_t indices,bool clear) noexcept {
        if(clear)return BoxRole::Keep;
        if(mask && !slot) {
            slot=true;
            if(indices==FrameFillIndices){fill=true;return BoxRole::Drop;}
            if(indices==FrameRingIndices){ring=true;return BoxRole::Drop;}
            return BoxRole::Keep; // (the fill in another shape - indoors: in place, as v693)
        }
        if(!mask && indices==FrameMaskIndices){mask=true;return BoxRole::Keep;}
        if(!mask && !fill && indices==FrameFillIndices){fill=true;return BoxRole::Drop;}
        if(!ring && indices==FrameRingIndices){ring=true;return BoxRole::Drop;}
        return BoxRole::Keep;
    }
    // (v631's form, the hud5 census test's: indices only, 0 = a clear) true = left out.
    constexpr bool drop(std::size_t indices) noexcept {return role(indices,indices==0)==BoxRole::Drop;}
};
// The flash test of one box draw: a textured 30-index / 8-vertex rectangle whose only texture is FlashTexture x FlashTexture texels.
inline constexpr unsigned FlashIndices=30,FlashVertices=8,FlashTexture=64;
constexpr bool flashTexture(std::size_t indices,unsigned vertices,unsigned textures,unsigned width,unsigned height) noexcept {
    return indices==FlashIndices && vertices==FlashVertices && textures==1 && width==FlashTexture && height==FlashTexture;
}
// RC1.1 (v809; owner 2026-10-24 ~22:05: "wanted flash is not fully filling the screen on the thor"): v808's wanted-level dump (run
// w0vqizm0, the cheat at 00:03:37 device time, RENDER_HUD16) - the frames with the flash add ONE untextured 30/8 fill after the bars
// (PS_GFill_ColorPremult; its 8 vertices one colour, 0x5050c2 in the blue phase, the alpha 0 on the edge ring and 255 inside, raw
// extent -60..20735 x -60..13360, long / short 1.550 - the map rectangle) - drawn in place, the map rectangle only (shots/wq-grid.png).
// wantedRect picks it: 30/8, one colour on every vertex and not white (the every-frame white rect, 1.593, is not it), an alpha gradient,
// the raw extent's long / short = WantedAspect within FlashAspectTolerance.
inline constexpr float WantedAspect=20795.0f/13420.0f,FlashAspectTolerance=0.03f;
inline bool wantedRect(const std::uint8_t* v,std::size_t indices,unsigned count) noexcept {
    if(!v || indices!=FlashIndices || count!=FlashVertices)return false;
    int a0=32767,a1=-32768,b0=32767,b1=-32768;unsigned alo=255,ahi=0;
    for(unsigned i=0;i<count;++i) {
        const std::uint8_t* q=v+std::size_t(i)*8u;
        const int a=std::int16_t(std::uint16_t((q[0]<<8)|q[1])),b=std::int16_t(std::uint16_t((q[2]<<8)|q[3]));
        a0=std::min(a0,a);a1=std::max(a1,a);b0=std::min(b0,b);b1=std::max(b1,b);
        if(q[5]!=v[5] || q[6]!=v[6] || q[7]!=v[7])return false; // (one colour)
        alo=std::min<unsigned>(alo,q[4]);ahi=std::max<unsigned>(ahi,q[4]);
    }
    if(v[5]==0xFF && v[6]==0xFF && v[7]==0xFF)return false; // (white: the every-frame rect)
    if(!(ahi>alo) || !(a1>a0) || !(b1>b0))return false;
    const float w=float(a1-a0),h=float(b1-b0),r=std::max(w,h)/std::min(w,h);
    return r>=WantedAspect*(1.0f-FlashAspectTolerance) && r<=WantedAspect*(1.0f+FlashAspectTolerance);
}
// hud15: the flash band is the frame band (frameBandTop: below the star band, as tall as the box; unused since hud12).
inline constexpr bool Hud15FlashBand=true;
// Every Nth game frame is replayed for the lower screen (second-screen.txt rate=1..4; default 2: half the replay cost per frame).
inline constexpr unsigned DefaultRate=2,MaxRate=4;

// files/settings/minimap.txt (written by the Thor edition app's Minimap row): "both", "top" (the app opens no lower-screen HUD; the
// library never sees a second window), "bottom" (the minimap box is replayed on the lower screen and its operations are not drawn on
// the main frame; v656: the DEFAULT - owner 2026-10-23 "I want to see everything on the second screen nothing on main screen" - for an
// absent file and any other content, as the app's row reads it). Letters in any case, blanks around the word, < 64 bytes.
enum class MapMode : std::uint8_t {Both,Top,Bottom};
inline MapMode parseMapMode(const char* s,std::size_t n) noexcept {
    if(!s || n>=64)return MapMode::Bottom;
    std::size_t i=0;while(i<n && (s[i]==' ' || s[i]=='\t' || s[i]=='\n' || s[i]=='\r'))++i;
    std::size_t e=n;while(e>i && (s[e-1]==' ' || s[e-1]=='\t' || s[e-1]=='\n' || s[e-1]=='\r'))--e;
    const auto is=[&](const char* w){std::size_t k=0;for(;w[k];++k){if(i+k>=e)return false;char c=s[i+k];if(c>='A' && c<='Z')c=char(c-'A'+'a');if(c!=w[k])return false;}
        return i+k==e;};
    return is("top")?MapMode::Top:is("both")?MapMode::Both:MapMode::Bottom; // (v656: anything else = Bottom)
}

} // namespace recomp::gpu::hudrep
