#pragma once
// fsr-modes (agent lane, on v458): render-resolution scale ("FSR quality modes"). Pure functions only: no Vulkan, no I/O, no
// state. The renderer (generic_renderer.cpp) keeps every piece of its bookkeeping in GUEST pixels (the Xbox 360's 1280x720
// frame, EDRAM rows, pending clear rectangles, resolve rectangles) and converts to HOST pixels at the Vulkan call with the
// functions below, so that all coordinate arithmetic of the feature is in this file and under host tests
// (agents/fsr-modes/tests/render_scale_test.cpp).
//
// The setting: files/settings/render-scale.txt, read once at boot. One decimal integer, the percentage per axis
// (optionally followed by '%'), white space around it ignored. Absent, empty, unreadable, malformed, longer than 63 bytes or outside 50..200 = 100
// (a slip such as 1000 for 100 must not select the largest mode).
// 100 = exactly v458 (the scale is the identity and the renderer takes none of the scaled paths).
// The percentage is snapped to sixteenths (6.25 % steps):
//     50 -> 8/16   640x360        75 -> 12/16  960x540  "Performance"      88 -> 14/16 1120x630 "Balanced"
//    100 -> 16/16 1280x720 (default, original)                 115 -> 18/16 1440x810 "Ultra quality"
//    125 -> 20/16 1600x900       150 -> 24/16 1920x1080 "Native"          200 -> 32/16 2560x1440
// Why sixteenths: the game draws its frame in screen tiles (predicated tiling: EDRAM surfaces one tile apart, window offsets,
// resolves that place each tile into the frame texture). Every tile boundary the v458 logs show is a multiple of 16 guest
// pixels (384, 336, 928, 352), and k/16 maps every multiple of 16 to an integer, so tiles meet without a seam or a sub-pixel
// phase and 1280x720 itself always maps to whole pixels (80k x 45k).
//
// The edge rule: guest edge g maps to host edge E(g) = ceil(g*k/16 - 1/2) = floor((g*k + 7) / 16). That is the column (row)
// of the first host pixel whose centre is at or right of (below) the scaled edge, i.e. exactly the pixels a rasterizer with
// the top-left rule covers for geometry drawn through the scaled viewport. Scissors, clear rectangles and copy rectangles
// therefore agree with scaled draws of the same guest rectangle, adjacent guest rectangles stay adjacent (no gap, no
// overlap: both use E of the shared edge), and E is monotonic. For k = 16, E(g) = g for every integer g.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace recomp::gpu::rs {

inline constexpr int One=16;               // the identity, in sixteenths
inline constexpr int MinK=8,MaxK=32;       // 50 % .. 200 %
inline constexpr unsigned FramePitch=1280; // the surface pitch of the game's full-resolution frame (the scaled surfaces)
inline constexpr unsigned FrameHeight=720;
inline constexpr unsigned WritebackTexels=4096; // generic_renderer.cpp MaxWritebackTexels (static_assert there)

