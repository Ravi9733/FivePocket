// Attempt v301: whether clear rectangles cover a render pass's whole area [0,w) x [0,h), each rectangle clipped to it.
// Exact: one covering rectangle answers at once; otherwise, with at most MaxRects rectangles whose clipped areas add up to
// at least w*h, a coordinate-compressed grid (every cell between consecutive edges must lie in some rectangle). More
// rectangles answer false: the caller then keeps vkCmdClearAttachments (never a wrong "covered").
// Pure; host test: tests/pass_clear_cover_test.cpp (brute-force raster oracle; RECOMP_V301_MUTANT_AREA_SUM must fail).
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "edram_tile_alias.h"

namespace recomp::gpu::generic::v301 {
constexpr std::size_t MaxRects=16;
inline bool coversArea(const std::vector<edram::Rect>& rects,std::int32_t w,std::int32_t h) {
    if(w<=0 || h<=0 || rects.empty())return false;
    std::int64_t area=0;
    for(const auto& r:rects) {
        const std::int32_t x0=std::max(r.x0,0),y0=std::max(r.y0,0),x1=std::min(r.x1,w),y1=std::min(r.y1,h);
        if(x1<=x0 || y1<=y0)continue;
        if(x0==0 && y0==0 && x1==w && y1==h)return true;
        area+=std::int64_t(x1-x0)*(y1-y0);
    }
    if(area<std::int64_t(w)*h || rects.size()>MaxRects)return false;
#if defined(RECOMP_V301_MUTANT_AREA_SUM) // test mutant: overlapping rectangles counted twice pass as covering
    return true;
#endif
    std::vector<std::int32_t> xs{0,w},ys{0,h};
    for(const auto& r:rects) {
        xs.push_back(std::clamp(r.x0,0,w));xs.push_back(std::clamp(r.x1,0,w));
        ys.push_back(std::clamp(r.y0,0,h));ys.push_back(std::clamp(r.y1,0,h));
    }
    std::sort(xs.begin(),xs.end());xs.erase(std::unique(xs.begin(),xs.end()),xs.end());
    std::sort(ys.begin(),ys.end());ys.erase(std::unique(ys.begin(),ys.end()),ys.end());
    for(std::size_t j=0;j+1<ys.size();++j)
        for(std::size_t i=0;i+1<xs.size();++i) {
            const std::int32_t cx=xs[i],cy=ys[j]; // the cell [xs[i],xs[i+1]) x [ys[j],ys[j+1]) lies in r iff its corner does
            const bool in=std::any_of(rects.begin(),rects.end(),[&](const edram::Rect& r){
                return r.x0<=cx && cx<r.x1 && r.y0<=cy && cy<r.y1;});
            if(!in)return false;
        }
    return true;
}
}
