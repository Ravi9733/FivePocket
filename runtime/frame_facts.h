#pragma once
// fixer2 (2026-10-22, DIAGNOSTIC): frame facts. The live glitch detector (local-analysis/play-agent/glitch_watch.py) finds
// one-frame faults seconds after they were shown (G7: the sun stripes in the therapist's office missing for ONE frame); a
// census taken then shows a later, normal frame. So the renderer keeps a few facts of every presented frame in a ring
// (memory only) and writes the last Dump of them to the log when the detector asks.
//   files/private-trace/diag-frame-facts       while present (polled every 60 presents): record one Fact per present
//   files/private-trace/diag-frame-facts-dump  the detector touches it on a hit: when its modification time differs from the
//                                              last one seen (polled every 15 presents while recording), the ring is logged
//                                              as RENDER_FRAME_FACTS lines, oldest first (nothing is deleted)
// Off (no diag-frame-facts): one load and a branch per present and per shadow-sampling draw; nothing recorded.
// Pure part (this file): the Fact, the ring and the line text (tests/frame_facts_test.cpp).
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <cstring>
#include <algorithm>
#include <memory>
#include <vector>

namespace recomp::gpu::generic::ff {

inline constexpr unsigned Ring=512;  // ~17 s at 30 fps: the detector's delay (feed ~1 s, 4 fps frames, the 15-present poll) fits
inline constexpr unsigned Dump=360;  // the frames logged on a request (~12 s)
inline constexpr unsigned GridW=16,GridH=9,GridN=GridW*GridH; // v582: the luma grid

// One presented frame. Counts are this frame's (deltas of the renderer's running counters); the light facts are those of the
// frame's FIRST draw that sampled a shadow map (the sun / cascade lighting draw): its pixel shader, the age in presents of the
// shadow map it bound (0: resolved in this frame), gDirectionalLight (c21) and gDirectionalColour (c22) as that draw read them.
struct Fact {
    std::uint64_t present=0,frame=0;
    std::int64_t wallMs=0;          // device wall clock (log alignment with logcat time stamps)
    std::uint32_t draws=0,resolves=0,shadowResolves=0,casterDraws=0,shadowDraws=0,staleShadowDraws=0,passes=0;
    std::uint32_t lightPs=0;        // low 32 bits of that draw's pixel shader key (0: no shadow-sampling draw this frame)
    std::uint32_t shadowAge=0;      // presents since the bound shadow map's resolve (~0u: unknown)
    float sunDir[3]{},sunColour[4]{};
    std::uint16_t tonemap=0;        // the frame's tonemap variant (postfx number; 0: none seen) - v577's family
    bool camera=false;              // the frame generator took a camera from it
    // v579 (G7, the sun dips): the sun pass. sunDraws: draws of the directional-light pixel shaders (directional#6p..#11p);
    // maskDraws: draws of cascadeshadows_rendering#18p (the screen-space sun shadow mask); for the frame's FIRST sun draw the
    // image it binds at s9 (the shadow mask: census la-sun-1, resolve 1152:0 -> 06aa0000): VkFormat, presents since its
    // resolve, a tag of its VkImage handle, and the mask's RGBA at four screen points as the GPU held it when the sun draw
    // was recorded (read back after the slot's fence: probe 0 not yet read, 1 read, 2 not taken). 0 = fully shadowed.
    std::uint16_t sunDraws=0,maskDraws=0;
    std::int32_t maskFormat=0;std::uint32_t maskAge=~0u,maskImage=0;
    std::uint8_t probe=2;float maskRgba[4][4]{};
    std::uint32_t seq=0;            // this fact's sequence number (the probe's read-back finds its frame by it)
    std::uint32_t drawRefused=0,resolveRefused=0; // v579: draws / resolves the renderer refused in this frame
    char firstRefuse[16]{};         // the first refusal's reason (draw or resolve), cut to 15 characters
    // v581 (G7: every 13-16 presents the shadow mask's rows 384-719 were 0,0,0,0 with two more passes): the frame's v310 residue
    // events in order, space separated (note()): T<base>/<rows> a colour surface becomes tiled-drawn, M<base>/<rows> one bound
    // past its tiled rows (residue marked), D<base> a mark dropped by a tiled rebind, A<base>:<bands> applied,
    // S<base>@<band base><<culprit base>:<fmt>:<pitch>/<rows>c<clock gap> a band stale, X<base>@<band base>:<why><near base>
    // a band missing, U<base> unsupported, P<base>:<fmt>:<pitch>/<rows> a pass begun, not tiled, over EDRAM tiles 1536-1871.
    // A full string ends in '+'. v583: K<base><<surface base>:<fmt>:<pitch>/<rows> a pass attached over the band that wrote none of
    // its tiles (copied anyway; v310 called it stale).
    char res[200]{};
    // v582 (fixer3, G10 / G1: one-frame black screen shapes in a night gunfight; DIAGNOSTIC): a GridW x GridH grid of the presented
    // real picture's luma (one texel at each cell's centre, read back after the slot's fence like the sun probe): 0..255, gridOk =
    // read. Finds the frame among the dump's 360 whose cells turned black while both neighbours were lit (oddCells()).
    std::uint8_t gridOk=0;std::uint8_t grid[GridN]{};
    std::uint32_t skoUs=0;  // v584: render-thread microseconds the per-frame skinned-draw scan took in this frame
    std::uint16_t skoBad=0; // v584: skinned draws of this frame with a sampled vertex > 10 m from the draw's body (RENDER_FRAME_SKO lines)
};

// v581: appends one residue event (a space first when not empty); when it does not fit the string ends in '+' instead.
inline void note(Fact& f,const char* e) noexcept {
    const std::size_t n=std::char_traits<char>::length(f.res),m=std::char_traits<char>::length(e),cap=sizeof f.res-1;
    if(n && f.res[n-1]=='+')return;
    if(n+(n?1:0)+m+1>cap){if(n<cap){f.res[n]='+';f.res[n+1]=0;}else f.res[cap-1]='+';return;}
    std::size_t k=n;if(n)f.res[k++]=' ';
    for(std::size_t i=0;i<m;++i)f.res[k++]=e[i];
    f.res[k]=0;
}

class FactRing {
public:
    void push(const Fact& f) noexcept {ring[at%Ring]=f;++at;}
    std::uint64_t count() const noexcept {return at;}
    // The last n facts, oldest first (fewer when fewer were recorded).
    template<class Fn> void last(unsigned n,Fn&& fn) const {
        const std::uint64_t k=at<n?at:n;
        for(std::uint64_t i=at-k;i<at;++i)fn(ring[i%Ring]);
    }
    void clear() noexcept {at=0;}
    // The fact with sequence `seq` while it is still in the ring (null otherwise).
    Fact* find(std::uint32_t seq) noexcept {
        const std::uint64_t k=at<Ring?at:Ring;
        for(std::uint64_t i=at-k;i<at;++i)if(ring[i%Ring].seq==seq)return &ring[i%Ring];
        return nullptr;
    }
private:
    std::array<Fact,Ring> ring{};std::uint64_t at=0;
};

// v582: a luma's code, floor(sqrt(luma)) (0..15): fine steps in the dark (0: luma 0, 1: 1..3, 2: 4..8, 3: 9..15 ...).
inline unsigned lumaCode(std::uint8_t l) noexcept {unsigned c=0;while(c<15 && (c+1)*(c+1)<=unsigned(l))++c;return c;}
// v582: a pixel's luma (Rec. 601 weights, integer) from 8-bit R, G, B.
inline std::uint8_t luma(std::uint8_t r,std::uint8_t g,std::uint8_t b) noexcept {return std::uint8_t((299u*r+587u*g+114u*b+500u)/1000u);}
// v582: the cells black in `cur` (code <= 1) that were lit in BOTH neighbours (code >= 3): a shape shown for one frame only. A cut to a
// dark scene stays dark in `next`, and a dark night area is dark in the neighbours too: neither counts.
inline unsigned oddCells(const Fact& prev,const Fact& cur,const Fact& next) noexcept {
    if(!prev.gridOk || !cur.gridOk || !next.gridOk)return 0;
    unsigned n=0;
    for(unsigned i=0;i<GridN;++i)if(lumaCode(cur.grid[i])<=1 && lumaCode(prev.grid[i])>=3 && lumaCode(next.grid[i])>=3)++n;
    return n;
}
// v582: the read-back grid (GridN texels of 4 bytes, RGBA or BGRA) into the fact.
inline void fillGrid(Fact& f,const std::uint8_t* texels,bool bgra) noexcept {
    for(unsigned i=0;i<GridN;++i){const std::uint8_t* t=texels+4*i;f.grid[i]=bgra?luma(t[2],t[1],t[0]):luma(t[0],t[1],t[2]);}
    f.gridOk=1;
}
inline constexpr unsigned OddMin=4; // cells: a flagged frame (G10's smallest feed shape covered ~1/30 of the picture = ~5 cells)

// v582: one draw of a frame, as the key ring keeps it: the vertex and pixel shader keys, its first bound target (EDRAM base | Xenos
// format << 16 | 1 << 31 for a depth-only draw) and its vertex or index count.
struct DrawKey {std::uint64_t vs=0,ps=0;std::uint32_t target=0,n=0;};
inline bool sameDraw(const DrawKey& a,const DrawKey& b) noexcept {return a.vs==b.vs && a.ps==b.ps && a.target==b.target;}
// v582: the draws of the last Frames frames (fixed storage, made once at the first arming; add() is a bounds check and a copy).
template<unsigned Frames,unsigned PerFrame> class KeyRing {
public:
    struct Frame {std::uint32_t seq=0,count=0,overflow=0;bool valid=false;std::array<DrawKey,PerFrame> keys{};};
    void begin(std::uint32_t seq) noexcept {cur=&frames[seq%Frames];cur->seq=seq;cur->count=0;cur->overflow=0;cur->valid=true;}
    void add(const DrawKey& k) noexcept {if(!cur)return;if(cur->count<PerFrame)cur->keys[cur->count++]=k;else ++cur->overflow;}
    const Frame* find(std::uint32_t seq) const noexcept {const auto& f=frames[seq%Frames];return f.valid && f.seq==seq?&f:nullptr;}
    void clear() noexcept {for(auto& f:frames)f.valid=false;cur=nullptr;}
private:
    std::array<Frame,Frames> frames{};Frame* cur=nullptr;
};
struct OddDraw {DrawKey key;unsigned count=0,extra=0;}; // count in the odd frame, extra = count - the larger neighbour's count
// v582: the draws of `cur` that occur more often there than in either neighbour (by vs, ps and target): those in neither neighbour
// first, then the most extra instances. Dump time
// only (allocates).
template<class F> std::vector<OddDraw> oddDraws(const F& prev,const F& cur,const F& next) {
    const auto tally=[](const F& f){std::vector<OddDraw> t;
        for(std::uint32_t i=0;i<f.count;++i){auto it=std::find_if(t.begin(),t.end(),[&](const OddDraw& o){return sameDraw(o.key,f.keys[i]);});
            if(it==t.end())t.push_back({f.keys[i],1,0});else{++it->count;it->key.n=std::max(it->key.n,f.keys[i].n);}}
        return t;};
    const auto a=tally(prev),c=tally(cur),b=tally(next);
    std::vector<OddDraw> out;
    for(const auto& o:c) {
        unsigned m=0;
        for(const auto* t:{&a,&b})for(const auto& x:*t)if(sameDraw(x.key,o.key))m=std::max(m,x.count);
        if(o.count>m)out.push_back({o.key,o.count,o.count-m});
    }
    std::stable_sort(out.begin(),out.end(),[](const OddDraw& x,const OddDraw& y){ // in neither neighbour first, then most extra
        const bool nx=x.count==x.extra,ny=y.count==y.extra;return nx!=ny?nx:x.extra>y.extra;});
    return out;
}

// " p=present f=frame t=wallMs d=draws r=resolves sr=shadowResolves cd=casterDraws sd=shadowDraws ss=stale ps=passes
//   light=ps/age sun=x,y,z col=r,g,b,a tm=tonemap cam=0|1"
inline std::string text(const Fact& f) {
    char b[640];
    std::snprintf(b,sizeof b," p=%llu f=%llu t=%lld d=%u r=%u sr=%u cd=%u sd=%u ss=%u ps=%u light=%08x/%d sun=%.4g,%.4g,%.4g col=%.4g,%.4g,%.4g,%.4g tm=%u cam=%u",
                  (unsigned long long)f.present,(unsigned long long)f.frame,(long long)f.wallMs,f.draws,f.resolves,f.shadowResolves,f.casterDraws,
                  f.shadowDraws,f.staleShadowDraws,f.passes,f.lightPs,f.shadowAge==~0u?-1:int(f.shadowAge),double(f.sunDir[0]),double(f.sunDir[1]),
                  double(f.sunDir[2]),double(f.sunColour[0]),double(f.sunColour[1]),double(f.sunColour[2]),double(f.sunColour[3]),unsigned(f.tonemap),unsigned(f.camera));
    // v579: " sun=<draws> mdraw=<mask draws> mask=<vkformat>/<age>/<image tag> probe=<state>:r,g,b,a;..." 
    std::string s=b;char c[96];
    std::snprintf(c,sizeof c," refused=%u/%u%s%s",f.drawRefused,f.resolveRefused,f.firstRefuse[0]?":":"",f.firstRefuse);s+=c;
    std::snprintf(c,sizeof c," sund=%u mdraw=%u mask=%d/%d/%04x probe=%u",unsigned(f.sunDraws),unsigned(f.maskDraws),int(f.maskFormat),
                  f.maskAge==~0u?-1:int(f.maskAge),unsigned(f.maskImage&0xFFFFu),unsigned(f.probe));s+=c;
    if(f.probe==1)for(unsigned q=0;q<4;++q){std::snprintf(c,sizeof c,"%s%.3g,%.3g,%.3g,%.3g",q?";":":",double(f.maskRgba[q][0]),double(f.maskRgba[q][1]),
                                                         double(f.maskRgba[q][2]),double(f.maskRgba[q][3]));s+=c;}
    // v581: " res=<events joined by ','>" ("-": none); the dump's " |" fact separator never occurs in it
    std::string r=f.res[0]?f.res:"-";for(auto& ch:r)if(ch==' ')ch=',';
    s+=" res=";s+=r;
    // v582: " lg=<GridN hex digits, row by row>" (lumaCode of each cell; "-": not read)
    if(f.skoBad){char k[24];std::snprintf(k,sizeof k," sko=%u",unsigned(f.skoBad));s+=k;} // v584 (only when > 0)
    if(f.skoUs){char k[24];std::snprintf(k,sizeof k," skous=%u",unsigned(f.skoUs));s+=k;} // v584: the scan's cost (only when > 0)
    s+=" lg=";
    if(f.gridOk){for(unsigned i=0;i<GridN;++i)s+="0123456789abcdef"[lumaCode(f.grid[i])];}else s+="-";
    return s;
}
// A running counter's increase since `prev` (a counter that was reset in between counts from 0).
inline std::uint32_t delta(std::uint64_t now,std::uint64_t& prev) noexcept {
    const std::uint64_t d=now>=prev?now-prev:now;prev=now;return std::uint32_t(d>0xFFFFFFFFull?0xFFFFFFFFull:d);
}

} // namespace recomp::gpu::generic::ff