// ---- the setting
struct Setting {
    int k=One;            // sixteenths actually used
    int percent=100;      // the number in the file (100 when absent / malformed)
    bool present=false;   // the file had any byte
    bool valid=true;      // false: malformed text or a number outside 50..200 (k = One)
    bool outOfRange=false;// a well-formed number outside 50..200 (percent keeps it for the log; k = One, valid = false)
};
inline constexpr int MinPercent=50,MaxPercent=200;
inline constexpr std::size_t MaxFileBytes=63; // the reader passes at most MaxFileBytes+1 bytes: a longer file is malformed, not cut
// percent -> sixteenths: round to nearest (half up), then clamp.
constexpr int sixteenthsOf(int percent) noexcept {
    const long long k=(static_cast<long long>(percent)*16+50)/100; // (percent >= 0 here)
    return static_cast<int>(std::clamp<long long>(k,MinK,MaxK));
}
// Parses the file's bytes. Grammar: [space]* digit{1,4} [space]* ['%'] [space]* ; space = ' ', \t, \r, \n. Everything else
// (a sign, a decimal point, letters, 5+ digits, a second number) is malformed: the default, valid=false. A well-formed number
// outside 50..200 is the default too (valid=false, outOfRange=true). More than MaxFileBytes bytes: malformed (the reader
// hands over only the file's first MaxFileBytes+1 bytes, so a longer file must not be judged by its beginning).
inline Setting parse(const char* text,std::size_t n) noexcept {
    Setting s;
    if(!text || !n)return s;
    s.present=true;
#if !defined(RECOMP_RS_MUTANT_LONG_FILE) // test mutant: a long file is judged by the bytes that were read
    if(n>MaxFileBytes){s.valid=false;return s;}
#endif
    const auto space=[](char c){return c==' ' || c=='\t' || c=='\r' || c=='\n';};
    std::size_t i=0;
    while(i<n && space(text[i]))++i;
    if(i==n){s.present=false;return s;} // (only white space: as an empty file)
    int value=0;unsigned digits=0;
    while(i<n && text[i]>='0' && text[i]<='9'){value=value*10+(text[i]-'0');++i;if(++digits>4){s.valid=false;return s;}}
    if(!digits){s.valid=false;return s;}
    while(i<n && space(text[i]))++i;
    if(i<n && text[i]=='%')++i;
    while(i<n && space(text[i]))++i;
    if(i!=n){s.valid=false;return s;}
    s.percent=value;
#if !defined(RECOMP_RS_MUTANT_RANGE_CLAMPS) // test mutant: a number outside 50..200 selects the nearest extreme (640x360 / 2560x1440)
    if(value<MinPercent || value>MaxPercent){s.valid=false;s.outOfRange=true;return s;}
#endif
    s.k=sixteenthsOf(value);
    return s;
}

// ---- edges and extents
// floor((g*k + 7) / 16) for any sign of g (arithmetic shift is not relied on).
constexpr std::int32_t edge(std::int32_t g,int k) noexcept {
#if defined(RECOMP_RS_MUTANT_ROUND_UP) // test mutant: round half up (disagrees with the rasterizer at exact halves)
    const long long v=static_cast<long long>(g)*k+8;
#elif defined(RECOMP_RS_MUTANT_TRUNCATE) // test mutant: truncation (edges move left/up; negative edges move the other way)
    const long long v=static_cast<long long>(g)*k;return static_cast<std::int32_t>(v/16);
#else
    const long long v=static_cast<long long>(g)*k+7;
#endif
    return static_cast<std::int32_t>(v>=0?v/16:-((-v+15)/16));
}
// An image extent: at least one pixel for a non-empty guest extent.
constexpr std::uint32_t extent(std::uint32_t g,int k) noexcept {
    if(!g)return 0;
#if defined(RECOMP_RS_MUTANT_ZERO_EXTENT) // test mutant: a small guest extent may map to an empty image
    return static_cast<std::uint32_t>(edge(static_cast<std::int32_t>(g),k));
#else
    return static_cast<std::uint32_t>(std::max<std::int32_t>(1,edge(static_cast<std::int32_t>(g),k)));
#endif
}
// An edge of a surface drawn with a window offset (a screen tile): the surface pixel g shows the screen pixel g - offset, and
// the tile is placed a whole number of host pixels, E(offset), from where its picture starts, so the host edge is
// E(g - offset) + E(offset). Equal to E(g) whenever the offset is a multiple of 16 (every tile the game uses) or zero. (The
// viewport, hostViewport below, uses the same E(offset); -E(-offset) would differ from it only when offset*k is 8 modulo 16.)
constexpr std::int32_t edgeWithOffset(std::int32_t g,std::int32_t offset,int k) noexcept {
#if defined(RECOMP_RS_MUTANT_OFFSET_IGNORED) // test mutant: the tile phase ignored
    (void)offset;return edge(g,k);
#else
    return edge(g-offset,k)+edge(offset,k);
#endif
}

