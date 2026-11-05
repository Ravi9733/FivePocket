#pragma once
// Attempt v343 (exact): coalesce the rectangles of one vkCmdClearAttachments call (they all clear to one value) into fewer
// rectangles with exactly the same union: rectangles in one row band (same y, height) that touch or overlap in x merge into one,
// then rectangles in one column band (same x, width) that touch or overlap in y merge, repeated until nothing changes.
// Every step replaces two rectangles by their union, which is itself a rectangle, so the covered pixel set never changes.
#include <algorithm>
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>
namespace v343 {
struct Stats {std::uint64_t calls=0,in=0,out=0;};
inline bool mergePass(std::vector<VkClearRect>& rs,bool rows) {
    const auto lo=[rows](const VkClearRect& r){return rows?r.rect.offset.x:r.rect.offset.y;};
    const auto hi=[rows](const VkClearRect& r){return rows?r.rect.offset.x+std::int32_t(r.rect.extent.width):r.rect.offset.y+std::int32_t(r.rect.extent.height);};
    const auto band0=[rows](const VkClearRect& r){return rows?r.rect.offset.y:r.rect.offset.x;};
    const auto band1=[rows](const VkClearRect& r){return rows?r.rect.extent.height:r.rect.extent.width;};
    std::sort(rs.begin(),rs.end(),[&](const VkClearRect& a,const VkClearRect& b){
        if(band0(a)!=band0(b))return band0(a)<band0(b);
        if(band1(a)!=band1(b))return band1(a)<band1(b);
        return lo(a)<lo(b);});
    std::vector<VkClearRect> out;out.reserve(rs.size());bool changed=false;
    for(const auto& r:rs) {
#if defined(RECOMP_V343_MUTANT_GAP) // test mutant: rectangles one pixel apart merge too (the union grows)
        const std::int32_t slack=1;
#else
        const std::int32_t slack=0;
#endif
        if(!out.empty() && band0(out.back())==band0(r) && band1(out.back())==band1(r) && lo(r)<=hi(out.back())+slack &&
           out.back().baseArrayLayer==r.baseArrayLayer && out.back().layerCount==r.layerCount) {
            auto& m=out.back();const std::int32_t h=std::max(hi(m),hi(r));
            if(rows)m.rect.extent.width=std::uint32_t(h-m.rect.offset.x);else m.rect.extent.height=std::uint32_t(h-m.rect.offset.y);
            changed=true;continue;
        }
        out.push_back(r);
    }
    rs.swap(out);return changed;
}
inline void coalesce(std::vector<VkClearRect>& rs,Stats& st) {
    ++st.calls;st.in+=rs.size();
    for(unsigned k=0;k<8;++k){const bool a=mergePass(rs,true),b=mergePass(rs,false);if(!a && !b)break;}
    st.out+=rs.size();
}
}
