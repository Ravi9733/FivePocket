#pragma once
// Attempt v75: the guest-memory image of a colour resolve (what the 360's resolve writes to the
// destination texture's memory), for resolves whose result the CPU reads back. Pure CPU: no Vulkan,
// no guest memory access (the caller passes the destination's current bytes and publishes the edit).
//
// Why (v75 investigation; evidence in the v75 report and tests/): GTA V's CPU reads the GPU exposure
// back from guest memory. sub_8238CE00 takes one of three pointers at 0x83954EC0 (triple buffered by
// the frame counter), loads the 1x1 k_16_16_16_16 texel with lvx + vupkd3d128 FLOAT16_4 (x,y,z,w =
// the texel's four big-endian halves) and stores y (the adapted exposure in stops) into the
// double-buffered 0x839B77F8; sub_82453808 turns it into e = s * 2^-y, and sub_82300CE8 /
// sub_82300D68 set globalScalars3 = (1/e, e, 1/e, e): every HDR-writing shader multiplies its output
// by .z (the pre-exposure into the 7e3 light buffer) and every HDR reader (postfx, water, glass)
// by .w. The generic renderer kept resolves in Vulkan images only, so the CPU read stale bytes.
//
// Layout rules come from generic_texture_decode.h (the same plan the texture decoder uses): texel
// (x, y) of level 0 at tex::blockAddress, bytes endian-swapped by the fetch's endian field (memory
// byte k ^ swap holds little-endian texel byte k, the decoder's rule read backwards).
#include "generic_texture_decode.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

