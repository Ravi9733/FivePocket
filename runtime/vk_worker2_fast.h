#pragma once
// vk-worker-2 (2026-10-03): exact memos for the Vulkan worker's EDRAM alias-clear bookkeeping (generic_renderer.cpp aliasClear,
// diag-vkw2-*). Host tests: tools/vk_worker2/tests/vkw2_memo_test.cpp (randomized differential against the computations
// below, small tables for collisions, mutants) and the renderer harness (run_vkw2.sh: raw vkCmdClearAttachments streams).
//
// Why: aliasClear is 4.3% of the worker in the v425 outdoor profile (~0.9 ms per frame) and 5.2% in the v414 house one (~0.6 ms),
// mostly in resolves (the G-buffer's predicated-tiling resolve clears alias into the lighting surfaces). Per frame ~120-160
// (clear, other surface) pairs each recompute the other surface's rectangles from the cleared tiles (rectsInFast: four vectors,
// row runs; mergeRects: sorts) and append them to that surface's pending list (appendPending: an any_of cover test per rectangle,
// then mergeRects over the whole list, 70-170 rectangles: two to four sorts) - the same inputs every frame.
//
// Both memos store a computation's inputs and its result and return the stored result only for inputs equal to the stored
// ones (every byte compared), so a hit returns exactly what the computation returns:
//  - TargetRects: aliasClear's rectangles of surface u for a clear of surface x over rect = mergeRects(rectsInFast(c, u)),
//    c = clearedTiles(an empty Cleared, x, rect). A pure function of (x, rect, u): clearedTiles reads only those, rectsInFast
//    only c and u, mergeRects only its vector. Key = the two Surfaces and the rectangle (48 bytes).
//  - PendingRects: appendPending's update of the entry's rectangle list (after its choice of entry): append each new rectangle
//    no rectangle of the list (as it grows) covers, then with merge mergeRects(list). A pure function of (list, new, merge).
#include "edram_tile_alias.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace recomp::gpu::generic::vkw2 {
using edram::Rect;
static_assert(sizeof(Rect)==16 && sizeof(edram::Surface)==16);

inline std::uint64_t mix(std::uint64_t h,std::uint64_t v){h^=v+0x9E3779B97F4A7C15ull+(h<<6)+(h>>2);return h*0xFF51AFD7ED558CCDull;}

// ---- TargetRects
struct AliasKey {
    edram::Surface x{};std::array<std::int32_t,4> rect{};edram::Surface u{};
};
static_assert(sizeof(AliasKey)==48);
inline bool sameKey(const AliasKey& a,const AliasKey& b){
#if defined(RECOMP_VKW2_MUTANT_KEY_NO_ROWS) // test mutant: u's EDRAM rows left out of the comparison (a tiled pass's rows change it)
    AliasKey c=b;c.u.rows=a.u.rows;return !std::memcmp(&a,&c,sizeof a);
#elif defined(RECOMP_VKW2_MUTANT_KEY_NO_RECT) // test mutant: the cleared rectangle's bottom row left out of the comparison
    AliasKey c=b;c.rect[3]=a.rect[3];return !std::memcmp(&a,&c,sizeof a);
#else
    return !std::memcmp(&a,&b,sizeof a);
#endif
}
// The computation TargetRects stands for (aliasClear's default path: v302 rectsInFast, v304 mergeRects).
inline std::vector<Rect> targetRects(const edram::Cleared& c,const edram::Surface& u){auto r=edram::rectsInFast(c,u);edram::mergeRects(r);return r;}

