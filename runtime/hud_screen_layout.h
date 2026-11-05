#pragma once
// hud2 (2026-10-22): the second screen's HUD (AYN Thor's lower display) - the plain arithmetic, no Vulkan and no Android, so the
// host tests run this text (local-analysis/agents/hud3/tests/hud3_test.cpp; hud2's local-analysis/agents/hud2/tests).
//
// hud3 (v585): the second screen no longer copies anything from the main picture. It draws its own HUD (layout3 / drawList
// below): the radar from the game's own radar render target (captured at its resolve, hud_screen.h), the health / armour
// bars and the wanted stars as shapes at the second screen's own resolution from the player's state (hud_game_state.h).
// The picture rectangles below remain only for the owner's switch hide=1 (v576), which blacks the game's own HUD rectangles
// on the MAIN screen while the second screen shows the HUD.
//
// The game's HUD rectangles on the main picture: rectangles of the 16:9 game picture, in normalized picture coordinates (0..1 across the picture the presenter
// draws, i.e. the image rect inside the window, not the window). Normalized coordinates make them independent of the render
// size (1280x720, 960x540 ...), the output scaler and the window size: the game lays its HUD out on the picture, and the
// presenter maps the picture onto the image rect.
//
// Where the game draws (measured 2026-10-22 on a Thor 1920x1080 frame, play-agent/frames/abort-approach-20261006-232842.png,
// and consistent with the 1724x970 letterboxed frame v165end_60.png):
//   minimap with its health / armour bars: outer border x 97..364, y 835..1024 px  = x 0.0505..0.1901, y 0.7731..0.9481
//   one wanted star (the rightmost):       x 1797..1823, y 56..83 px              = x 0.9359..0.9495, y 0.0519..0.0769
// Both agree with the safe-zone placement the game uses (public description of GTA V's HUD anchor: inset = (1 - s) / 2 on
// every side for the safe-zone size s in 0.9 .. 1.0; minimap width = 1 / (4 * aspect) of the picture, height = 1 / 5.674;
// minimap anchored at the safe zone's bottom-left, the wanted stars right-aligned at its top-right): at s = 0.9 the inset is
// 0.05, the minimap spans x 0.05..0.1906, y 0.7738..0.95 - within 1 px of the measurement. Other safe-zone settings move the
// anchors: files/settings/second-screen.txt may name the inset ("inset=<percent>", 0 .. 10; default 5 = the measured one).
// The five-star row's width is not measured (one star only on the frames at hand): Stars covers ~6 star widths leftwards
// from the right anchor; a narrower row shows some of the picture beside it, never cuts a star.
#include "hud_game_state.h"
#include "hud_replay.h" // hud11 (v635): hudrep geometry (Hud11Cut, Hud11Slices, RadarRegion)
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace recomp::gpu::hud {
struct RectF {float x0,y0,x1,y1;};             // normalized picture coordinates
struct RectI {std::int32_t x0,y0,x1,y1;        // pixels, x1 / y1 exclusive
    constexpr std::int32_t w() const noexcept {return x1-x0;}
    constexpr std::int32_t h() const noexcept {return y1-y0;}
    constexpr bool empty() const noexcept {return x1<=x0 || y1<=y0;}};
constexpr float PictureAspect=16.0f/9.0f;
constexpr float DefaultInsetPercent=5.0f,MaxInsetPercent=10.0f;
constexpr float MinimapWidth=1.0f/(4.0f*PictureAspect),MinimapHeight=1.0f/5.674f;
constexpr float Pad=0.002f;                     // a few pixels round each element (its dark outline stays whole)
constexpr float StarsWidth=0.085f,StarsHeight=0.030f;

// files/settings/second-screen.txt: first word "hud" (letters in any case) = the second screen shows the HUD; anything else,
// an empty or absent file, or a file of 128 bytes or more = off. Further words (blank separated) are options:
//   inset=<number>  safe-zone inset in percent of the picture, 0 .. 10 (digits, optional '.', digits); invalid: ignored
//   map=0 | stars=0 leave that element out
//   replica=0       (hud4 v597) the second screen draws v596's picture (radar capture, drawn bars); the renderer replays nothing
//   hide=1          (v576, the owner's switch; off by default) black the copied elements' rectangles on the MAIN screen while
//                   the second screen shows them - a change of the main picture, so never on without this word
// Unknown words are ignored (and counted, for the log line).
struct Settings {bool on=false;float insetPercent=DefaultInsetPercent;bool minimap=true,stars=true;unsigned ignored=0;bool hide=false;
    bool replica=true; // hud4 (v597): replica=0 = v596's picture (the radar capture and drawn bars), no replay
    unsigned rate=2; // hud7 (v620): rate=1..4 = the lower screen is replayed every Nth game frame (default 2)
#if defined(RECOMP_THOR_EDITION)
    bool wheelMain=false; // thor-wheel (v601): wheel=main = "Weapon wheel: Main screen" (the game's own); default Lower screen
    bool lookGame=false;  // thor-wheel (v638): look=game = the box as the game composites it (the map at 0.5 over the world under the box)
    bool radioMain=false; // thor-radio (v651): radio=main = "Radio wheel: Main screen" (the game's own); default Lower screen
    bool characterMain=false; // thor-char: character=main = "Character wheel: Main screen" (the game's own); default Lower screen
    bool phoneMain=false; // v815: phone=main = the game's phone on the main screen (its own); default Lower screen
    bool textOn=false; // v836 (owner 2026-10-25: "the brief can stay in the top left corner, remove that off the second screen"): v818's lower-screen help box only with text=on (default none; the main screen's own box is untouched either way)
    bool saveOff=false; // v820: save=off = no Quick Save button on the lower screen (default: shown)
#endif
    };
constexpr std::size_t SettingsCap=128;
inline bool blank(char c) noexcept {return c==' ' || c=='\t' || c=='\n' || c=='\r';}
inline char lower(char c) noexcept {return c>='A' && c<='Z'?char(c-'A'+'a'):c;}
inline bool wordIs(const char* s,std::size_t n,const char* w) noexcept {
    std::size_t i=0;for(;w[i];++i)if(i>=n || lower(s[i])!=w[i])return false;
    return i==n;
}
// "5", "2.5", "10": digits, at most one '.', at most 8 characters; false otherwise.
inline bool number(const char* s,std::size_t n,float& out) noexcept {
    if(!n || n>8)return false;
    double v=0,scale=0;bool digit=false;
    for(std::size_t i=0;i<n;++i) {
        const char c=s[i];
        if(c=='.'){if(scale!=0)return false;scale=1;continue;}
        if(c<'0' || c>'9')return false;
        digit=true;
        if(scale!=0){scale/=10;v+=scale*(c-'0');}else v=v*10+(c-'0');
    }
    if(!digit)return false;
    out=float(v);return true;
}
inline Settings parseSettings(const char* s,std::size_t n) noexcept {
    Settings r;
    if(!s || n>=SettingsCap)return r;
    std::size_t i=0;bool first=true;
    while(i<n) {
        while(i<n && blank(s[i]))++i;
        if(i>=n)break;
        const std::size_t b=i;while(i<n && !blank(s[i]))++i;
        const char* w=s+b;const std::size_t wn=i-b;
        if(first){first=false;if(!wordIs(w,wn,"hud"))return Settings{};r.on=true;continue;}
        float v=0;
        if(wn>6 && wordIs(w,6,"inset=") && number(w+6,wn-6,v) && v<=MaxInsetPercent)r.insetPercent=v;
        else if(wordIs(w,wn,"map=0"))r.minimap=false;
        else if(wordIs(w,wn,"stars=0"))r.stars=false;
        else if(wordIs(w,wn,"hide=1"))r.hide=true;
        else if(wordIs(w,wn,"replica=0"))r.replica=false; // hud4
        else if(wn==6 && wordIs(w,5,"rate=") && w[5]>='1' && w[5]<='4')r.rate=unsigned(w[5]-'0'); // hud7
#if defined(RECOMP_THOR_EDITION)
        else if(wordIs(w,wn,"wheel=main"))r.wheelMain=true; // thor-wheel
        else if(wordIs(w,wn,"look=game"))r.lookGame=true; // thor-wheel (v638)
        else if(wordIs(w,wn,"radio=main"))r.radioMain=true; // thor-radio (v651)
        else if(wordIs(w,wn,"character=main"))r.characterMain=true; // thor-char
        else if(wordIs(w,wn,"phone=main"))r.phoneMain=true; // v815
        else if(wordIs(w,wn,"text=on"))r.textOn=true; else if(wordIs(w,wn,"text=off"))r.textOn=false; // v836: the mirror opt-in (text=off, v818's word, = the default)
        else if(wordIs(w,wn,"save=off"))r.saveOff=true; // v820
#endif
        else ++r.ignored;
    }
    return r;
}

inline RectF clamp01(RectF r) noexcept {
    return {std::clamp(r.x0,0.0f,1.0f),std::clamp(r.y0,0.0f,1.0f),std::clamp(r.x1,0.0f,1.0f),std::clamp(r.y1,0.0f,1.0f)};
}
inline RectF minimapRect(float insetPercent) noexcept {
    const float i=insetPercent/100.0f;
    return clamp01({i-Pad,1.0f-i-MinimapHeight-Pad,i+MinimapWidth+Pad,1.0f-i+Pad});
}
inline RectF starsRect(float insetPercent) noexcept {
    const float i=insetPercent/100.0f;
    return clamp01({1.0f-i-StarsWidth-Pad,i-Pad,1.0f-i+Pad,i+StarsHeight+Pad});
}

// A normalized rectangle on the presenter's image rect (left, top, width, height in window pixels): outward rounding, then
// clamped to the image rect. Empty when the image rect is.
inline RectI onPicture(RectF r,std::uint32_t left,std::uint32_t top,std::uint32_t width,std::uint32_t height) noexcept {
    if(!width || !height)return {0,0,0,0};
    const auto lo=[](float v,std::uint32_t n){return std::int32_t(std::floor(double(v)*n));};
    const auto hi=[](float v,std::uint32_t n){return std::int32_t(std::ceil(double(v)*n));};
    RectI p{std::int32_t(left)+lo(r.x0,width),std::int32_t(top)+lo(r.y0,height),std::int32_t(left)+hi(r.x1,width),std::int32_t(top)+hi(r.y1,height)};
    p.x0=std::max(p.x0,std::int32_t(left));p.y0=std::max(p.y0,std::int32_t(top));
    p.x1=std::min(p.x1,std::int32_t(left+width));p.y1=std::min(p.y1,std::int32_t(top+height));
    return p;
}

// ---------------------------------------------------------------- hud3 (v585): the second screen's own HUD
// The radar render target (census, every frame of 1,182 Thor census frames, local-analysis/thor-play-1021/census): EDRAM base
// 1440, pitch 560, k_8_8_8_8, 560x512. Each frame: a clear of 0..512 to 0 (transparent), a clear of the map rectangle
// (10,13)-(180,122) to the water colour, the vector map tiles scissored to it, then Scaleform shapes (blips, the player arrow)
// scissored to (0,0)-(190,135) - blips at the map's edge reach into the margin - and the resolve (caller 824126fc) of
// 0..512 to a 512x512 k_8_8_8_8 texture. The frame's HUD pass then composites that texture into the minimap box and draws the
// box's frame and the bars over it on the main picture. Guest pixels below; the capture keeps RadarCopyW x RadarCopyH of it.
constexpr float RadarMapX0=10,RadarMapY0=13,RadarMapX1=180,RadarMapY1=122; // the map (opaque in the target)
constexpr float RadarOverhang=6;                                          // margin shown round the map: blips at its edge
constexpr std::uint32_t RadarCopyW=200,RadarCopyH=144;                    // copied (past 190x135 the game's clear: zero)
constexpr std::uint32_t RadarCapacity=512;                                // the capture image (host texels, render scale <= 2.5)
constexpr float RadarAspect=(RadarMapX1-RadarMapX0)/(RadarMapY1-RadarMapY0);

struct Box {float x0=0,y0=0,x1=0,y1=0;
    constexpr float w() const noexcept {return x1-x0;}
    constexpr float h() const noexcept {return y1-y0;}
    constexpr bool empty() const noexcept {return !(x1>x0) || !(y1>y0);}};

// Proportions of GTA V's own minimap box (measured on the Thor 1920x1080 frame above, in units of the map's height): the
// frame's border 3 %, the gap above the bars 3 %, the bars 5.5 %; the health bar takes the left half (the special-ability
// bar is not shown: hud_game_state.h). Stars: 11 % of the map's height each, right-aligned above the frame.
constexpr float BorderK=0.03f,BarGapK=0.03f,BarK=0.055f,StarK=0.11f,StarGapK=0.35f,StarPitchK=1.10f;
struct Layout3 {
    bool ok=false;
    Box frame,map,radar,health,armour;   // radar: the map plus the overhang margin (the radar texture's quad)
    std::array<Box,5> stars{};           // left to right
    float radius=0,border=0;
};
inline float px(float v) noexcept {return std::round(v);}
// The HUD on a W x H second screen: the minimap as large as the screen allows (its frame inside a 3 % margin, the overhang
// inside a 2 % one), bars inside the frame directly under the map, the star row above; the group centred. map=0 keeps the
// frame (an empty map area), stars=0 drops the star row (the map grows).
inline Layout3 layout3(std::uint32_t width,std::uint32_t height,const Settings& s) noexcept {
    Layout3 L;
    if(width<64 || height<64 || width>16384 || height>16384)return L;
    const float W=float(width),H=float(height),m=0.03f*std::min(W,H),edge=0.02f*std::min(W,H);
    const float A=RadarAspect,over=RadarOverhang/(RadarMapY1-RadarMapY0); // (overhang in map heights)
    const float frameH=1.0f+BorderK+BarGapK+BarK+BorderK;                  // the frame's height per map height
    const float starsH=s.stars?StarK*(1.0f+StarGapK):0.0f;
    float mh=std::min({(W-2*m)/(A+2*BorderK),(W-2*edge)/(A+2*over),(H-2*m)/(frameH+starsH),(H-2*edge)/(1.0f+2*over+starsH)});
    if(!(mh>=16.0f))return L;
    mh=std::floor(mh);
    const float mw=std::floor(mh*A),b=std::max(2.0f,px(BorderK*mh)),gap=px(BarGapK*mh),bar=std::max(3.0f,px(BarK*mh));
    const float star=px(StarK*mh),groupH=(s.stars?star+px(StarGapK*star):0.0f)+b+mh+gap+bar+b;
    const float top=px((H-groupH)*0.5f),left=px((W-(mw+2*b))*0.5f);
    const float fy0=top+(s.stars?star+px(StarGapK*star):0.0f);
    L.frame={left,fy0,left+mw+2*b,fy0+b+mh+gap+bar+b};
    L.map={left+b,fy0+b,left+b+mw,fy0+b+mh};
    const float o=px(RadarOverhang*mw/(RadarMapX1-RadarMapX0));
    L.radar={L.map.x0-o,L.map.y0-o,L.map.x1+o,L.map.y1+o};
    const float by=L.map.y1+gap,split=px(std::max(1.0f,0.6f*b)),mid=px(L.map.x0+mw*0.5f);
    L.health={L.map.x0,by,mid-split*0.5f,by+bar};
    L.armour={mid+split*0.5f,by,L.map.x1,by+bar};
    L.health.x1=std::floor(L.health.x1);L.armour.x0=std::ceil(L.armour.x0);
    if(s.stars) {
        const float pitch=px(StarPitchK*star);
        for(int k=0;k<5;++k){const float x1=L.frame.x1-float(4-k)*pitch;L.stars[std::size_t(k)]={x1-star,top,x1,top+star};}
    }
    L.radius=std::max(2.0f,px(1.2f*b));L.border=b;
    L.ok=true;
    return L;
}

// What the second screen draws: up to MaxDraws quads, each one push-constant block of the HUD pipeline (hud_screen.vert /
// .frag; the CPU twin in local-analysis/agents/hud3/tests/hud3_reference.h). All coordinates are second-screen pixels, except
// a radar draw's shape / inner rectangles, which are texels of the radar capture image. No draw reads the main picture.
enum Kind : std::uint32_t {KindBox=0,KindRadar=1,KindBar=2,KindStar=3,KindReplica=4,KindLabel=5,KindShadow=6}; // (hud4 v597: KindReplica; v667: KindLabel; v681: KindShadow)
struct Draw {
    float rect[4]{};   // the quad (pixels); the vertex shader adds `apron` pixels round it for antialiasing
    float color[4]{};  // box / star fill, bar fill (straight alpha)
    float color2[4]{}; // bar track, star outline; radar: the map's rectangle in texels (sampled without its margin)
    float param[4]{};  // kind, radius | outline width, bar fraction | star inner ratio | radar dim, apron
    float shape[4]{};  // the shape's rectangle (pixels); radar: the quad's rectangle in texels
};
static_assert(sizeof(Draw)==80,"push-constant block (+ 16 bytes of screen = 96)");
constexpr unsigned MaxDraws=16;
struct DrawList {std::array<Draw,MaxDraws> d{};unsigned n=0;
    Draw& add() noexcept {static Draw spare;return n<MaxDraws?d[n++]:spare;}};
// The radar capture's state: never captured, or captured `age` real frames ago, at `scale` host texels per guest pixel.
struct RadarInfo {bool ever=false;unsigned age=0;float scale=1.0f;};
constexpr unsigned RadarStaleFrames=4; // the game draws the radar every frame it shows it: 4 frames without = hidden (menu, cutscene, load)
// Colours (display values, straight alpha): GTA V's HUD colours (hudcolour.dat's GREEN 114,204,114 / RED 224,50,50 /
// BLUE 93,182,229), the tracks at a third of their bar's colour, white stars with a dark outline, unlit stars dark grey.
struct Rgba {float r,g,b,a;};
constexpr Rgba Background{0,0,0,1},FrameColour{0.085f,0.085f,0.09f,1},NoMapColour{0.03f,0.035f,0.04f,1};
constexpr Rgba HealthColour{114/255.0f,204/255.0f,114/255.0f,1},HealthLowColour{224/255.0f,50/255.0f,50/255.0f,1},
    ArmourColour{93/255.0f,182/255.0f,229/255.0f,1},StarOn{0.94f,0.94f,0.94f,1},StarOff{0.15f,0.15f,0.16f,1},StarOutline{0,0,0,0.85f};
constexpr float TrackK=0.33f,DimK=0.38f,HealthLow=0.25f,StarInner=0.45f;
inline void put(float* d,Rgba c,float dim) noexcept {d[0]=c.r*dim;d[1]=c.g*dim;d[2]=c.b*dim;d[3]=c.a;}
inline void put(float* d,const Box& b) noexcept {d[0]=b.x0;d[1]=b.y0;d[2]=b.x1;d[3]=b.y1;}
inline bool radarLive(const RadarInfo& r) noexcept {return r.ever && r.age<RadarStaleFrames;}
// (hud6 v618: no native stars any more; the game's own star draws are replayed, hud_replay.h StarCollector)
// The frame's draw list. Stale radar (a menu, a cutscene, loading): everything at DimK, the last radar kept.
inline DrawList drawList(const Layout3& L,const Settings& s,const GameState& g,const RadarInfo& r) noexcept {
    DrawList out;
    if(!L.ok)return out;
    const float dim=radarLive(r)?1.0f:DimK;
    // the frame as four strips round the map (its inside is the map's: no overdraw)
    const Box strips[4]{{L.frame.x0,L.frame.y0,L.frame.x1,L.map.y0},{L.frame.x0,L.map.y1,L.frame.x1,L.frame.y1},
                        {L.frame.x0,L.map.y0,L.map.x0,L.map.y1},{L.map.x1,L.map.y0,L.frame.x1,L.map.y1}};
    for(const auto& st:strips){auto& d=out.add();put(d.rect,st);put(d.shape,L.frame);put(d.color,FrameColour,dim);
        d.param[0]=KindBox;d.param[1]=L.radius;d.param[3]=1;}
    if(s.minimap && r.ever) {
        auto& d=out.add();put(d.rect,L.radar);
        // the map in whole texels (at a fractional render scale its edge row is the one the game's scissor rounds to); the
        // map's rectangle on the screen shows exactly those texels, the overhang the margin round them
        const float t=r.scale,ix0=std::round(RadarMapX0*t),iy0=std::round(RadarMapY0*t),ix1=std::round(RadarMapX1*t),iy1=std::round(RadarMapY1*t);
        const float kx=(ix1-ix0)/L.map.w(),ky=(iy1-iy0)/L.map.h(); // texels per screen pixel
        d.shape[0]=ix0-(L.map.x0-L.radar.x0)*kx;d.shape[1]=iy0-(L.map.y0-L.radar.y0)*ky;
        d.shape[2]=ix1+(L.radar.x1-L.map.x1)*kx;d.shape[3]=iy1+(L.radar.y1-L.map.y1)*ky;
        d.color2[0]=ix0;d.color2[1]=iy0;d.color2[2]=ix1;d.color2[3]=iy1;
        d.param[0]=KindRadar;d.param[2]=dim;d.param[3]=0;
    } else {
        auto& d=out.add();put(d.rect,L.map);put(d.shape,L.map);put(d.color,NoMapColour,dim);d.param[0]=KindBox;d.param[3]=0;
    }
    const float hf=healthFraction(g),af=armourFraction(g);
    {
        auto& d=out.add();put(d.rect,L.health);put(d.shape,L.health);
        const Rgba c=g.ped && hf<HealthLow?HealthLowColour:HealthColour;
        put(d.color,c,dim);put(d.color2,{c.r*TrackK,c.g*TrackK,c.b*TrackK,1},dim);
        d.param[0]=KindBar;d.param[1]=0;d.param[2]=hf;d.param[3]=1;
    }
    {
        auto& d=out.add();put(d.rect,L.armour);put(d.shape,L.armour);
        put(d.color,ArmourColour,dim);put(d.color2,{ArmourColour.r*TrackK,ArmourColour.g*TrackK,ArmourColour.b*TrackK,1},dim);
        d.param[0]=KindBar;d.param[1]=0;d.param[2]=af;d.param[3]=1;
    }
    // (hud6 v618, the owner: no generic stars; "no stars at all is better than generic ones": the fallback draws none)
    return out;
}
// What a frame's picture depends on: an unchanged key is not drawn again (the second screen keeps showing the last one).
inline std::uint64_t contentKey(const Settings& s,const GameState& g,const RadarInfo& r,std::uint64_t radarSerial) noexcept {
    std::uint64_t k=radarLive(r)?radarSerial:(r.ever?0x5354414C45ull:0x4E4F4E45ull); // live: every capture is new; else stale / none
    const auto q=[](float f){return std::uint64_t(std::lround(f*1000.0f))&0xFFFull;};
    k=k*0x100000001B3ull^(q(healthFraction(g))|q(armourFraction(g))<<12|std::uint64_t(g.wanted+1)<<24|std::uint64_t(g.ped)<<28|
        std::uint64_t(s.minimap)<<29|std::uint64_t(s.stars)<<30|std::uint64_t(g.ped && healthFraction(g)<HealthLow)<<31);
    return k;
}

// ---------------------------------------------------------------- hud4 (v597): the replica
// The renderer replays the game's own minimap operations (the radar pass, the composite, the frame, bars, arrow and blips) into a
// shadow overlay at k/16 the console's size (hud_replay.h, hud_replay_renderer.inc) and publishes it: an image and the box (the
// composite's scissor) in its host texels. The second screen shows that box as it is, texel for texel when it fits (it is made
// to fit: hudrep::scaleFor), else scaled down uniformly; the proportions are the game's (one scale for both axes). The wanted
// stars are not in the box (the game draws them elsewhere; their draws are not identified yet): drawn as shapes above the box,
// right-aligned, and only at a wanted level of 1 or more (at 0 the game shows none).
struct ReplicaInfo {bool ever=false;unsigned age=0;float box[4]{};float invW=0,invH=0;
    bool stars=false;float starBox[4]{}; // hud5 (v604): this replica's star band holds the game's stars; their crop (host texels)
    float map[4]{},bars[4]{}; // hud6 (v618): the map inside the box, the bar strip (host texels)
    float interior[4]{},interiorAt[2]{}; // hud7 (v620): map = the screen-sized band; the interior's texels and its screen place
    float frame[4]{}; // hud11 (v635): the frame band's texels (all 0: none)
    float backdrop[4]{}; // v638 (look=game)
    float flash[4]{};}; // hud15 (v695): the flash band's map rectangle (texels; all 0: no flash this replay) // v638 (look=game): the main frame's pixels under the box this frame (texels; all 0: none)
constexpr float ReplicaStarK=0.13f,ReplicaStarGapK=0.04f,ReplicaStarPitchK=1.12f,ReplicaFill=0.98f;
struct ReplicaLayout {bool ok=false;Box box;std::array<Box,5> stars{};float scale=1;};
// W x H second screen, a box of bw x bh host texels.
inline ReplicaLayout replicaLayout(std::uint32_t width,std::uint32_t height,float bw,float bh) noexcept {
    ReplicaLayout L;
    if(width<64 || height<64 || !(bw>=8.0f) || !(bh>=8.0f))return L;
    const float W=float(width),H=float(height);
    const float rowK=ReplicaStarK+ReplicaStarGapK; // (the star row, in box heights)
    L.scale=std::min({1.0f,ReplicaFill*W/bw,ReplicaFill*H/(bh*(1.0f+rowK))});
    const float w=std::round(bw*L.scale),h=std::round(bh*L.scale);
    const float star=px(ReplicaStarK*h),gap=px(ReplicaStarGapK*h);
    const float top=std::floor((H-(h+star+gap))*0.5f),left=std::floor((W-w)*0.5f);
    L.box={left,top+star+gap,left+w,top+star+gap+h};
    const float pitch=px(ReplicaStarPitchK*star);
    for(int k=0;k<5;++k){const float x1=L.box.x1-float(4-k)*pitch;L.stars[std::size_t(k)]={x1-star,top,x1,top+star};}
    L.ok=true;
    return L;
}
inline bool replicaLive(const ReplicaInfo& r) noexcept {return r.ever && r.age<RadarStaleFrames;}
// hud5 (v604): the box as large as the screen allows with the star crop (sw x sh texels) above it, right-aligned to the box, a gap of
// StarGap5 box heights between; both at one scale (texel for texel when they fit: hud_replay.h scaleFor5 makes them fit), the group
// centred. The star row's place is kept when no star is drawn (the box does not move with the wanted level).
constexpr float StarGap5=0.022f,Margin5=4.0f;
struct ReplicaLayout5 {bool ok=false;Box box,stars;float scale=1;};
inline ReplicaLayout5 replicaLayout5(std::uint32_t width,std::uint32_t height,float bw,float bh,float sw,float sh) noexcept {
    ReplicaLayout5 L;
    if(width<64 || height<64 || !(bw>=8.0f) || !(bh>=8.0f) || !(sw>=0.0f) || !(sh>=0.0f))return L;
    const float W=float(width),H=float(height),gap=std::round(StarGap5*bh);
    L.scale=std::min({1.0f,(W-2*Margin5)/std::max(bw,sw),(H-2*Margin5)/(bh+sh+gap)});
    if(!(L.scale>0.0f))return L;
    const float w=std::round(bw*L.scale),h=std::round(bh*L.scale),s2w=std::round(sw*L.scale),s2h=std::round(sh*L.scale),g=std::round(gap*L.scale);
    const float top=std::floor((H-(s2h+g+h))*0.5f),left=std::floor((W-w)*0.5f);
    L.stars={left+w-s2w,top,left+w,top+s2h};
    L.box={left,top+s2h+g,left+w,top+s2h+g+h};
    L.ok=true;
    return L;
}
// ---- hud6 (v618, the owner: "fill the map to the whole screen, then have the stars overlay smaller in the top right corner"):
//  - the map: the screen's size of texels from the middle of the map interior (texel for texel: the replay's scale makes the interior at
//    least that large, hud_replay.h scaleFor6); a smaller interior (another screen) is shown as its largest centred crop of the
//    screen's aspect, enlarged;
//  - the bar strip across the bottom edge at the screen's width (its proportions kept), over the map;
//  - the game's stars (when this replica has them) at StarH6 of the screen's height, StarMargin6 from the top and right edges, over the
//    map, with their own alpha (the star band is drawn over transparent black: premultiplied).
// Kind 4 flags in param.y: 1 = premultiplied alpha (the stars), 2 = area-filtered when scaled down (the bars, the stars), 4 (hud8 v625) =
// coverage below FrameShadeA is dropped (the box layer: the game's faint inner shading round the map, alpha ~0.2 on the device frame).
constexpr float StarH6=0.072f,StarMargin6=0.018f;
struct ReplicaLayout6 {bool ok=false;Box map,mapTex,bars,stars;bool mapExact=false;};
inline ReplicaLayout6 replicaLayout6(std::uint32_t width,std::uint32_t height,const ReplicaInfo& r) noexcept {
    ReplicaLayout6 L;
    const float W=float(width),H=float(height),mw=r.map[2]-r.map[0],mh=r.map[3]-r.map[1];
    if(width<64 || height<64 || !(mw>=8.0f) || !(mh>=8.0f))return L;
    L.map={0,0,W,H};
    const float cx=std::floor((r.map[0]+r.map[2])*0.5f),cy=std::floor((r.map[1]+r.map[3])*0.5f);
    if(mw>=W && mh>=H) { // texel for texel: the middle W x H of the interior (inside it)
        const float x0=std::clamp(cx-std::floor(W*0.5f),r.map[0],r.map[2]-W),y0=std::clamp(cy-std::floor(H*0.5f),r.map[1],r.map[3]-H);
        L.mapTex={x0,y0,x0+W,y0+H};L.mapExact=true;
    } else { // the largest centred crop of the screen's aspect, enlarged
        const float s=std::min(mw/W,mh/H),cw=W*s,ch=H*s;
        L.mapTex={(r.map[0]+r.map[2]-cw)*0.5f,(r.map[1]+r.map[3]-ch)*0.5f,(r.map[0]+r.map[2]+cw)*0.5f,(r.map[1]+r.map[3]+ch)*0.5f};
    }
    const float bw=r.bars[2]-r.bars[0],bh=r.bars[3]-r.bars[1];
    if(bw>=1.0f && bh>=1.0f){const float h=std::round(W*bh/bw);L.bars={0,H-h,W,H};}
    const float sw=r.starBox[2]-r.starBox[0],sh=r.starBox[3]-r.starBox[1];
    if(sw>=1.0f && sh>=1.0f) {
        const float h=std::round(StarH6*H),w=std::round(h*sw/sh),m=std::round(StarMargin6*H);
        L.stars={W-m-w,m,W-m,m+h};
    }
    L.ok=true;
    return L;
}
inline DrawList replicaDrawList6(const ReplicaLayout6& L,const Settings& s,const ReplicaInfo& r) noexcept {
    DrawList out;
    if(!L.ok || !r.ever)return out;
    const float dim=replicaLive(r)?1.0f:DimK;
    const auto quad=[&](const Box& rect,const Box& tex,float flags) {
        auto& d=out.add();put(d.rect,rect);put(d.shape,tex);
        d.color2[0]=r.invW;d.color2[1]=r.invH;d.param[0]=KindReplica;d.param[1]=flags;d.param[2]=dim;d.param[3]=0;
    };
    if(s.minimap) {
        quad(L.map,L.mapTex,0.0f);
        if(!L.bars.empty())quad(L.bars,Box{r.bars[0],r.bars[1],r.bars[2],r.bars[3]},2.0f);
    }
    if(s.stars && r.stars && !L.stars.empty())quad(L.stars,Box{r.starBox[0],r.starBox[1],r.starBox[2],r.starBox[3]},3.0f);
    return out;
}
// ---- hud11 (v635): the lower screen is the game's minimap box (hud_replay.h hud11). Quads, in drawing order:
//  - the map: the replay's screen-sized crop (radar rows from Hud11TopRow), texel for texel over the whole screen, opaque;
//  - the frame: 9 slices of the frame band (premultiplied, the frame's own alpha): corners at the screen's corners and the top / bottom
//    edges texel for texel (their middle fitted to the screen's width: a uniform edge), the side edges stretched over the interior's height;
//  - the box's draws: rows < Hud11Cut in place on the map (texel for texel), rows >= Hud11Cut at the screen's bottom (texel for texel);
//  - the game's stars at the top right, inside the frame.
inline constexpr float LookGameMapAlpha=0.5f; // v638: the game's composite scales the radar by c230 = 0.5 (census: the minimap composite f0ff7fe2)
struct ReplicaLayout11 {bool ok=false;Box map,mapTex,top,topTex,bottom,bottomTex,stars;std::array<Box,8> frame{},frameTex{};unsigned frames=0;
    std::array<Box,4> border{};unsigned borders=0;
    Box flash,flashTex;}; // (hud15 v695: the damage flash's screen rectangle = the map area inside the frame, and its texels) // (v636: the solid border: the screen outside the map rectangle's place)
inline ReplicaLayout11 replicaLayout11(std::uint32_t width,std::uint32_t height,const ReplicaInfo& r) noexcept {
    ReplicaLayout11 L;
    const float W=float(width),H=float(height);
    if(width<64 || height<64 || !(r.map[2]-r.map[0]>=8.0f) || !(r.map[3]-r.map[1]>=8.0f))return L;
    const float mh=std::min(H,r.map[3]-r.map[1]);
    L.map={0,0,std::min(W,r.map[2]-r.map[0]),mh};L.mapTex={r.map[0],r.map[1],r.map[0]+L.map.x1,r.map[1]+mh};
    // the box's draws (the interior texels = the radar region at the replay's scale; k from its height: 135 rows)
    const float iw=r.interior[2]-r.interior[0],ih=r.interior[3]-r.interior[1];
    if(iw>0.0f && ih>0.0f) {
        const float cut=std::round(ih*float(hudrep::Hud11Cut)/float(hudrep::RadarRegion.y1));
        const float x0=std::max(r.interiorAt[0],0.0f),x1=std::min(r.interiorAt[0]+iw,W),tx0=r.interior[0]+(x0-r.interiorAt[0]),tx1=tx0+(x1-x0);
        const float ty=r.interiorAt[1]; // (box row 0's place on the screen)
        const Box top{x0,std::max(ty,0.0f),x1,std::min(ty+cut,H)};
        if(!top.empty()){L.top=top;L.topTex={tx0,r.interior[1]+(top.y0-ty),tx1,r.interior[1]+(top.y1-ty)};}
        const float bh=ih-cut;
        if(bh>0.0f){L.bottom={x0,H-bh,x1,H};L.bottomTex={tx0,r.interior[1]+cut,tx1,r.interior[3]};}
    }
    // hud12 (v637; the owner's target thor-edition/owner-target-lower-2026-10-23.jpg: v636's interior 1:1 in a thin black frame, the bars
    // band across the width): the map stays over the whole screen; a 14-px (at 1080) black frame on the left, top and right; the bars band
    // (box rows >= Hud12BandRow: the game's black band behind the bars) black across the screen down to the bottom; the box's bottom piece
    // fitted horizontally so the bars (columns 10..180) run from the frame's inner left edge to its inner right edge (vertically texel for
    // texel). No game frame slices (hudrep::Hud11FrameBand off: v635's ring showed inset, v636's 63/85-px border was rejected).
    if(iw>0.0f && ih>0.0f) {
        const float syb=ih/float(hudrep::RadarRegion.y1),fr=std::round(hudrep::Hud12FramePx1080*H/1080.0f);
        const float yband=H-std::round((float(hudrep::RadarRegion.y1)-hudrep::Hud12BandRow)*syb);
        L.border[0]={0,0,fr,H};L.border[1]={W-fr,0,W,H};L.border[2]={fr,0,W-fr,fr};L.border[3]={fr,yband,W-fr,H};L.borders=4;
        if(!L.bottom.empty()) { // the bottom piece: columns 10..180 onto [fr, W - fr], clipped to the screen with its texels
            const float sxb=iw/float(hudrep::RadarRegion.x1);
            const float s=(W-2.0f*fr)/float(hudrep::RadarMap.x1-hudrep::RadarMap.x0); // (screen px per guest column)
            const float x0=fr-float(hudrep::RadarMap.x0)*s,x1=x0+float(hudrep::RadarRegion.x1)*s;
            const float cx0=std::max(x0,0.0f),cx1=std::min(x1,W);
            const float t0=r.interior[0],per=sxb/s; // (texels per screen px)
            L.bottom={cx0,L.bottom.y0,cx1,L.bottom.y1};L.bottomTex={t0+(cx0-x0)*per,L.bottomTex.y0,t0+(cx1-x0)*per,L.bottomTex.y1};
        }
        // hud15 (v695; owner 2026-10-24 "fit it to the borders of the screen"): the game's damage flash covers the game's map rectangle;
        // here it covers the lower screen's map area: inside the frame (left, top, right) down to the bars band, the flash band's map
        // rectangle stretched onto it (the game's own drawing, its proportions per axis to the area)
        const float fw=r.flash[2]-r.flash[0],fh=r.flash[3]-r.flash[1];
        if(fw>=1.0f && fh>=1.0f && yband>fr){L.flash={fr,fr,W-fr,yband};L.flashTex={r.flash[0],r.flash[1],r.flash[2],r.flash[3]};}
    }
    const float sw=r.starBox[2]-r.starBox[0],sh=r.starBox[3]-r.starBox[1];
    if(sw>=1.0f && sh>=1.0f) { // (v636: on the map, inside the frame's top-right corner)
        const float m=std::round(0.015f*H),h=std::round(StarH6*H),w=std::round(h*sw/sh);
        const float x1=(L.borders?L.border[1].x0:W)-m,y0=(L.borders?L.border[2].y1:0.0f)+m;
        L.stars={x1-w,y0,x1,y0+h};
    }
    L.ok=true;
    return L;
}
inline DrawList replicaDrawList11(const ReplicaLayout11& L,const Settings& s,const ReplicaInfo& r) noexcept {
    DrawList out;
    if(!L.ok || !r.ever)return out;
    const float dim=replicaLive(r)?1.0f:DimK;
    const auto quad=[&](const Box& rect,const Box& tex,float flags) {
        auto& d=out.add();put(d.rect,rect);put(d.shape,tex);
        d.color2[0]=r.invW;d.color2[1]=r.invH;d.param[0]=KindReplica;d.param[1]=flags;d.param[2]=dim;d.param[3]=0;
    };
    if(s.minimap) {
#if defined(RECOMP_THOR_EDITION)
        // v638 (second-screen.txt look=game): the box as the game composites it - the world under the box (the main frame's pixels there,
        // captured just before the game's composite, upscaled over the screen), the map over it at the game's 0.5 (flag 8: alpha = param.w)
        const bool game=s.lookGame && r.backdrop[2]-r.backdrop[0]>=8.0f && r.backdrop[3]-r.backdrop[1]>=8.0f;
        if(game) {
            quad(L.map,Box{r.backdrop[0],r.backdrop[1],r.backdrop[2],r.backdrop[3]},0.0f); // (the whole screen, bilinear)
            quad(L.map,L.mapTex,8.0f);out.d[out.n-1].param[3]=LookGameMapAlpha;
        } else
#endif
        quad(L.map,L.mapTex,0.0f);
        for(unsigned i=0;i<L.borders;++i)if(!L.border[i].empty()){auto& d=out.add();put(d.rect,L.border[i]);put(d.shape,L.border[i]);
            put(d.color,Background,1.0f);d.param[0]=KindBox;d.param[1]=0;d.param[3]=0;} // (v636: the solid border, flush with the edges)
        for(unsigned i=0;i<L.frames;++i)quad(L.frame[i],L.frameTex[i],1.0f); // (premultiplied: the frame's own alpha over the border / map)
        if(!L.flash.empty())quad(L.flash,L.flashTex,5.0f); // hud15: in the game's order (right after its mask: under the box's other draws), as the box layer is shown
        if(!L.top.empty())quad(L.top,L.topTex,5.0f);
        if(!L.bottom.empty())quad(L.bottom,L.bottomTex,5.0f);
    }
    if(s.stars && r.stars && !L.stars.empty())quad(L.stars,Box{r.starBox[0],r.starBox[1],r.starBox[2],r.starBox[3]},3.0f);
    return out;
}
// ---- hud7 (v620): the map's whole width (edge blips kept) at full strength; the box's own draws of the interior over it
//  - the map band: the replay's screen-sized crop of the radar texture (the game's map, the rows above and below the box's interior drawn
//    by the same radar pass made taller), texel for texel over the whole screen, opaque;
//  - the interior of the box (blips, the arrow, north, route marks: the game's draws over its map; transparent elsewhere), texel for
//    texel at its place on the map (interiorAt), premultiplied over it, clipped to the screen;
//  - the bar strip and the stars as hud6.
struct ReplicaLayout7 {bool ok=false;Box map,mapTex,interior,interiorTex,bars,stars;};
inline ReplicaLayout7 replicaLayout7(std::uint32_t width,std::uint32_t height,const ReplicaInfo& r) noexcept {
    ReplicaLayout7 L;
    const float W=float(width),H=float(height);
    if(width<64 || height<64 || !(r.map[2]-r.map[0]>=8.0f) || !(r.map[3]-r.map[1]>=8.0f))return L;
    // hud10 (v634): a map band shorter than the screen sits at the screen's BOTTOM (the box's bars at the bottom edge), black above it
    const float mh=std::min(H,r.map[3]-r.map[1]),dy=H-mh;
    L.map={0,dy,std::min(W,r.map[2]-r.map[0]),H};L.mapTex={r.map[0],r.map[1],r.map[0]+L.map.x1,r.map[1]+mh};
    // the interior: its screen rectangle clipped to the screen, the same texels clipped with it (one texel per pixel)
    const float iw=r.interior[2]-r.interior[0],ih=r.interior[3]-r.interior[1];
    Box at{r.interiorAt[0],r.interiorAt[1]+dy,r.interiorAt[0]+iw,r.interiorAt[1]+dy+ih};
    const Box clip{std::max(at.x0,0.0f),std::max(at.y0,0.0f),std::min(at.x1,W),std::min(at.y1,H)};
    if(!clip.empty()){L.interior=clip;L.interiorTex={r.interior[0]+(clip.x0-at.x0),r.interior[1]+(clip.y0-at.y0),r.interior[0]+(clip.x1-at.x0),r.interior[1]+(clip.y1-at.y0)};}
    const float bw=r.bars[2]-r.bars[0],bh=r.bars[3]-r.bars[1];
    if(bw>=1.0f && bh>=1.0f){const float h=std::round(W*bh/bw);L.bars={0,H-h,W,H};}
    const float sw=r.starBox[2]-r.starBox[0],sh=r.starBox[3]-r.starBox[1];
    if(sw>=1.0f && sh>=1.0f){const float h=std::round(StarH6*H),w=std::round(h*sw/sh),m=std::round(StarMargin6*H);L.stars={W-m-w,m,W-m,m+h};}
    L.ok=true;
    return L;
}
inline DrawList replicaDrawList7(const ReplicaLayout7& L,const Settings& s,const ReplicaInfo& r) noexcept {
    DrawList out;
    if(!L.ok || !r.ever)return out;
    const float dim=replicaLive(r)?1.0f:DimK;
    const auto quad=[&](const Box& rect,const Box& tex,float flags) {
        auto& d=out.add();put(d.rect,rect);put(d.shape,tex);
        d.color2[0]=r.invW;d.color2[1]=r.invH;d.param[0]=KindReplica;d.param[1]=flags;d.param[2]=dim;d.param[3]=0;
    };
    if(s.minimap) {
        quad(L.map,L.mapTex,0.0f);
        if(!L.interior.empty())quad(L.interior,L.interiorTex,5.0f); // (hud8: premultiplied, the faint frame shading left out: flag 4)
        if(!L.bars.empty())quad(L.bars,Box{r.bars[0],r.bars[1],r.bars[2],r.bars[3]},2.0f);
    }
    if(s.stars && r.stars && !L.stars.empty())quad(L.stars,Box{r.starBox[0],r.starBox[1],r.starBox[2],r.starBox[3]},3.0f);
    return out;
}
// The hud5 draw list: the box, then the game's stars when this replica has them (no native stars: the game draws none at wanted 0).
inline DrawList replicaDrawList5(const ReplicaLayout5& L,const Settings& s,const ReplicaInfo& r) noexcept {
    DrawList out;
    if(!L.ok || !r.ever)return out;
    const float dim=replicaLive(r)?1.0f:DimK;
    if(s.minimap) {
        auto& d=out.add();put(d.rect,L.box);
        d.shape[0]=r.box[0];d.shape[1]=r.box[1];d.shape[2]=r.box[2];d.shape[3]=r.box[3];
        d.color2[0]=r.invW;d.color2[1]=r.invH;d.param[0]=KindReplica;d.param[2]=dim;d.param[3]=0;
    }
    if(s.stars && r.stars && !L.stars.empty()) {
        auto& d=out.add();put(d.rect,L.stars);
        d.shape[0]=r.starBox[0];d.shape[1]=r.starBox[1];d.shape[2]=r.starBox[2];d.shape[3]=r.starBox[3];
        d.color2[0]=r.invW;d.color2[1]=r.invH;d.param[0]=KindReplica;d.param[2]=dim;d.param[3]=0;
    }
    return out;
}
// The replica's draw list: the box (KindReplica: rect = the screen box, shape = the box in the overlay's host texels, color2.xy =
// 1 / the overlay's size, param.z = dim), then the stars at wanted >= 1.
inline DrawList replicaDrawList(const ReplicaLayout& L,const Settings& s,const GameState& g,const ReplicaInfo& r) noexcept {
    DrawList out;
    if(!L.ok || !r.ever)return out;
    const float dim=replicaLive(r)?1.0f:DimK;
    if(s.minimap) {
        auto& d=out.add();put(d.rect,L.box);
        d.shape[0]=r.box[0];d.shape[1]=r.box[1];d.shape[2]=r.box[2];d.shape[3]=r.box[3];
        d.color2[0]=r.invW;d.color2[1]=r.invH;d.param[0]=KindReplica;d.param[2]=dim;d.param[3]=0;
    }
    (void)g; // (hud6 v618: no native stars)
    return out;
}
inline std::uint64_t replicaKey(const Settings& s,const GameState& g,const ReplicaInfo& r,std::uint64_t serial) noexcept {
    std::uint64_t k=replicaLive(r)?serial:(r.ever?0x5354414C45ull:0x4E4F4E45ull);
    k=k*0x100000001B3ull^(std::uint64_t(g.wanted+1)|std::uint64_t(s.minimap)<<8|std::uint64_t(s.stars)<<9|std::uint64_t(r.stars)<<10|0x4855443400000000ull);
    return k;
}
// v632 (main's decision, 2026-10-23): the game shows no HUD -> the lower screen shows no HUD either: black, nothing drawn (until v631:
// the last map dimmed). "No HUD" = neither the radar capture nor the minimap replica is current (no radar drawn by the game for
// RadarStaleFrames frames: a cutscene, and equally a pause menu or a load screen, the same signal). The weapon wheel, when current,
// still wins (wheel_screen.inc runs after this). hudHidden() is what Screen::prepare / record use; HiddenKey = one black picture,
// drawn once and then skipped as unchanged.
constexpr std::uint64_t HiddenKey=0x48494444454E4855ull;
inline bool hudHidden(const Settings& s,const RadarInfo& radar,const ReplicaInfo& rep) noexcept {
    return !radarLive(radar) && !(s.replica && replicaLive(rep));
}
}
