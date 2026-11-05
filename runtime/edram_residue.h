// Attempt v310: the EDRAM a predicated-tiled pass leaves behind, for a surface that is bound again without tiling.
// The 360 draws a tiled pass once per screen tile, so after it every surface's EDRAM holds its LAST screen tile at
// tile-local rows. The generic renderer draws the pass once over whole images instead; that matches the hardware for the
// per-tile resolves, but not for a surface bound afterwards with more rows: its image rows past its tiled rows lie, in
// EDRAM, over the next surface of the tiled pass, i.e. that surface's last tile.
// GTA V relies on it at 1280x720 (824D6860: backbuffer 1280x720 and 837C1E18 set): the G-buffer resolve loop (82312788,
// census frame of 2026-09-30) skips G-buffer 2's bottom-tile resolve; 82481770 then draws G-buffer 2's resolved top tile
// into the surface at EDRAM 1152 (G-buffer 3's base, whose rows 384-719 are EDRAM 1536-1871: G-buffer 2's bottom tile) and
// resolves 1152 whole into G-buffer 2's texture. Without this, rows 384-719 of that texture held G-buffer 3's cleared rows:
// the lower half of the screen lit dark.
// Planned here (pure; host test tests/edram_residue_test.cpp): for a surface T whose tiled rows were r, each band j >= 1 of
// r image rows inside the new pass's rows comes from the surface S at T's base + j*r rows (same pitch and format, drawn by
// a tiled pass of r rows), rows [H-r, H) of S (its last tile; H the image height, a multiple of r), unless a pass attached a
// surface overlapping S's EDRAM after S's last pass (then the hardware bits there are not S's). Band 0 (T's own EDRAM, its
// own last tile) is not remapped: not needed by any use seen (82481770 redraws it), counted by the renderer as kept.
#pragma once
#include "edram_tile_alias.h"
#include <array>
#include <cstdint>
#include <vector>