struct Rect {std::int32_t x0=0,y0=0,x1=0,y1=0;}; // half open, guest or host pixels
constexpr bool empty(const Rect& r) noexcept {return r.x1<=r.x0 || r.y1<=r.y0;}
// A guest rectangle (already clipped to the guest surface by the caller) in host pixels, clipped to the host image.
constexpr Rect hostRect(const Rect& g,int k,std::uint32_t hostWidth,std::uint32_t hostHeight,std::int32_t offsetX=0,std::int32_t offsetY=0) noexcept {
    const auto w=static_cast<std::int32_t>(hostWidth),h=static_cast<std::int32_t>(hostHeight);
    Rect r;
    r.x0=std::clamp(edgeWithOffset(g.x0,offsetX,k),0,w);r.y0=std::clamp(edgeWithOffset(g.y0,offsetY,k),0,h);
#if defined(RECOMP_RS_MUTANT_RECT_SIZE) // test mutant: the size scaled instead of the far edge (adjacent rectangles gap or overlap)
    r.x1=std::clamp(r.x0+edge(g.x1-g.x0,k),r.x0,w);r.y1=std::clamp(r.y0+edge(g.y1-g.y0,k),r.y0,h);
#else
    r.x1=std::clamp(edgeWithOffset(g.x1,offsetX,k),r.x0,w);r.y1=std::clamp(edgeWithOffset(g.y1,offsetY,k),r.y0,h);
#endif
    return r;
}

// ---- viewport
struct Viewport {float x=0,y=0,width=0,height=0;};
// The viewport of a draw. `v` is the guest viewport AS THE RENDERER COMPUTED IT (x, y, width, height as VkViewport, the height
// possibly negative), i.e. with the window offset (ox, oy) already added to x and y; (ox, oy) is that offset (0 when the
// hardware does not add it to vertex window coordinates). The picture without the offset is scaled by k/16 (sub-pixel
// positions kept), and the tile's origin is a whole number of host pixels, E(offset), as it is a whole number of pixels on the
// console:  (x - ox) * s + E(ox)  =  x * s + (E(ox) - ox * s).
// The second form is the one computed: its correction term is exactly 0 when the offset is a multiple of 16 (every tile the
// game uses) or zero, so those viewports are exactly x * s.
inline Viewport hostViewport(const Viewport& v,std::int32_t ox,std::int32_t oy,int k) noexcept {
    const float s=static_cast<float>(k)*(1.0f/16.0f);
    Viewport h;
#if defined(RECOMP_RS_MUTANT_VIEWPORT_X) // test mutant: the tile phase of the horizontal offset dropped
    h.x=v.x*s;(void)ox;
#else
    h.x=v.x*s+(static_cast<float>(edge(ox,k))-static_cast<float>(ox)*s);
#endif
#if defined(RECOMP_RS_MUTANT_VIEWPORT_Y) // test mutant: the vertical offset left in guest pixels
    h.y=(v.y-static_cast<float>(oy))*s+static_cast<float>(oy);
#else
    h.y=v.y*s+(static_cast<float>(edge(oy,k))-static_cast<float>(oy)*s);
#endif
    h.width=v.width*s;h.height=v.height*s;
    return h;
}