namespace recomp::gpu::generic::wb {

// The resolved image's texel layout (the renderer maps its VkFormat to one of these).
enum class Source : std::uint8_t {None,R32F,RG32F,RGBA32F,R16F,RG16F,RGBA16F,RGBA8};
inline unsigned sourceBytes(Source s) {
    switch(s){case Source::R32F:return 4;case Source::RG32F:return 8;case Source::RGBA32F:return 16;case Source::R16F:return 2;
    case Source::RG16F:return 4;case Source::RGBA16F:return 8;case Source::RGBA8:return 4;default:return 0;}
}
inline unsigned sourceComponents(Source s) {
    switch(s){case Source::R32F:case Source::R16F:return 1;case Source::RG32F:case Source::RG16F:return 2;
    case Source::None:return 0;default:return 4;}
}
// Destination formats written back: float formats (bit-exact from a same-width source, converted
// between half and float otherwise) and k_8_8_8_8 from an 8888 source.
struct Destination {unsigned components=0,bits=0;bool unorm8=false;};
inline Destination destinationOf(unsigned format) {
    switch(format) {
    case 36: return {1,32};             // k_32_FLOAT
    case 37: return {2,32};             // k_32_32_FLOAT
    case 38: return {4,32};             // k_32_32_32_32_FLOAT
    case 27: case 30: return {1,16};    // k_16_EXPAND, k_16_FLOAT
    case 28: case 31: return {2,16};    // k_16_16_EXPAND, k_16_16_FLOAT
    case 29: case 32: return {4,16};    // k_16_16_16_16_EXPAND, k_16_16_16_16_FLOAT
    case 6: return {4,8,true};          // k_8_8_8_8
    case 2: return {1,8,true};          // k_8 (v668: from an 8888 source, the component its view samples)
    default: return {};
    }
}

// v613 (swim-fixer, 2026-10-22): which resolves are written back. v75 capped every destination at 4096 texels (a cost limit;
// the case was the 1x1 exposure texel). The 360's resolve writes every destination's memory; the CPU reads two larger
// GPU products back: the water simulation's 128x128 k_32_FLOAT maps (Thor census: velocity 0bf94000 resolved 4x a frame,
// height 0bfa4000 once). Water::Init (sub_82936D08) allocates them as one 128 KB block: velocity = block+0 (the CPU reads
// it with XGAddress2DTiledOffset sub_8257CA68 in sub_828F1FF8; Water::Update sub_824894D8 -> sub_823C4560 blends queued
// disturbances into it), height = block+0x10000 (read linearly: watertex#22p untiles in its shader, so the tiled resolve
// leaves natural order). CBuoyancy (sub_824D00A8, called for peds from sub_82434390) samples height and water velocity
// through sub_828F2258 and drags bodies toward the water's vertical velocity. Without the writeback the CPU saw a flat sea
// (height 0) and a velocity map that only accumulated disturbances and never decayed (Michael held under after a dive).
// Single-channel 32-bit float destinations (data maps, never display images) up to 128x128 are therefore written back too;
// switch diag-generic-no-data-writeback restores v75's single cap.
//
// v668 (map-fow, 2026-10-24): the pause map's fog-of-war mask is a 128x128 k_8 texture the CPU owns. CMiniMap's init
// (sub_82594258) creates it, keeps its bits pointer at 0x838959F4 and clears them (memset 16384); the GPU's exploration
// update resolves into it; GET_MINIMAP_FOW_COORDINATE_IS_REVEALED (native e010f081's neighbour 65b705f6 -> sub_8253C648)
// reads a texel through XGAddress2DTiledOffset (sub_8257CA68, 1 byte per texel; revealed = byte > 128); and the
// savegame's "MiniMap" block (sub_82955A38/AB0/B50/DA0, beside "Global", "Garages", "PlayerPed") serializes those 16384
// bytes. Without the writeback a save stored the zeros of the init and a load's bytes never reached the GPU: every
// session started with an unexplored island (only the blips' own reveals and this session's drive showed land).
// k_8 destinations up to 128x128 are therefore written back too (MaskFormat).
inline constexpr unsigned ImageTexels=4096,DataTexels=16384,DataFormat=36,MaskFormat=2; // k_32_FLOAT, k_8
inline bool sizeEligible(unsigned format,std::uint64_t texels,bool dataMaps) noexcept {
#if defined(RECOMP_V613_MUTANT_IMAGE_CAP_ONLY) // test mutant: v75's single cap
    (void)format;(void)dataMaps;return texels<=ImageTexels;
#elif defined(RECOMP_V668_MUTANT_NO_MASK) // test mutant: v613's rule (k_8 masks keep v75's cap)
    return texels<=ImageTexels || (dataMaps && format==DataFormat && texels<=DataTexels);
#else
    return texels<=ImageTexels || (dataMaps && (format==DataFormat || format==MaskFormat) && texels<=DataTexels);
#endif
}
// v668: destinations whose guest memory the CPU writes between resolves, the GPU then reading the CPU's bytes (the fog
// mask: a savegame load writes the saved exploration into it). For these the renderer keeps the hash of what it published
// and, when the guest bytes no longer match at the next fetch, drops the resolved image so the fetch decodes guest memory
// (the 360 has only the memory). Not the water maps: Water::Update's CPU disturbances would drop the velocity image every
// frame (a separate fidelity question, unmeasured).
inline bool cpuWritten(unsigned format) noexcept {return format==MaskFormat;}
// v668: FNV-1a over a publication's bytes (change detection of the CPU's writes, not a proof).
inline std::uint64_t publicationHash(const std::uint8_t* p,std::size_t n) noexcept {
    std::uint64_t h=1469598103934665603ull;for(std::size_t i=0;i<n;++i){h^=p[i];h*=1099511628211ull;}return h;
}
// v613: a slot's queued writebacks in recording order; entry i is superseded when a later entry of the same destination,
// layout, rectangle and source follows (its texels then replace i's entirely, as present() publishes oldest first). The
// velocity map is resolved four times a frame: only the last one is taken out of the slot buffer and published.
template<class W> inline std::vector<bool> superseded(const std::vector<W>& list) {
    std::vector<bool> out(list.size(),false);
    for(std::size_t i=0;i<list.size();++i)for(std::size_t j=i+1;j<list.size() && !out[i];++j) {
        const auto& a=list[i];const auto& b=list[j];
#if !defined(RECOMP_V613_MUTANT_KEEP_FIRST)
        out[i]=a.physical==b.physical && a.fetch==b.fetch && a.source==b.source && a.swap==b.swap && a.x==b.x && a.y==b.y &&
               a.w==b.w && a.h==b.h;
#else // test mutant: the later entry is dropped instead of the earlier one (publishes a stale resolve)
        if(a.physical==b.physical && a.fetch==b.fetch && a.source==b.source && a.swap==b.swap && a.x==b.x && a.y==b.y &&
           a.w==b.w && a.h==b.h)out[j]=true;
#endif
    }
    return out;
}

enum class Refusal : std::uint8_t {None,Plan,Format,Source,Rect,Span};
inline const char* refusalName(Refusal r) {
    switch(r){case Refusal::Plan:return "plan";case Refusal::Format:return "format";case Refusal::Source:return "source";
    case Refusal::Rect:return "rect";case Refusal::Span:return "span";default:return "none";}
}

// Bytes [0, span) from the destination's base address hold level 0 (rounded up to 4 bytes).
struct Layout {tex::Plan plan;Destination dst;std::uint32_t span=0;};
inline std::optional<Layout> layout(const std::array<std::uint32_t,6>& fetch,Refusal& why) {
    why=Refusal::None;
    tex::Refusal r;auto p=tex::plan(fetch,r);
    if(!p || p->levels.empty() || p->levels[0].absent || p->dim!=tex::Dim::TwoD || p->layers!=1){why=Refusal::Plan;return std::nullopt;}
    const auto dst=destinationOf(p->desc.format);
    if(!dst.components || p->format.blockW!=1 || p->format.blockBytes*8u!=dst.components*dst.bits){why=Refusal::Format;return std::nullopt;}
    const auto end=tex::levelEnd(*p,p->levels[0]);
    if(!end || end>(1u<<20)){why=Refusal::Span;return std::nullopt;}
    Layout l;l.plan=*p;l.dst=dst;l.span=std::uint32_t((end+3)&~std::uint64_t{3});
    return l;
}

inline float halfBitsToFloat(std::uint16_t h){return tex::fromHalf(h);}
// One source component as the destination's component bits (little-endian integer).
inline std::uint32_t component(Source s,const std::uint8_t* texel,unsigned c,const Destination& d) {
    if(s==Source::RGBA8)return texel[c]; // only to k_8_8_8_8 (checked by the caller)
    const bool half=s==Source::R16F || s==Source::RG16F || s==Source::RGBA16F;
    if(half) {
        std::uint16_t h;std::memcpy(&h,texel+2*c,2);
        if(d.bits==16)return h;
        const float f=halfBitsToFloat(h);std::uint32_t u;std::memcpy(&u,&f,4);return u;
    }
    std::uint32_t u;std::memcpy(&u,texel+4*c,4);
    if(d.bits==32)return u;
    float f;std::memcpy(&f,&u,4);return tex::toHalf(f);
}

// v613: the byte offset (tex::blockAddress) of every level-0 texel, row-major, and their byte range, memoised per layout
// shape on the calling thread (publications run on the render thread; the water maps are the same two 128x128 shapes every
// frame: per-texel tiled address arithmetic was most of their ~0.2 ms host cost). Only for 2D levels of 1x1 blocks and at
// most 65536 texels; nullptr otherwise (the callers then take the per-texel path).
struct OffsetTable {bool tiled=false;unsigned bytes=0,pitch=0,rows=0,ox=0,oy=0,w=0,h=0;std::uint64_t offset=0,lo=0,hi=0;
                    std::vector<std::uint32_t> at;};
inline const OffsetTable* texelOffsets(const Layout& l) {
    const auto& p=l.plan;const auto& v=p.levels[0];const auto& s=v.storage;
    if(p.dim!=tex::Dim::TwoD || p.format.blockW!=1 || p.format.blockH!=1 || std::uint64_t{v.width}*v.height>65536u)return nullptr;
    thread_local std::array<OffsetTable,4> cache{};thread_local unsigned next=0;
    for(const auto& e:cache)
        if(!e.at.empty() && e.tiled==p.desc.tiled && e.bytes==p.format.blockBytes && e.pitch==s.rowPitchBlocks && e.rows==s.sliceRows &&
           e.offset==s.offset && e.ox==v.originX && e.oy==v.originY && e.w==v.width && e.h==v.height)return &e;
    auto& e=cache[next++%cache.size()];
    e.tiled=p.desc.tiled;e.bytes=p.format.blockBytes;e.pitch=s.rowPitchBlocks;e.rows=s.sliceRows;e.offset=s.offset;
    e.ox=v.originX;e.oy=v.originY;e.w=v.width;e.h=v.height;e.at.assign(std::size_t{v.width}*v.height,0);
    e.lo=~std::uint64_t{0};e.hi=0;
    for(unsigned y=0;y<v.height;++y)for(unsigned x=0;x<v.width;++x) {
        const auto a=tex::blockAddress(p,s,0,v.originX+x,v.originY+y,0);
        if(a>0xFFFFFFFFu){e.at.clear();return nullptr;}
        e.at[std::size_t{y}*v.width+x]=std::uint32_t(a);e.lo=std::min(e.lo,a);e.hi=std::max<std::uint64_t>(e.hi,a+p.format.blockBytes);
    }
    return &e;
}

// The 4-byte-aligned byte range [first, second) of the destination the rectangle's texels occupy (the
// endian swap stays inside each aligned word): what a publication has to cover.
inline std::pair<std::uint32_t,std::uint32_t> touched(const Layout& l,unsigned x0,unsigned y0,unsigned w,unsigned h) {
    const auto& p=l.plan;const auto& level=p.levels[0];
    if(x0==0 && y0==0 && w==level.width && h==level.height && w && h) // v613: the whole level (memoised range)
        if(const auto* t=texelOffsets(l)) {
            const std::uint64_t lo=t->lo&~std::uint64_t{3},hi=std::min<std::uint64_t>((t->hi+3)&~std::uint64_t{3},l.span);
            if(hi<=lo)return {0,0};
            return {std::uint32_t(lo),std::uint32_t(hi)};
        }
    std::uint64_t lo=~std::uint64_t{0},hi=0;
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
        const auto at=tex::blockAddress(p,level.storage,0,level.originX+x0+x,level.originY+y0+y,0);
        lo=std::min(lo,at);hi=std::max<std::uint64_t>(hi,at+p.format.blockBytes);
    }
    if(hi<=lo)return {0,0};
    lo&=~std::uint64_t{3};hi=std::min<std::uint64_t>((hi+3)&~std::uint64_t{3},l.span);
    return {std::uint32_t(lo),std::uint32_t(hi)};
}

