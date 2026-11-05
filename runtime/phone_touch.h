#pragma once
// v828 (thor-radio, Thor edition only; included by map_tap.h and the host test). Owner 2026-10-25 on v826: "the phone is great, but
// maybe make it touch like the weapon wheel or the radio so it feels like an actual phone". A tap ON an app icon or a list row selects
// that item: the library reads the phone screen it already copies (the 256 x 256 movie, RGBA8), finds the current highlight, and
// presses the D-pad steps to the tapped item, then A. Pure (no Vulkan): the host test runs it on movies taken from real device shots.
//  - The tap arrives as the app's lower-screen position (shares of the lower screen, 8 bits each: PhoneTapAtCode in the right stick);
//    the library maps it into the phone screen's rectangle (wheel_screen_layout.h phoneLayout, portrait or turned) and from there into
//    DISPLAY coordinates s, t in 0..1 of the screen as the player sees it (a landscape app: the movie read turned back, as the shader).
//  - Home grid (3 x 3, row 2 holds two apps; v815 shots pa-*, 2026-10-25 ph2 / ph3 / q4): the highlight is a bluish-grey frame
//    (b - r >= 12, luma >= 100) round the selected icon; the cell whose square ring (GridRing0..GridRing1 texels) holds the most such
//    pixels (>= GridMinFrame) is the selection. Steps: rows first (UP / DOWN), then columns (LEFT / RIGHT), no wrap, then A.
//  - A list (Contacts, Email, ...): the selected row is a blue band (b >= 150, b - r >= 90: the title bar's blue is greyer) across the
//    screen; rows are as tall as the band. Steps: (tap row - band row) DOWN or UP, then A. A tap on the status / title area (the top
//    HeaderShare of the display) or the soft-key bar (below FooterShare) does nothing (the app sends the bar's Back itself).
//  - Anything else (no frame, no band): a plain A, as v815.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "phone_call.h" // v852: the call state (ringing / in a call) for plan(call)
namespace recomp::gpu::phonetouch {
constexpr std::uint32_t MovieW=256,MovieH=256;
constexpr int CellU[3]{54,129,203},CellV[3]{87,140,194},CellReach=37,GridRing0=27,GridRing1=33;constexpr unsigned GridMinFrame=150;
constexpr float BandMinShare=0.5f,HeaderShare=0.23f,FooterShare=0.88f;
// v830 (device 2026-10-25, v828 run v2ve3p2v: a landscape app's Back could not be tapped - the app's Back zone is the portrait layout's):
// the soft-key bar's right part (display t > FooterShare, s >= BackShareX) = B, in either orientation (the app keeps its own portrait zone).
constexpr float BackShareX=0.6f;constexpr unsigned BandMinRows=3,MaxSteps=12;
enum class Key:std::uint8_t {Up,Down,Left,Right,A,B};
enum class Kind:std::uint8_t {Plain,Grid,List,Ignore,Back,Answer,Decline}; // (v852: Answer / Decline while ringing or in a call)
struct Plan {Kind kind=Kind::Plain;int fromRow=-1,fromCol=-1,toRow=-1,toCol=-1;std::vector<Key> keys;};
// Packed by the app (WheelGesture.java phoneTapPacked): x share in the high byte, y share in the low byte, 0..255 each.
constexpr std::int16_t pack(float x,float y) noexcept {
    const int a=int(std::clamp(x,0.0f,1.0f)*255.0f+0.5f),b=int(std::clamp(y,0.0f,1.0f)*255.0f+0.5f);
    return std::int16_t(std::uint16_t((a<<8)|b));
}
constexpr void unpack(std::int16_t v,float& x,float& y) noexcept {
    const auto u=std::uint16_t(v);x=float(u>>8)/255.0f;y=float(u&255u)/255.0f;
}
// A movie pixel for display coordinates (s, t in 0..1 as the player sees the screen): portrait = the texel itself; turned = the movie
// holds the app a quarter clockwise, so the display's (s, t) is the movie's (1 - t, s) (hud_screen.frag flag 32).
struct Movie {
    const std::uint8_t* rgba=nullptr;std::uint32_t w=MovieW,h=MovieH,pitch=MovieW*4;bool turned=false;
    const std::uint8_t* at(float s,float t) const noexcept {
        const float u=turned?1.0f-t:s,v=turned?s:t;
        const std::uint32_t x=std::min(w-1,std::uint32_t(std::max(0.0f,u)*float(w))),y=std::min(h-1,std::uint32_t(std::max(0.0f,v)*float(h)));
        return rgba+std::size_t(y)*pitch+std::size_t(x)*4;
    }
    const std::uint8_t* texel(int x,int y) const noexcept {return rgba+std::size_t(std::clamp(y,0,int(h)-1))*pitch+std::size_t(std::clamp(x,0,int(w)-1))*4;}
};
constexpr bool framePixel(const std::uint8_t* p) noexcept {return int(p[2])-int(p[0])>=12 && 0.299f*p[0]+0.587f*p[1]+0.114f*p[2]>=100.0f;}
constexpr bool bandPixel(const std::uint8_t* p) noexcept {return p[2]>=150 && int(p[2])-int(p[0])>=90;} // (the title bar, 116,137,178: b - r 62, not)
// The grid's selected cell (row, col), or -1 when no cell has a clear frame (portrait movies only: the home screen).
inline int gridSelection(const Movie& m,unsigned* frame=nullptr) noexcept {
    unsigned best=0;int at=-1;
    for(int r=0;r<3;++r)for(int c=0;c<3;++c) {
        unsigned n=0;
        for(int dv=-GridRing1;dv<=GridRing1;++dv)for(int du=-GridRing1;du<=GridRing1;++du) {
            if(std::max(std::abs(du),std::abs(dv))<GridRing0)continue;
            const int x=CellU[c]+du,y=CellV[r]+dv;
            if(x<0 || y<0 || x>=int(m.w) || y>=int(m.h))continue;
            if(framePixel(m.texel(x,y)))++n;
        }
        if(n>best){best=n;at=r*3+c;}
    }
    if(frame)*frame=best;
    return best>=GridMinFrame?at:-1;
}
// The list's selected band in display rows (0..DisplayRows-1): first and last row, or false.
constexpr unsigned DisplayRows=128,DisplayCols=96;
inline bool listBand(const Movie& m,int& top,int& bottom) noexcept {
    top=bottom=-1;int runTop=-1;
    for(unsigned y=0;y<=DisplayRows;++y) {
        bool on=false;
        if(y<DisplayRows) {
            unsigned n=0;
            for(unsigned x=0;x<DisplayCols;++x)if(bandPixel(m.at((float(x)+0.5f)/DisplayCols,(float(y)+0.5f)/DisplayRows)))++n;
            on=float(n)>=BandMinShare*float(DisplayCols);
        }
        if(on && runTop<0)runTop=int(y);
        if(!on && runTop>=0) {
            if(int(y)-runTop>=int(BandMinRows)){top=runTop;bottom=int(y)-1;return true;}
            runTop=-1;
        }
    }
    return false;
}
// The plan for a tap at display (s, t) of the phone screen (outside 0..1: Ignore).
// v852 (phone_call.h): call = the phone rings or a call is on (the game's IS_PED_RINGTONE_PLAYING / IS_MOBILE_PHONE_CALL_ONGOING): the
// screen's right part (s >= BackShareX: the soft-key bar's right slot, the phone's red Back key) = B (decline / hang up), the rest = A
// (the select key: answer); no image and no D-pad steps (the call screen has no grid or list to walk).
inline Plan plan(const Movie& m,float s,float t,bool call=false) noexcept {
    Plan p;
    if(!(s>=0.0f && s<=1.0f && t>=0.0f && t<=1.0f)){p.kind=Kind::Ignore;return p;}
    if(call) {
        if(s>=BackShareX){p.kind=Kind::Decline;p.keys.push_back(Key::B);}
        else {p.kind=Kind::Answer;p.keys.push_back(Key::A);}
        return p;
    }
    if(t>FooterShare && s>=BackShareX){p.kind=Kind::Back;p.keys.push_back(Key::B);return p;} // (v830: no image needed)
    if(!m.rgba){p.kind=Kind::Ignore;return p;}
    if(!m.turned) {
        const int sel=gridSelection(m);
        if(sel>=0) {
            p.fromRow=sel/3;p.fromCol=sel%3;
            const float u=s*float(m.w),v=t*float(m.h);
            int tr=-1,tc=-1;float bestD=1e9f;
            for(int r=0;r<3;++r)for(int c=0;c<3;++c) {
                if(r==2 && c==2)continue; // (no app there)
                const float du=u-float(CellU[c]),dv=v-float(CellV[r]);
                if(std::fabs(du)>CellReach || std::fabs(dv)>CellReach)continue;
                const float d=du*du+dv*dv;if(d<bestD){bestD=d;tr=r;tc=c;}
            }
            if(tr<0){p.kind=Kind::Ignore;return p;} // (between the icons, the title, the bar: nothing)
            p.kind=Kind::Grid;p.toRow=tr;p.toCol=tc;
            for(int k=0;k<std::abs(tr-p.fromRow);++k)p.keys.push_back(tr>p.fromRow?Key::Down:Key::Up);
            for(int k=0;k<std::abs(tc-p.fromCol);++k)p.keys.push_back(tc>p.fromCol?Key::Right:Key::Left);
            p.keys.push_back(Key::A);
            return p;
        }
    }
    int top=0,bottom=0;
    if(listBand(m,top,bottom)) {
        const float rowH=float(bottom-top+1),y=t*float(DisplayRows);
        if(t<HeaderShare || t>FooterShare){p.kind=Kind::Ignore;return p;} // (the status / title area, the soft-key bar)
        const int d=int(std::floor((y-float(top))/rowH));
        if(std::abs(d)>int(MaxSteps)){p.kind=Kind::Ignore;return p;}
        p.kind=Kind::List;p.fromRow=0;p.toRow=d;
        for(int k=0;k<std::abs(d);++k)p.keys.push_back(d>0?Key::Down:Key::Up);
        p.keys.push_back(Key::A);
        return p;
    }
    p.kind=Kind::Plain;p.keys.push_back(Key::A);
    return p;
}
} // namespace recomp::gpu::phonetouch