// ---- copies between images
struct Copy {std::int32_t sx=0,sy=0,dx=0,dy=0;std::uint32_t w=0,h=0;};
// A 1:1 texel copy between two images of the SAME scale: guest rectangle (sx, sy, w, h) of the source to (dx, dy) of the
// destination, as a host copy. The extent is the smaller of the two host spans (they are equal when the rectangle's and the
// destination's offsets differ by a multiple of 16, the game's tiles), clipped to both host images: never outside either.
constexpr Copy hostCopy(const Copy& g,int k,std::uint32_t srcHostW,std::uint32_t srcHostH,std::uint32_t dstHostW,std::uint32_t dstHostH) noexcept {
    Copy h;
    const auto gw=static_cast<std::int32_t>(g.w),gh=static_cast<std::int32_t>(g.h);
    h.sx=std::clamp(edge(g.sx,k),0,static_cast<std::int32_t>(srcHostW));h.sy=std::clamp(edge(g.sy,k),0,static_cast<std::int32_t>(srcHostH));
    h.dx=std::clamp(edge(g.dx,k),0,static_cast<std::int32_t>(dstHostW));h.dy=std::clamp(edge(g.dy,k),0,static_cast<std::int32_t>(dstHostH));
    const std::int32_t sw=edge(g.sx+gw,k)-edge(g.sx,k),dw=edge(g.dx+gw,k)-edge(g.dx,k);
    const std::int32_t sh=edge(g.sy+gh,k)-edge(g.sy,k),dh=edge(g.dy+gh,k)-edge(g.dy,k);
#if defined(RECOMP_RS_MUTANT_COPY_CLIP) // test mutant: the copy not clipped to the destination image
    const std::int32_t w=std::min({sw,dw,static_cast<std::int32_t>(srcHostW)-h.sx});
    const std::int32_t hh=std::min({sh,dh,static_cast<std::int32_t>(srcHostH)-h.sy});
#else
    const std::int32_t w=std::min({sw,dw,static_cast<std::int32_t>(srcHostW)-h.sx,static_cast<std::int32_t>(dstHostW)-h.dx});
    const std::int32_t hh=std::min({sh,dh,static_cast<std::int32_t>(srcHostH)-h.sy,static_cast<std::int32_t>(dstHostH)-h.dy});
#endif
    h.w=static_cast<std::uint32_t>(std::max(0,w));h.h=static_cast<std::uint32_t>(std::max(0,hh));
    return h;
}
// A scaling blit from an image of scale kSrc to an image of scale kDst (a scaled surface resolved into a guest-sized texture
// the CPU reads back, or the scaled front buffer into the 1280x720 present image): each side's own host rectangle.
struct Blit {Rect src,dst;};
constexpr Blit hostBlit(const Copy& g,int kSrc,std::uint32_t srcHostW,std::uint32_t srcHostH,int kDst,std::uint32_t dstHostW,std::uint32_t dstHostH) noexcept {
    const auto gw=static_cast<std::int32_t>(g.w),gh=static_cast<std::int32_t>(g.h);
    Blit b;
    b.src=hostRect({g.sx,g.sy,g.sx+gw,g.sy+gh},kSrc,srcHostW,srcHostH);
    b.dst=hostRect({g.dx,g.dy,g.dx+gw,g.dy+gh},kDst,dstHostW,dstHostH);
    return b;
}

// ---- which images are scaled
// A render target (an EDRAM surface, keyed by base, format, pitch): scaled when it has the frame's pitch. Surfaces of other
// pitches (half and quarter resolution effects, shadow atlases, reflection maps, the luminance chain, data grids) keep the
// console's size: their texels are data or already-reduced pictures, and every pass that reads a scaled picture samples it
// with normalized coordinates.
constexpr int targetK(unsigned pitch,int k) noexcept {
#if defined(RECOMP_RS_TEST_ALL_PITCHES) // host harness: every surface scaled (its scenes use small pitches)
    (void)pitch;return k;
#elif defined(RECOMP_RS_MUTANT_PITCH) // test mutant: half-resolution surfaces scaled too
    return pitch==FramePitch || pitch==FramePitch/2?k:One;
#else
    return pitch==FramePitch?k:One;
#endif
}
// A resolve destination (the texture a surface is copied into): it takes its source surface's scale, except where texels
// must stay the console's: a destination the CPU reads back (at most WritebackTexels texels, colour only) and a destination
// with mip levels, array layers or cube faces (their level sizes follow the guest size).
constexpr int resolvedK(int sourceK,unsigned width,unsigned height,bool depth,bool singleLevelAndLayer,bool writebackEnabled) noexcept {
    if(sourceK==One)return One;
    if(!singleLevelAndLayer)return One;
#if !defined(RECOMP_RS_MUTANT_WRITEBACK_SCALED) // test mutant: a destination the CPU reads back is scaled
    if(!depth && writebackEnabled && static_cast<std::uint64_t>(width)*height<=WritebackTexels)return One;
#endif
    return sourceK;
}

