// Attempt v586 (fold-fixer, 2026-10-22, F1 "black spikes over the sea"): a texture fetch that reads a resolve destination
// with ANOTHER memory layout than the resolve wrote.
// The Xenos resolve always writes its destination in the 2D tiled layout (RB_COPY_DEST_INFO has no linear mode; the
// texel slots are tex::tiled2D's). A fetch constant may describe the same memory as linear (fetch word 0 bit 31 clear)
// or with another pitch: it then reads the bytes the resolve wrote at ITS OWN slots. GTA V relies on this in the water
// simulation: watertex#22p (shader-translation-v41full-batch-03/shader-0712) computes, for its own pixel (X, Y), the
// tiled element index tiled2D(X, Y, 128, 4 B) / 4 (checked equal for all 16384 pixels: v586/tests/layout_remap_test) and
// samples the height and velocity maps at that index's LINEAR position; the tiled resolve of its output then leaves the
// 128x128 k_32_FLOAT height map 0bfa3000 in linear order in memory, and every reader (watertex#21p, water#1v's vertex
// texture) fetches it as linear. Up to v585 the renderer handed any fetch of a resolved address the resolved image itself
// (texel (x, y) = render target pixel (x, y)), i.e. the tiled reading: the height field was permuted every frame, the wave
// simulation diverged (census f-spikes-1: velocity 2051, heights -80..41 m) and the ocean mesh stood up in spikes.
// Here: for a fetch whose layout differs from the resolve's, the resolved texel each fetch texel reads (pure functions; the
// renderer gathers them into an image of the fetch's shape after every resolve into that address: generic_layout_remap.comp).
#pragma once
#include "resolved_vertex_fetch.h"
#include <cstdint>
#include <vector>

namespace recomp::gpu::generic::rlr {

// The layout a resolve leaves in memory: the hardware's tiled 2D layout with the destination's pitch, whatever the
// destination fetch's own tiled bit says.
inline rvf::Layout resolveMemoryLayout(unsigned pitchTexels,unsigned height,unsigned log2Bytes) {
#if defined(RECOMP_V586_MUTANT_DESTINATION_BIT) // test mutant: the resolve's layout taken as linear (the destination fetch's own bit, linear here)
    return rvf::Layout{false,pitchTexels,height,log2Bytes};
#else
    return rvf::Layout{true,pitchTexels,height,log2Bytes};
#endif
}
// The layout a fetch reads (its tiled bit, pitch, height, bytes per texel of its format).
inline rvf::Layout fetchLayout(bool tiled,unsigned pitchTexels,unsigned height,unsigned log2Bytes) {
    return rvf::Layout{tiled,pitchTexels,height,log2Bytes};
}
// Whether a fetch reads the resolved texels one for one (then the resolved image itself is what it samples).
// Two tiled layouts with the same pitch, bytes per texel and covering height place every texel alike.
inline bool sameTexels(const rvf::Layout& resolve,const rvf::Layout& fetch) {
    return resolve.tiled==fetch.tiled && resolve.pitch==fetch.pitch && resolve.log2Bytes==fetch.log2Bytes;
}
// The byte offset of fetch texel (u, v) in its layout.
inline std::uint64_t fetchByte(const rvf::Layout& f,unsigned u,unsigned v) {
    return f.tiled?tex::tiled2D(u,v,f.pitch,f.log2Bytes):(std::uint64_t{v}*f.pitch+u)<<f.log2Bytes;
}
// For every fetch texel (u < width, v < height), row-major, the resolved image texel at the same memory slot as
// sy * resolveWidth + sx (the resolve's texel table: slots of its footprint that hold one of its texels), or ~0u where the
// resolve wrote none (padding, beyond its footprint: the remap image holds zeros there). Requires equal bytes per texel (a
// fetch of another texel size reinterprets bytes: not represented, ok = false, the caller keeps v585's path).
struct Plan {std::vector<std::uint32_t> sources;std::uint64_t unwritten=0;bool ok=false;};
inline Plan plan(const rvf::Layout& resolve,unsigned resolveWidth,const rvf::Layout& fetch,unsigned width,unsigned height) {
    Plan p;
    if(resolve.log2Bytes!=fetch.log2Bytes || !resolve.pitch || !fetch.pitch || !width || !height || width>fetch.pitch || !resolveWidth)return p;
    const auto table=rvf::texelTable(resolve,resolveWidth);
    p.sources.resize(std::size_t(width)*height,~0u);
    for(unsigned v=0;v<height;++v)for(unsigned u=0;u<width;++u) {
        const std::uint64_t slot=fetchByte(fetch,u,v)>>fetch.log2Bytes;
        const std::uint32_t t=slot<table.size()?table[std::size_t(slot)]:~0u;
        if(t==~0u){++p.unwritten;continue;}
        p.sources[std::size_t(v)*width+u]=(t>>16)*resolveWidth+(t&0xFFFFu);
    }
    p.ok=true;
    return p;
}
}
