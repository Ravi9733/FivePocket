#pragma once
// Attempt v56: D3D DrawVertices (original 823CE5A0: device r3, primitive r4, start vertex r5, vertex
// count r6), the non-indexed draw with bound streams. The game reaches it through rage's
// DrawPrimitive wrapper 822AE400 (primitive table index, start, count) and 8235E5D8. Its PM4 is
// VGT_INDX_OFFSET (type-0 0x2102) = start, then DRAW_INDX (0xC0012201) with the initiator
// count<<16 | primitive | 0x80 (auto index). No hook covered it before v56, so the generic renderer
// never drew these draws (v52/v55 census: the 14 shadow maps, the G-buffer's fourth target and the
// SSAO result were resolved and never sampled).
#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace recomp::gpu {

struct DrawVerticesChunk {std::uint32_t start=0,count=0;};

// The split 823CE5A0 performs when count > 65535. For each primitive type the table at 82006610 holds
// two words: w0 (vertices per primitive) and w1 (vertices a following chunk repeats). The first chunk
// is w0 * even(65535 / w0) vertices; each later chunk starts w1 vertices before the previous chunk's
// end and holds the rest plus those w1 vertices (the loop at 823CE984 recomputes the chunk for it).
// Without the table (unreadable guest bytes) or a zero w0 (the original traps) the draw is one chunk.
inline std::vector<DrawVerticesChunk> planDrawVertices(std::uint32_t start,std::uint32_t count,
        const std::optional<std::array<std::uint32_t,2>>& table) {
    std::vector<DrawVerticesChunk> out;
    if(!count)return out;
    if(count<=65535 || !table || !(*table)[0]){out.push_back({start,count});return out;}
    const std::uint32_t perPrimitive=(*table)[0],repeat=(*table)[1];
    std::uint32_t chunk=(65535/perPrimitive)&~1u;chunk*=perPrimitive;
    if(!chunk || repeat>=chunk){out.push_back({start,count});return out;}
    while(true) {
        const std::uint32_t n=count<=65535?count:chunk;
        out.push_back({start,n});
        if(n==count || out.size()>4096)break;
        const std::uint32_t remaining=count-n;
        start+=n-repeat;count=remaining+repeat;
    }
    return out;
}

// One chunk as a generic draw: the auto-generated indices 0..count-1 with VGT_INDX_OFFSET (the chunk's
// start) as the base vertex, which the renderer adds before the VGT min/max clamp, as for DMA indices.
// (The renderer's non-indexed path draws vertices from 0, so a start vertex needs this indexed form.)
template<class Indices> // v214: any vector of uint32 (generic::IndexVector); every element is written after resize()
inline void drawVerticesIndices(const DrawVerticesChunk& chunk,Indices& indices,std::uint32_t& baseVertex) {
    indices.resize(chunk.count);
    for(std::uint32_t i=0;i<chunk.count;++i)indices[i]=i;
    baseVertex=chunk.start;
}

// Per-frame DrawVertices accounting (the counter line). `routed` counts generic draws recorded,
// `refused` the renderer's refusals, `skipped` draws not routed (switch, not owned, no shader code).
struct DrawVerticesFrame {
    unsigned calls=0,chunks=0,routed=0,refused=0,skipped=0,vertices=0;
    std::array<unsigned,16> primitives{};  // by Xenos primitive type (15: other)
    std::string samples;                   // the first few calls: primitive, count, vs/ps objects, pixel textures
    unsigned sampled=0;
    std::string lastWhy;
    std::array<std::array<std::uint32_t,2>,4> callers{}; // guest task token, calls (the first four tokens)
    unsigned otherCallers=0;
    void call(unsigned primitive,unsigned count,std::uint32_t caller) {
        ++calls;vertices+=count;++primitives[primitive<15?primitive:15];
        for(auto& c:callers)if(c[1] && c[0]==caller){++c[1];return;}
        for(auto& c:callers)if(!c[1]){c={caller,1};return;}
        ++otherCallers;
    }
    void sample(unsigned primitive,unsigned count,std::uint32_t vs,std::uint32_t ps,const std::vector<std::uint32_t>& textures,const char* why) {
        if(sampled>=4)return;
        ++sampled;char b[96];std::snprintf(b,sizeof b," [p%u n%u vs%08x ps%08x %s tex",primitive,count,vs,ps,why?why:"?");samples+=b;
        for(std::size_t i=0;i<textures.size() && i<8;++i){std::snprintf(b,sizeof b,"%s%08x",i?",":"=",textures[i]);samples+=b;}
        samples+="]";
    }
    std::string line(unsigned long long frame,bool routing) const {
        std::string p;char b[32];
        for(unsigned i=0;i<16;++i)if(primitives[i]){std::snprintf(b,sizeof b,"%s%u:%u",p.empty()?"":",",i,primitives[i]);p+=b;}
        std::string who;
        for(const auto& c:callers)if(c[1]){std::snprintf(b,sizeof b,"%s%08x:%u",who.empty()?"":",",c[0],c[1]);who+=b;}
        if(otherCallers){std::snprintf(b,sizeof b,",other:%u",otherCallers);who+=b;}
        char head[320];
        std::snprintf(head,sizeof head,"RENDER_DIAG_DRAW_VERTICES frame=%llu routing=%u calls=%u chunks=%u routed=%u refused=%u skipped=%u vertices=%u prims=%s callers=%s last_why=%s",
            frame,unsigned(routing),calls,chunks,routed,refused,skipped,vertices,p.empty()?"-":p.c_str(),who.empty()?"-":who.c_str(),
            lastWhy.empty()?"-":lastWhy.c_str());
        return head+samples;
    }
};

} // namespace recomp::gpu