template<unsigned Bits> struct TargetRectsMemo {
    struct Entry {AliasKey key{};bool used=false;std::vector<Rect> rects;};
    std::array<Entry,std::size_t{1}<<Bits> e{};
    static std::size_t index(AliasKey k) {
#if defined(RECOMP_VKW2_MUTANT_KEY_NO_ROWS)
        k.u.rows=0;
#elif defined(RECOMP_VKW2_MUTANT_KEY_NO_RECT)
        k.rect[3]=0;
#endif
        std::uint64_t w[6];std::memcpy(w,&k,sizeof w);std::uint64_t h=0x51ED270B27A6A7A1ull;
        for(const auto x:w)h=mix(h,x);
        return std::size_t(h>>(64-Bits));
    }
    // The stored rectangles for k, or nullptr.
    const std::vector<Rect>* find(const AliasKey& k) const {const auto& x=e[index(k)];return x.used && sameKey(x.key,k)?&x.rects:nullptr;}
    void store(const AliasKey& k,const std::vector<Rect>& r) {auto& x=e[index(k)];x.used=false;x.rects=r;x.key=k;x.used=true;}
    void clear(){for(auto& x:e){x.used=false;x.rects=std::vector<Rect>{};}}
};

// ---- PendingRects
// The computation PendingRects stands for: appendPending's loop and merge over the chosen entry's list (generic_renderer.cpp
// v96/v304, copied verbatim).
inline void appendRects(std::vector<Rect>& list,const std::vector<Rect>& rects,bool merge) {
    for(const auto& r:rects) {
        const bool covered=std::any_of(list.begin(),list.end(),[&](const Rect& e){
            return e.x0<=r.x0 && e.y0<=r.y0 && e.x1>=r.x1 && e.y1>=r.y1;});
        if(!covered)list.push_back(r);
    }
    if(merge)edram::mergeRects(list);
}
inline bool sameRects(const std::vector<Rect>& a,const std::vector<Rect>& b) {
    return a.size()==b.size() && (a.empty() || !std::memcmp(a.data(),b.data(),a.size()*sizeof(Rect)));
}
template<unsigned Bits> struct PendingRectsMemo {
    struct Entry {bool used=false,merge=false;std::vector<Rect> before,add,after;};
    std::array<Entry,std::size_t{1}<<Bits> e{};
    // The slot: from the sizes, merge, every new rectangle and the list's first and last (the full lists are compared at a hit).
    static std::size_t index(const std::vector<Rect>& list,const std::vector<Rect>& add,bool merge) {
        std::uint64_t h=mix(0x2545F4914F6CDD1Dull,(std::uint64_t(list.size())<<33)|(std::uint64_t(add.size())<<1)|(merge?1u:0u));
        const auto put=[&](const Rect& r){std::uint64_t w[2];std::memcpy(w,&r,16);h=mix(mix(h,w[0]),w[1]);};
        for(const auto& r:add)put(r);
        if(!list.empty()){put(list.front());put(list.back());}
        return std::size_t(h>>(64-Bits));
    }
    // With (list, add, merge) stored at slot i: list becomes the stored result; true. Else false (list unchanged).
    bool apply(std::size_t i,std::vector<Rect>& list,const std::vector<Rect>& add,bool merge) const {
        const auto& x=e[i];
#if defined(RECOMP_VKW2_MUTANT_PENDING_NO_LIST) // test mutant: a hit compares the new rectangles and the list's size only (not its rectangles)
        if(!x.used || x.merge!=merge || !sameRects(x.add,add) || x.before.size()!=list.size())return false;
#elif defined(RECOMP_VKW2_MUTANT_PENDING_NO_ADD) // test mutant: a hit compares the list only (not the new rectangles)
        if(!x.used || x.merge!=merge || !sameRects(x.before,list))return false;
#else
        if(!x.used || x.merge!=merge || !sameRects(x.add,add) || !sameRects(x.before,list))return false;
#endif
        list.assign(x.after.begin(),x.after.end());
        return true;
    }
    // Computes the update at slot i, stores it, and applies it to list.
    void compute(std::size_t i,std::vector<Rect>& list,const std::vector<Rect>& add,bool merge) {
        auto& x=e[i];x.used=false;
        x.before.assign(list.begin(),list.end());x.add.assign(add.begin(),add.end());x.merge=merge;
        appendRects(list,add,merge);
        x.after.assign(list.begin(),list.end());x.used=true;
    }
    void clear(){for(auto& x:e){x.used=false;x.before=x.add=x.after=std::vector<Rect>{};}}
};

} // namespace recomp::gpu::generic::vkw2
