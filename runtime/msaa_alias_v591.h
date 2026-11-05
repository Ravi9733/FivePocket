#pragma once
// Attempt v591 (fold-fixer2, F5 black marina water): EDRAM aliasing of a 4x MSAA surface by a 1x surface at the same base.
// The 360 keeps every render target in one EDRAM of 80x16-sample tiles. A 4x MSAA surface of pitch P holds 40x8 pixels per
// tile, each pixel's four samples as a 2x2 block of the tile's sample grid; a 1x surface of pitch 2P at the same base reads
// that same grid as 80x16 pixels, so 1x pixel (2x+i, 2y+j) is sample (i,j) of 4x pixel (x,y).
// GTA V uses this for the water: watertex#19p draws the water pass's depth at 320x180 with 4x MSAA (RB_SURFACE_INFO
// 05020140) into EDRAM 1808, and the half-res water pass (640x360, 1x, surface 0a000280) binds 1808 at pitch 640 as its
// depth (census f5-marina-black-1 census-07, frame 37444, draws 2139/2140).
// This renderer keeps one SINGLE-SAMPLE image per (base, format, pitch, depth) (all images VK_SAMPLE_COUNT_1_BIT): the 4x
// surface's four samples were never kept, only one value per pixel. The best available reading of the 1x surface is
// therefore that value replicated over its 2x2 block. Exact when the four samples are equal (a full-coverage draw whose
// pixel shader writes one depth or colour per pixel, as watertex#19p's full-screen rectangle with ALWAYS depth); at a
// partially covered edge pixel the console's samples differ and this is an approximation (not represented here).
// Pure functions; host test: local-analysis/attempts/v591/tests/msaa_alias_test.cpp.
#include <algorithm>
#include <cstdint>
#include <vector>

namespace recomp::gpu::generic::msaa_alias {
// RB_SURFACE_INFO msaa_samples (bits 16-17): 0 1x, 1 2x, 2 4x.
constexpr unsigned Samples1x=0,Samples2x=1,Samples4x=2;

// Whether a draw with RB_DEPTHCONTROL `dc` and RB_STENCILREFMASK front/back `front`/`back` (write mask bits 16-23) can change
// its depth/stencil attachment: depth test and write enabled, or stencil enabled with a non-zero write mask and an operation
// other than KEEP (front; back too when backface stencil is enabled). Binding is not writing: GTA V's im#22v passes bind the
// main depth at 4x (pitch 640 over EDRAM 0) with stencil on and every operation KEEP (census f5-marina-v591, draws 1997/2000:
// dc 00700275, sref 00ff0703): they prime the hierarchical stencil and leave the EDRAM unchanged.
inline bool depthStencilWrites(std::uint32_t dc,std::uint32_t front,std::uint32_t back) noexcept {
#if defined(RECOMP_V592_MUTANT_BIND_IS_WRITE) // host test mutant: v591's rule (any pass that binds the target counts)
    (void)front;(void)back;return (dc&7)!=0;
#endif
    if((dc&6)==6)return true;
    if(!(dc&1))return false;
    const auto ops=[](std::uint32_t d,unsigned shift){return ((d>>shift)&7)|((d>>(shift+3))&7)|((d>>(shift+6))&7);};
    if(((front>>16)&0xFF) && ops(dc,11))return true;
    return (dc&0x80) && ((back>>16)&0xFF) && ops(dc,23);
}
enum class Verdict {Alias,NotThisPair,SourceOlder,Unsupported2x};
// Whether the 1x surface (destination) must take the 4x surface's (source) content before its next use: the source is a 4x
// surface of half the destination's pitch at the same base, format and kind, whose last real write (a draw or clear that
// changed it while it was 4x: sourceWritten) is later than the destination's last pass or write.
inline Verdict decide(unsigned sourceSamples,unsigned sourcePitch,std::uint64_t sourceWritten,
                      unsigned destinationSamples,unsigned destinationPitch,std::uint64_t destinationWritten) noexcept {
    if(destinationSamples!=Samples1x || !sourcePitch || destinationPitch!=2*sourcePitch)return Verdict::NotThisPair;
    if(sourceSamples==Samples2x)return Verdict::Unsupported2x; // (2x: pitch equal, 1x2 sample blocks: other key, not modelled)
    if(sourceSamples!=Samples4x)return Verdict::NotThisPair;
    if(sourceWritten<=destinationWritten)return Verdict::SourceOlder;
    return Verdict::Alias;
}
// The destination area the source covers (its 2x2 blocks), clipped to the destination image.
struct Extent {unsigned width=0,height=0;};
inline Extent covered(unsigned sourceWidth,unsigned sourceHeight,unsigned destinationWidth,unsigned destinationHeight) noexcept {
    return {std::min(destinationWidth,2*sourceWidth),std::min(destinationHeight,2*sourceHeight)};
}
// Per destination texel (row-major over `e`), the source texel index (row-major, source image width `sourceWidth`) whose
// single stored value stands for the sample the console holds there. The table feeds generic_layout_remap.comp (v586).
inline std::vector<std::uint32_t> sourceTable(const Extent& e,unsigned sourceWidth) {
    std::vector<std::uint32_t> t(std::size_t(e.width)*e.height);
    for(unsigned y=0;y<e.height;++y)
        for(unsigned x=0;x<e.width;++x)
#if defined(RECOMP_V591_MUTANT_SAME_COORDS) // host test mutant: the 1x pixel read at its own coordinates in the 4x image
            t[std::size_t(y)*e.width+x]=std::min(y,e.height/2-1)*sourceWidth+std::min(x,sourceWidth-1);
#else
            t[std::size_t(y)*e.width+x]=(y/2)*sourceWidth+x/2;
#endif
    return t;
}
} // namespace recomp::gpu::generic::msaa_alias
