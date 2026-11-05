// Attempt v55: EDRAM tile geometry for clear aliasing. DIAGNOSTIC ONLY.
// The 360 keeps every render target in one 10 MB EDRAM of 2048 tiles; at 1x MSAA and 32 bpp a tile
// holds 80x16 pixels and a surface of pitch P covers ceil(P/80) tiles per tile row from its base,
// wrapping at 2048. The generic renderer keeps one image per (base, format, pitch), so a clear only
// changed the cleared image; on the hardware it also changes every other surface whose tiles overlap.
// GTA V relies on that: the G-buffer resolves (predicated tiling, 384-row screen tiles) clear EDRAM
// 384..1535 and the lighting buffer at 720 then accumulates onto those zeros (v52 census).
// `rows` is the number of pixel rows a surface has in EDRAM in the pass that last drew it (the rows
// before the next bound base); a tiled pass draws screen row y at EDRAM row y mod rows.
// Pure functions; host test: local-analysis/attempts/v55/tests/edram_tile_alias_test.cpp.
#pragma once
#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <vector>

namespace recomp::gpu::generic::edram {
constexpr unsigned Tiles=2048,TileWidth=80,TileHeight=16;
struct Surface {unsigned base=0,pitch=0,rows=0,height=0;}; // rows 0: the whole image height is in EDRAM
struct Rect {std::int32_t x0=0,y0=0,x1=0,y1=0;};
// The cleared part of one tile in tile-local pixels (x in 0..80, y in 0..16); x1 == 0 means none.
struct Local {std::uint8_t x0=0,y0=0,x1=0,y1=0;bool any()const{return x1>x0 && y1>y0;}};
// v58: `mask` marks the tiles with a cleared part, for a cheap footprint test before rectsIn.
// v302: `words` holds the same bits as `mask`, 64 tiles per word (range tests without 2048-bit shifts).
struct Cleared {std::array<Local,Tiles> tile{};std::bitset<Tiles> mask;std::array<std::uint64_t,Tiles/64> words{};};

inline unsigned tilesPerRow(const Surface& s){return (s.pitch+TileWidth-1)/TileWidth;}
inline unsigned edramRows(const Surface& s){
    const unsigned h=s.rows?std::min(s.rows,s.height):s.height;
    return std::max((h+TileHeight-1)/TileHeight*TileHeight,TileHeight);
}
inline void add(Local& l,unsigned x0,unsigned y0,unsigned x1,unsigned y1){
    if(!l.any()){l={std::uint8_t(x0),std::uint8_t(y0),std::uint8_t(x1),std::uint8_t(y1)};return;}
    // Two screen tiles of a tiled pass land on one EDRAM tile: keep the bounding box.
    l.x0=std::uint8_t(std::min<unsigned>(l.x0,x0));l.y0=std::uint8_t(std::min<unsigned>(l.y0,y0));
    l.x1=std::uint8_t(std::max<unsigned>(l.x1,x1));l.y1=std::uint8_t(std::max<unsigned>(l.y1,y1));
}
// Records the EDRAM pixels a clear of [x0,x1) x [y0,y1) (image coordinates of s) writes.
inline void clearedTiles(Cleared& out,const Surface& s,std::int32_t x0,std::int32_t y0,std::int32_t x1,std::int32_t y1) {
    const unsigned tpr=tilesPerRow(s),rows=edramRows(s);
    x0=std::max(x0,0);y0=std::max(y0,0);
    x1=std::min<std::int32_t>(x1,std::int32_t(s.pitch));y1=std::min<std::int32_t>(y1,std::int32_t(s.height));
    if(x1<=x0 || y1<=y0 || !tpr)return;
    for(unsigned ty=unsigned(y0)/TileHeight;ty*TileHeight<unsigned(y1);++ty) {
        const unsigned top=ty*TileHeight;
        const unsigned ly0=std::max(unsigned(y0),top)-top,ly1=std::min(unsigned(y1),top+TileHeight)-top;
        const unsigned edramRow=(top%rows)/TileHeight;
        for(unsigned tc=unsigned(x0)/TileWidth;tc<tpr && tc*TileWidth<unsigned(x1);++tc) {
            const unsigned left=tc*TileWidth;
            const unsigned lx0=std::max(unsigned(x0),left)-left,lx1=std::min(unsigned(x1),left+TileWidth)-left;
            const unsigned t=(s.base+edramRow*tpr+tc)%Tiles;
            out.mask.set(t);out.words[t/64]|=std::uint64_t{1}<<(t%64);add(out.tile[t],lx0,ly0,lx1,ly1);
        }
    }
}
// v63: the EDRAM rows a surface at `base` (pitch `pitch`) has before the next configured surface base
// above it (wrapping at 2048); 0 when no other base constrains it. `bases` are every base the pass
// configures (RB_COLOR_INFO 0-3 and RB_DEPTH_INFO), written or masked: a colour-masked slot still owns
// its EDRAM (v62: a DrawVertices draw binding only A and D made D look full height, and C's resolve
// clear then wiped D's lower tile before D was resolved).
template<class Bases> unsigned rowsBeforeNextBase(unsigned base,unsigned pitch,const Bases& bases) {
    const unsigned tpr=(pitch+TileWidth-1)/TileWidth;
    unsigned next=Tiles;
    for(const unsigned b:bases){const unsigned d=(b-base)&(Tiles-1);if(d && d<next)next=d;}
    return next<Tiles && tpr?next/tpr*TileHeight:0;
}
// v63: a pass's EDRAM rows: all its surfaces share RB_SURFACE_INFO's pitch and one screen tile height, so
// the smallest gap between configured bases bounds every surface (the last base, bounded only by the
// wrap, would otherwise look taller than its tile). 0 when no base constrains another.
template<class Bases> unsigned passRows(unsigned pitch,const Bases& bases) {
    unsigned rows=0;
    for(const unsigned b:bases){const unsigned r=rowsBeforeNextBase(b,pitch,bases);if(r && (!rows || r<rows))rows=r;}
    return rows;
}
// v58: whether any tile of d's EDRAM footprint (its rows from its base, wrapping) has a cleared part.
inline bool touches(const Cleared& c,const Surface& d) {
    const unsigned n=std::min(Tiles,tilesPerRow(d)*(edramRows(d)/TileHeight));
    if(!n || c.mask.none())return false;
    std::bitset<Tiles> f;f.set();f>>=Tiles-n;           // tiles [0, n)
    const unsigned b=d.base%Tiles;
    if(b)f=(f<<b)|(f>>(Tiles-b));                       // rotated to [base, base+n) mod 2048
    return (f&c.mask).any();
}
// Image rectangles of d whose EDRAM pixels `c` records. A tiled surface (rows < height) holds each EDRAM
// row once per screen tile, so the rectangles repeat every `rows` image rows.
inline std::vector<Rect> rectsIn(const Cleared& c,const Surface& d) {
    std::vector<Rect> out,rowRects;
    const unsigned tpr=tilesPerRow(d),rows=edramRows(d);
    if(!tpr || !d.height)return out;
    // v61: rectangles of one EDRAM row merge with the previous row's when their columns match and they
    // touch (a full-width clear becomes one rectangle, not one per 16-row tile row).
    std::vector<Rect> open,next;
    const auto emit=[&](Rect r) {
        r.x1=std::min<std::int32_t>(r.x1,std::int32_t(d.pitch));
        if(r.x1<=r.x0)return;
        for(auto& o:open)if(o.x0==r.x0 && o.x1==r.x1 && o.y1==r.y0){o.y1=r.y1;next.push_back(o);o.x1=o.x0;return;}
        next.push_back(r);
    };
    const auto closeRow=[&] {
        for(const auto& o:open)if(o.x1>o.x0)rowRects.push_back(o);
        open.swap(next);next.clear();
    };
    for(unsigned er=0;er*TileHeight<rows;++er) {
        Rect run{};bool isOpen=false;
        for(unsigned tc=0;tc<tpr;++tc) {
            const auto& l=c.tile[(d.base+er*tpr+tc)%Tiles];
            if(!l.any()){if(isOpen){emit(run);isOpen=false;}continue;}
            const Rect r{std::int32_t(tc*TileWidth+l.x0),std::int32_t(er*TileHeight+l.y0),
                         std::int32_t(tc*TileWidth+l.x1),std::int32_t(er*TileHeight+l.y1)};
            if(isOpen && run.x1==r.x0 && run.y0==r.y0 && run.y1==r.y1)run.x1=r.x1;
            else{if(isOpen)emit(run);run=r;isOpen=true;}
        }
        if(isOpen)emit(run);
        closeRow();
    }
    closeRow();
    // A tiled surface holds each EDRAM row once per screen tile: repeat every `rows` image rows.
    for(const auto& r:rowRects)
        for(unsigned offset=0;offset<d.height;offset+=rows) {
            const std::int32_t y0=r.y0+std::int32_t(offset),y1=std::min<std::int32_t>(r.y1+std::int32_t(offset),std::int32_t(d.height));
            if(y1>y0)out.push_back({r.x0,y0,r.x1,y1});
            if(rows>=d.height)break;
        }
    return out;
}
// v302: whether any of tiles [from, from+n) (mod 2048) has a cleared part; n <= 2048.
inline bool anyBits(const std::array<std::uint64_t,Tiles/64>& w,unsigned from,unsigned n) {
    if(!n)return false;
    if(n>=Tiles){for(const auto x:w)if(x)return true;return false;}
    from%=Tiles;
    const auto seg=[&](unsigned a,unsigned b) { // tiles [a, b) inside [0, 2048)
        while(a<b) {
            const unsigned bit=a%64,take=std::min(64u-bit,b-a);
            const std::uint64_t m=(take==64?~std::uint64_t{0}:((std::uint64_t{1}<<take)-1))<<bit;
            if(w[a/64]&m)return true;
            a+=take;
        }
        return false;
    };
    const unsigned end=from+n;
#if defined(RECOMP_V302_MUTANT_NO_WRAP) // test mutant: a footprint that wraps past tile 2047 loses its wrapped part
    return seg(from,std::min(end,Tiles));
#else
    return end<=Tiles?seg(from,end):(seg(from,Tiles) || seg(0,end-Tiles));
#endif
}
// v302: touches() over the footprint's words only (same answer).
inline bool touchesFast(const Cleared& c,const Surface& d) {
    const unsigned n=std::min(Tiles,tilesPerRow(d)*(edramRows(d)/TileHeight));
    return n && anyBits(c.words,d.base%Tiles,n);
}
// v302: rectsIn() skipping EDRAM rows none of whose tiles has a cleared part (same rectangles, same order): such a row
// emits nothing, and its closeRow only moves the rectangles still open into the result (nothing when none is open).
inline std::vector<Rect> rectsInFast(const Cleared& c,const Surface& d) {
    std::vector<Rect> out,rowRects;
    const unsigned tpr=tilesPerRow(d),rows=edramRows(d);
    if(!tpr || !d.height)return out;
    std::vector<Rect> open,next;
    const auto emit=[&](Rect r) {
        r.x1=std::min<std::int32_t>(r.x1,std::int32_t(d.pitch));
        if(r.x1<=r.x0)return;
        for(auto& o:open)if(o.x0==r.x0 && o.x1==r.x1 && o.y1==r.y0){o.y1=r.y1;next.push_back(o);o.x1=o.x0;return;}
        next.push_back(r);
    };
    const auto closeRow=[&] {
        for(const auto& o:open)if(o.x1>o.x0)rowRects.push_back(o);
        open.swap(next);next.clear();
    };
    for(unsigned er=0;er*TileHeight<rows;++er) {
        if(!anyBits(c.words,d.base+er*tpr,tpr)){if(!open.empty())closeRow();continue;}
        Rect run{};bool isOpen=false;
        for(unsigned tc=0;tc<tpr;++tc) {
            const auto& l=c.tile[(d.base+er*tpr+tc)%Tiles];
            if(!l.any()){if(isOpen){emit(run);isOpen=false;}continue;}
            const Rect r{std::int32_t(tc*TileWidth+l.x0),std::int32_t(er*TileHeight+l.y0),
                         std::int32_t(tc*TileWidth+l.x1),std::int32_t(er*TileHeight+l.y1)};
            if(isOpen && run.x1==r.x0 && run.y0==r.y0 && run.y1==r.y1)run.x1=r.x1;
            else{if(isOpen)emit(run);run=r;isOpen=true;}
        }
        if(isOpen)emit(run);
        closeRow();
    }
    closeRow();
    for(const auto& r:rowRects)
        for(unsigned offset=0;offset<d.height;offset+=rows) {
            const std::int32_t y0=r.y0+std::int32_t(offset),y1=std::min<std::int32_t>(r.y1+std::int32_t(offset),std::int32_t(d.height));
            if(y1>y0)out.push_back({r.x0,y0,r.x1,y1});
            if(rows>=d.height)break;
        }
    return out;
}
// v304: the same pixels in fewer rectangles. Rectangles with the same columns whose rows touch or overlap become their union
// (exactly a rectangle), then rectangles with the same rows whose columns touch or overlap, repeated until nothing merges.
// Every input pixel is in the output and nothing else; the order changes (a clear of all of them writes the same pixels).
// v303 device tally: 1280-pitch targets at EDRAM bases 32/64/96 took 72-168 rectangles per frame in one call, rectsIn's
// repeats every `rows` (32) image rows, which touch. Host test: tests/merge_rects_test.cpp.
inline void mergeRects(std::vector<Rect>& rs) {
    if(rs.size()<2)return;
    std::vector<Rect> out;out.reserve(rs.size());
    for(bool changed=true;changed;) {
        changed=false;
        std::sort(rs.begin(),rs.end(),[](const Rect& a,const Rect& b){
            return a.x0!=b.x0?a.x0<b.x0:a.x1!=b.x1?a.x1<b.x1:a.y0!=b.y0?a.y0<b.y0:a.y1<b.y1;});
        out.clear();
        for(const auto& r:rs) {
#if defined(RECOMP_V304_MUTANT_GAP) // test mutant: rectangles up to one tile row apart are joined (the gap's pixels are added)
            if(!out.empty() && out.back().x0==r.x0 && out.back().x1==r.x1 && r.y0<=out.back().y1+std::int32_t(TileHeight))
#else
            if(!out.empty() && out.back().x0==r.x0 && out.back().x1==r.x1 && r.y0<=out.back().y1)
#endif
                {out.back().y1=std::max(out.back().y1,r.y1);changed=true;}
            else out.push_back(r);
        }
        rs.swap(out);
        std::sort(rs.begin(),rs.end(),[](const Rect& a,const Rect& b){
            return a.y0!=b.y0?a.y0<b.y0:a.y1!=b.y1?a.y1<b.y1:a.x0!=b.x0?a.x0<b.x0:a.x1<b.x1;});
        out.clear();
        for(const auto& r:rs) {
            if(!out.empty() && out.back().y0==r.y0 && out.back().y1==r.y1 && r.x0<=out.back().x1){out.back().x1=std::max(out.back().x1,r.x1);changed=true;}
            else out.push_back(r);
        }
        rs.swap(out);
    }
}
}