// Writes the resolved rectangle (texels at `x0,y0`, `w` x `h`, `rowBytes` apart, in `source`
// layout) into `guest` (the destination's bytes [0, span)). `swapRB` exchanges components 0 and 2
// (RB_COPY_DEST_INFO.copy_dest_swap: 823C2970..823C2AB8 sets it when the destination's red selector
// is not X; the generic renderer keeps EDRAM order in its image and swaps in the view instead).
// Returns the number of texels written, or nullopt (nothing written) on a refusal.
// v278: writeReference is v75's per-byte form (the test reference; diag-v278-baseline); write() below takes a direct path.
inline std::optional<unsigned> writeReference(const Layout& l,Source source,const std::uint8_t* texels,std::size_t rowBytes,
                                     unsigned x0,unsigned y0,unsigned w,unsigned h,bool swapRB,std::vector<std::uint8_t>& guest,Refusal& why) {
    why=Refusal::None;
    const auto& p=l.plan;const auto& d=l.dst;const auto& level=p.levels[0];
    const unsigned stride=sourceBytes(source);
    // v668: a single-component k_8 destination from an 8888 source takes the component the destination's view samples
    // (copyColor: view[0] = 2 with copy_dest_swap, else 0; the renderer keeps EDRAM order in the image).
    const bool pick=source==Source::RGBA8 && d.components==1 && d.unorm8;
    const unsigned n=pick?1u:sourceComponents(source);
    if(!n || n!=d.components || (d.unorm8!=(source==Source::RGBA8))){why=Refusal::Source;return std::nullopt;}
    if(!w || !h || x0+w>level.width || y0+h>level.height || rowBytes<std::size_t{w}*stride){why=Refusal::Rect;return std::nullopt;}
    if(guest.size()<l.span){why=Refusal::Span;return std::nullopt;}
    constexpr unsigned EndianXor[]{0,1,3,2};const unsigned x4=EndianXor[p.desc.endian&3];
    const unsigned texelBytes=p.format.blockBytes,componentBytes=d.bits/8;
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
        const std::uint8_t* t=texels+std::size_t{y}*rowBytes+std::size_t{x}*stride;
        std::array<std::uint8_t,16> le{};
        for(unsigned c=0;c<n;++c) {
#if defined(RECOMP_V668_MUTANT_PICK_ALPHA) // test mutant: the k_8 byte taken from the source's alpha
            const unsigned from=pick?3u:swapRB && n==4 && (c==0 || c==2)?2-c:c;
#else
            const unsigned from=pick?(swapRB?2u:0u):swapRB && n==4 && (c==0 || c==2)?2-c:c;
#endif
            const auto v=component(source,t,from,d);
            for(unsigned b=0;b<componentBytes;++b)le[c*componentBytes+b]=std::uint8_t(v>>(8*b));
        }
        const auto at=tex::blockAddress(p,level.storage,0,level.originX+x0+x,level.originY+y0+y,0);
        for(unsigned k=0;k<texelBytes;++k) {
            const auto o=(at+k)^x4;
            if(o>=guest.size()){why=Refusal::Span;return std::nullopt;}
            guest[o]=le[k];
        }
    }
    return w*h;
}
// v278: the same result as writeReference (v275 outdoor profile: its per-byte loop, tex::blockAddress and component() were
// ~2.1% of the render thread for ~9 resolves / 170 KB per frame). When no conversion applies (each source component has the
// destination's width: RGBA8 -> k_8_8_8_8, 16-bit float -> 16-bit, 32-bit float -> 32-bit) and a texel is a whole number
// of 4-byte words, texel k's destination bytes are a fixed permutation of its source bytes: guest[at + j] = le[j ^ x4]
// (the endian swap stays inside the texel's aligned words) and le[c*cb + b] = source byte from(c)*cb + b (swapRB exchanging
// components 0 and 2). The permutation is built once per call; a texel whose address is not 4-byte aligned, and every
// other format pair, takes the reference loop (same order, same refusals: a texel out of the span refuses the whole write).
inline std::atomic<bool> referenceWrite{false}; // diag-v278-baseline
inline std::optional<unsigned> write(const Layout& l,Source source,const std::uint8_t* texels,std::size_t rowBytes,
                                     unsigned x0,unsigned y0,unsigned w,unsigned h,bool swapRB,std::vector<std::uint8_t>& guest,Refusal& why) {
    const auto& p=l.plan;const auto& d=l.dst;const auto& level=p.levels[0];
    const unsigned n=sourceComponents(source),stride=sourceBytes(source);
    const bool half=source==Source::R16F || source==Source::RG16F || source==Source::RGBA16F;
    const unsigned srcComponentBytes=source==Source::RGBA8?1u:half?2u:4u;
    const unsigned texelBytes=p.format.blockBytes,componentBytes=d.bits/8;
    if(!referenceWrite.load(std::memory_order_relaxed) && source==Source::RGBA8 && d.components==1 && d.unorm8 && texelBytes==1 &&
       x0==0 && y0==0 && w==level.width && h==level.height && rowBytes==std::size_t{w}*stride && guest.size()>=l.span)
        if(const auto* t=texelOffsets(l)) { // v668: a whole k_8 level (the fog mask) through the memoised offsets
            constexpr unsigned EndianXor[]{0,1,3,2};const unsigned x4=EndianXor[p.desc.endian&3];
            const unsigned c=swapRB?2u:0u;const std::uint32_t* at=t->at.data();const std::size_t count=t->at.size();
            for(std::size_t i=0;i<count;++i)if((at[i]^x4)>=guest.size()){why=Refusal::Span;return std::nullopt;}
            why=Refusal::None;
            for(std::size_t i=0;i<count;++i)guest[at[i]^x4]=texels[i*4+c];
            return w*h;
        }
    const bool direct=!referenceWrite.load(std::memory_order_relaxed) && n && n==d.components && d.unorm8==(source==Source::RGBA8) &&
                      srcComponentBytes==componentBytes && texelBytes==stride && texelBytes>=4 && texelBytes%4==0 && texelBytes<=16;
    if(!direct)return writeReference(l,source,texels,rowBytes,x0,y0,w,h,swapRB,guest,why);
    why=Refusal::None;
    if(!w || !h || x0+w>level.width || y0+h>level.height || rowBytes<std::size_t{w}*stride){why=Refusal::Rect;return std::nullopt;}
    if(guest.size()<l.span){why=Refusal::Span;return std::nullopt;}
    constexpr unsigned EndianXor[]{0,1,3,2};const unsigned x4=EndianXor[p.desc.endian&3];
    std::array<std::uint8_t,16> from{};
    for(unsigned j=0;j<texelBytes;++j) {
        const unsigned i=j^x4,c=i/componentBytes,b=i%componentBytes;
#if defined(RECOMP_V278_MUTANT_NO_SWAP) // test mutant: swapRB ignored on the direct path
        const unsigned fc=c;
#else
        const unsigned fc=swapRB && n==4 && (c==0 || c==2)?2-c:c;
#endif
        from[j]=std::uint8_t(fc*componentBytes+b);
    }
    std::uint8_t* out=guest.data();const std::size_t size=guest.size();
    if(x0==0 && y0==0 && w==level.width && h==level.height && !(rowBytes!=std::size_t{w}*stride))
        if(const auto* t=texelOffsets(l)) { // v613: the whole level through the memoised offsets (same bytes as below)
            if(t->hi>size){why=Refusal::Span;return std::nullopt;}
            const std::uint32_t* at=t->at.data();const std::size_t count=t->at.size();bool aligned=true;
            for(std::size_t i=0;i<count;++i)aligned&=!(at[i]&3u);
            if(aligned) {
                if(texelBytes==4 && from[0]==3 && from[1]==2 && from[2]==1 && from[3]==0) { // 32-bit texels, 8in32: one byte swap
                    for(std::size_t i=0;i<count;++i){std::uint32_t v;std::memcpy(&v,texels+i*4,4);v=__builtin_bswap32(v);std::memcpy(out+at[i],&v,4);}
                    return w*h;
                }
                for(std::size_t i=0;i<count;++i) {
                    const std::uint8_t* src=texels+i*stride;std::uint8_t* o=out+at[i];
                    for(unsigned j=0;j<texelBytes;++j)o[j]=src[from[j]];
                }
                return w*h;
            }
        }
    for(unsigned y=0;y<h;++y) {
        const std::uint8_t* row=texels+std::size_t{y}*rowBytes;
        for(unsigned x=0;x<w;++x) {
            const std::uint8_t* t=row+std::size_t{x}*stride;
            const auto at=tex::blockAddress(p,level.storage,0,level.originX+x0+x,level.originY+y0+y,0);
            if(at+texelBytes>size){why=Refusal::Span;return std::nullopt;}
            if(at&3) { // (not seen for these formats; the reference's byte order for this texel)
                std::array<std::uint8_t,16> le{};for(unsigned k=0;k<texelBytes;++k)le[k]=t[from[k^x4]]; // (le[i] = source byte of i)
                for(unsigned k=0;k<texelBytes;++k){const auto o=(at+k)^x4;if(o>=size){why=Refusal::Span;return std::nullopt;}out[o]=le[k];}
                continue;
            }
            std::uint8_t* o=out+at;
            for(unsigned j=0;j<texelBytes;++j)o[j]=t[from[j]];
        }
    }
    return w*h;
}
}