// ---- values that leave the GPU in pixel units
// The fragment position a guest pixel shader reads (the 360's vPos register) must be in guest pixels: host position times
// this factor (applied to x and y of FragCoord in the shader variant used on scaled surfaces, spirv_frag_scale.h).
constexpr float fragCoordFactor(int k) noexcept {return 16.0f/static_cast<float>(k);}
// An occlusion query's sample count (the game compares it with pixel thresholds): host samples -> guest samples, rounded to
// nearest; a non-zero host count never becomes zero ("visible" stays visible).
constexpr std::uint64_t guestSamples(std::uint64_t hostSamples,int k) noexcept {
    if(!hostSamples || k==One)return hostSamples;
    const std::uint64_t kk=static_cast<std::uint64_t>(k)*static_cast<std::uint64_t>(k);
#if defined(RECOMP_RS_MUTANT_SAMPLES_LINEAR) // test mutant: divided by the scale instead of its square
    const std::uint64_t g=(hostSamples*16+static_cast<std::uint64_t>(k)/2)/static_cast<std::uint64_t>(k);
#else
    const std::uint64_t g=(hostSamples*256+kk/2)/kk;
#endif
    return std::max<std::uint64_t>(1,g);
}
// The scale of the pass of each counted draw of one query pool of N queries (the renderer's OcqSlot): draw i of the pool
// was recorded in a pass of scale k, and its host sample count is reported in guest samples. A fixed array, so that noting
// a draw cannot fail (no allocation); an index outside the pool is ignored / reported unscaled. Entries of an earlier use of
// the pool are overwritten before they are read (every counted draw notes its own index first).
template<std::size_t N> struct SampleScales {
    std::array<std::uint8_t,N> k{}; // (0 = never noted = One)
    constexpr void note(std::size_t i,int passK) noexcept {
#if defined(RECOMP_RS_MUTANT_SAMPLES_SLOT) // test mutant: every draw's scale kept in the first entry
        i=0;
#endif
        if(i<N)k[i]=static_cast<std::uint8_t>(passK);
    }
    constexpr std::uint64_t guest(std::size_t i,std::uint64_t hostSamples) const noexcept {return guestSamples(hostSamples,i<N && k[i]?k[i]:One);}
};
// A diagnostic probe point given in guest pixels (census points, bot vision samples): the host texel whose centre is nearest.
constexpr std::uint32_t probe(std::uint32_t g,int k,std::uint32_t hostExtent) noexcept {
    if(!hostExtent)return 0;
    const std::uint64_t h=(static_cast<std::uint64_t>(g)*2+1)*static_cast<std::uint64_t>(k)/32; // floor((g + 0.5) * k / 16)
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(h,hostExtent-1));
}

// ---- limits
// Whether a scale can be used on a device: the widest scaled image and the renderer's screen-space viewport must fit.
constexpr bool fits(int k,std::uint32_t maxImageDimension2D,std::uint32_t maxViewportDimension,std::uint32_t screenSpaceExtent) noexcept {
    if(k<MinK || k>MaxK)return false;
    return extent(FramePitch,k)<=maxImageDimension2D && extent(4096,k)<=maxImageDimension2D &&
           extent(screenSpaceExtent,k)<=maxViewportDimension;
}

} // namespace recomp::gpu::rs