namespace recomp::gpu::generic::v310 {
// v583 (fixer3, G7: the sun lost below row 384 for one frame): a pass that ATTACHES a surface overlapping S's EDRAM does not
// overwrite S's bits there; only its draws and clears do, inside their scissor rectangles. The radar (minimap) target at EDRAM
// 1440 (pitch 560, 512 rows: tiles 1440-1663, over the band 1536-1871) is sometimes drawn between the G-buffer and 82481770;
// every one of its draws is scissored to (0,0)-(190,135) (censuses: 2673 + 2515 + ... draws, two scissors), i.e. tiles 1440-1498,
// none in the band. v310 called the band stale on the attachment alone (v582 facts: S1152@1536<1440:0:560/512, A1152:0 on those
// frames, bottom probes 0): the shadow mask's rows 384-719 kept 1152's cleared zeros. Now each surface keeps the rectangles its
// last passes wrote (Write: the pass's clock and EDRAM rows, the union of its draw scissors and clear rectangles), and a surface
// is a culprit only when a rectangle written after S's last pass covers a band tile. Unknown writes (64 bpp, MSAA, more passes
// than the ring keeps since S) keep v310's whole-footprint test.
struct Write {std::uint64_t clock=0;unsigned rows=0;std::int32_t x0=0,y0=0,x1=0,y1=0;};
inline constexpr unsigned WriteRing=4;
struct Surface {
    unsigned base=0,pitch=0,height=0,format=0,samples=0;bool depth=false;
    unsigned edramRows=0;      // rows its last pass gave it (edram::Surface::rows; 0 = the whole image)
    unsigned tiledRows=0;      // rows of the tiled pass that last drew it (0: its last pass was not tiled)
    std::uint64_t passClock=0; // the renderer's write clock at the last pass that attached it
    std::array<Write,WriteRing> writes{};unsigned writesN=0; // v583: its last passes (writesN counts all; the ring keeps the last WriteRing)
    bool writesKnown=false;    // v583: the rectangles are exact for its tiles (1x, 32 bpp); false: v310's footprint test
};
struct Copy {std::size_t source=0;std::int32_t srcY=0,dstY=0,rows=0;};
struct Plan {std::vector<Copy> copies;unsigned missing=0,stale=0,spared=0;bool unsupported=false;
    std::size_t sparedBy=~std::size_t(0); // v583: the first surface v310's footprint test would have called a culprit, whose writes miss the band
    // v581 (DIAGNOSTIC, G7): the first band left out and why. staleBy: the surface attached over S's band after S's last pass;
    // missingWhy: of the surfaces at the band's base the first field that differs ('n' none there, 'p' pitch, 'd' depth, 'f' format,
    // 'h' height, 's' samples, 'r' tiled rows), missingNear: that surface. Neither changes which bands are copied.
    std::size_t staleBy=~std::size_t(0),missingNear=~std::size_t(0);char missingWhy=0;unsigned firstBadBase=0;};

inline unsigned footprintTiles(const Surface& s) {
    const edram::Surface e{s.base%edram::Tiles,s.pitch,s.edramRows,s.height};
    return std::min(edram::Tiles,edram::tilesPerRow(e)*(edram::edramRows(e)/edram::TileHeight));
}
// Whether EDRAM tiles [a, a+n) and [b, b+m) (mod 2048) share a tile.
inline bool overlaps(unsigned a,unsigned n,unsigned b,unsigned m) {
    if(!n || !m)return false;
    const unsigned ab=(b-a)&(edram::Tiles-1),ba=(a-b)&(edram::Tiles-1);
    return ab<n || ba<m;
}
// v583: whether c wrote any EDRAM tile in [base, base+n) (mod 2048) in a pass after `after` (S's last pass clock).
inline bool drewOver(const Surface& c,unsigned base,unsigned n,std::uint64_t after) {
    if(c.passClock<=after)return false;
    const auto footprint=[&]{return overlaps(base,n,c.base%edram::Tiles,footprintTiles(c));};
#if defined(RECOMP_V583_MUTANT_FOOTPRINT) // test mutant: v310's attachment test (the radar pass spoils the band again)
    return footprint();
#endif
    const unsigned kept=std::min(c.writesN,WriteRing);
    if(!c.writesKnown || !kept)return footprint();
    std::uint64_t oldest=~0ull;for(unsigned k=0;k<kept;++k)oldest=std::min(oldest,c.writes[k].clock);
    if(c.writesN>WriteRing && oldest>after)return footprint(); // passes after S fell out of the ring
    for(unsigned k=0;k<kept;++k) {
        const Write& w=c.writes[k];
        if(w.clock<=after || w.x1<=w.x0 || w.y1<=w.y0)continue;
        const edram::Surface e{c.base%edram::Tiles,c.pitch,w.rows,c.height};
        const unsigned tpr=edram::tilesPerRow(e),rows=edram::edramRows(e);
        const std::int32_t x1=std::min<std::int32_t>(w.x1,std::int32_t(c.pitch)),y1=std::min<std::int32_t>(w.y1,std::int32_t(c.height));
        for(unsigned ty=unsigned(std::max(w.y0,0))/edram::TileHeight;std::int32_t(ty*edram::TileHeight)<y1;++ty) {
            const unsigned row=((ty*edram::TileHeight)%rows)/edram::TileHeight;
            for(unsigned tc=unsigned(std::max(w.x0,0))/edram::TileWidth;tc<tpr && std::int32_t(tc*edram::TileWidth)<x1;++tc)
                if(((e.base+row*tpr+tc-base)&(edram::Tiles-1))<n)return true;
        }
    }
    return false;
}
// T = s[t], whose tiled pass gave it `rows` rows, bound now by a pass of `newRows` rows (0: the whole image).
inline Plan plan(const std::vector<Surface>& s,std::size_t t,unsigned rows,unsigned newRows) {
    Plan p;const auto& T=s[t];const unsigned H=T.height;
    if(!rows || rows%edram::TileHeight || !H || H%rows || T.depth || T.samples || !T.pitch){p.unsupported=true;return p;}
    const unsigned extent=newRows?std::min(newRows,H):H;
    const unsigned tpr=(T.pitch+edram::TileWidth-1)/edram::TileWidth,bandTiles=rows/edram::TileHeight*tpr;
    const unsigned last=H-rows; // the first image row of the last screen tile
    for(unsigned j=1;j*rows<extent;++j) {
        const unsigned base=(T.base+j*bandTiles)%edram::Tiles;
        std::size_t k=s.size();
        for(std::size_t i=0;i<s.size();++i) {
            const auto& c=s[i];
            if(i!=t && c.base%edram::Tiles==base && c.pitch==T.pitch && !c.depth && c.format==T.format && c.height==H && !c.samples &&
               c.tiledRows==rows){k=i;break;}
        }
        if(k==s.size()) {
            if(!p.missingWhy && !p.stale) { // v581: the nearest surface at that base and its first differing field
                p.missingWhy='n';p.firstBadBase=base;
                for(std::size_t i=0;i<s.size();++i) {
                    const auto& c=s[i];if(i==t || c.base%edram::Tiles!=base)continue;
                    const char w=c.pitch!=T.pitch?'p':c.depth?'d':c.format!=T.format?'f':c.height!=H?'h':c.samples?'s':'r';
                    if(p.missingWhy=='n' || w=='r'){p.missingWhy=w;p.missingNear=i;}
                }
            }
            ++p.missing;continue;
        }
        bool stale=false;
#if !defined(RECOMP_V310_MUTANT_NO_STALE) // test mutant: a surface drawn over S's EDRAM after S does not stop the copy
        for(std::size_t i=0;i<s.size() && !stale;++i)
            if(i!=t && i!=k && s[i].passClock>s[k].passClock && overlaps(base,bandTiles,s[i].base%edram::Tiles,footprintTiles(s[i]))) {
                if(!drewOver(s[i],base,bandTiles,s[k].passClock)){ // v583: attached over the band, but wrote none of its tiles
                    ++p.spared;if(p.sparedBy==~std::size_t(0))p.sparedBy=i;continue;
                }
                stale=true;if(p.staleBy==~std::size_t(0) && !p.missingWhy){p.staleBy=i;p.missingNear=k;p.firstBadBase=base;} // (v581: the culprit)
            }
#endif
        if(stale){++p.stale;continue;}
        const std::int32_t n=std::int32_t(std::min(rows,extent-j*rows));
#if defined(RECOMP_V310_MUTANT_SAME_ROWS) // test mutant: S's rows at the same screen rows as the band, not S's last tile
        p.copies.push_back({k,std::int32_t(j*rows),std::int32_t(j*rows),n});
#else
        p.copies.push_back({k,std::int32_t(last),std::int32_t(j*rows),n});
#endif
    }
    return p;
}
}
