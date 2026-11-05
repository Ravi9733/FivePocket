#pragma once
// lane fsr-stage-b (render-resolution stage B, on v463): the size of the picture the renderer hands to the presenter.
// Pure functions and one process-wide value; no Vulkan, no I/O. Host tests: agents/fsr-stage-b/tests/present_source_test.cpp.
//
// Stage A (lane fsr-modes, render_scale.h) draws the game's full-resolution surfaces at k/16 of 1280x720 and then blits
// the finished frame into the 1280x720 present image, so the presenter's scalers always saw 1280x720. Stage B: with a
// scale applied, a frame that goes to the presenter on the GPU (Renderer::present's `direct` frames) is handed over at its
// render size, and the presenter scales THAT to the window in one step:
//   - the picture already has the size of the window's image rect (150 % on a 1920x1080 panel): the tables at 1:1 and a
//     1:1 copy, no scaler and no sharpening (Path::One);
//   - FSR 1 (EASU + RCAS or EASU only) or Snapdragon GSR 1 is the frame's scaler and the picture is not larger than the
//     rect: its constants are computed for the render size and it reads a render-size map picture (Path::Scaler);
//   - any other scaler (nearest, linear, bicubic and its compute / gather / packed forms), a picture larger than the rect,
//     or after a failure of the render-size objects: the presenter itself blits the picture to a 1280x720 image, which is
//     stage A's blit, and the unchanged v463 passes run on that (Path::Down).
// With no scale applied nothing here is consulted beyond one relaxed load per GPU frame: the source is 1280x720
// (Path::Legacy) and every expression of the presenter reduces to v463's.
#include "direct_present_reference.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace recomp::gpu::direct::rsb {
struct Size {std::uint32_t w=SourceWidth,h=SourceHeight;};
constexpr bool operator==(Size a,Size b) noexcept {return a.w==b.w && a.h==b.h;}
constexpr bool operator!=(Size a,Size b) noexcept {return !(a==b);}
constexpr bool isDefault(Size s) noexcept {return s.w==SourceWidth && s.h==SourceHeight;}
// The sizes stage A can render: 80k x 45k for k = 8 .. 32 sixteenths (render_scale.h: 50 % .. 200 %). Every one of them is
// exactly 16:9, so the window's image rect (layout(): the largest 16:9 rect) is the same for every scale.
constexpr bool valid(Size s) noexcept {
    return s.w>=640 && s.w<=2560 && s.w%80==0 && std::uint64_t{s.h}*16==std::uint64_t{s.w}*9;
}
// One word for the atomic: 0 = the default 1280x720 (so a zero-initialised value means "v463").
constexpr std::uint64_t pack(Size s) noexcept {return isDefault(s) || !valid(s)?0ull:(std::uint64_t{s.w}<<32)|s.h;}
constexpr Size unpack(std::uint64_t v) noexcept {
    const Size s{std::uint32_t(v>>32),std::uint32_t(v&0xFFFFFFFFull)};
    return v && valid(s)?s:Size{};
}
// The size of the renderer's GPU frames for this run. Written once by Renderer::initialize (rsbLoad, before any present);
// read by the renderer (the present image's extent) and by the presenter (every GPU frame), so both always agree.
inline std::atomic<std::uint64_t> gpuSourceBits{0};
inline bool setGpuSource(std::uint32_t w,std::uint32_t h) noexcept {
    const Size s{w,h};
    if(!valid(s)){gpuSourceBits.store(0,std::memory_order_relaxed);return false;}
    gpuSourceBits.store(pack(s),std::memory_order_relaxed);return !isDefault(s);
}
inline Size gpuSource() noexcept {return unpack(gpuSourceBits.load(std::memory_order_relaxed));}

// Renderer::present: the present image's extent along one axis. `stageB` = the run's render extent on that axis, or 0 when
// stage B is off (no scale, or files/private-trace/diag-rs-stage-a). Only a frame that goes to the presenter on the GPU is
// handed over at render size; every frame that is read back (census, frame interpolation, the sRGB image, the display
// ramp, a presenter that is not ready) keeps the guest's width x height.
constexpr unsigned presentExtent(unsigned guest,unsigned stageB,bool direct) noexcept {
    return direct && stageB?stageB:guest;
}

// Renderer::present (review): the front buffer the frame is blitted from should itself have the present image's size in a
// stage B run (stage A scaled it). If it does not (a front buffer stage A left at 1280x720), the blit is a filtered
// upscale and the presenter's "1:1 copy" or scaler then works on a soft picture: counted and logged, never silent.
constexpr bool frontMismatch(unsigned stageB,unsigned frontW,unsigned frontH,unsigned presentW,unsigned presentH,bool direct) noexcept {
    return stageB && direct && (frontW!=presentW || frontH!=presentH);
}

// ---- the presenter's choice for one frame
enum class Path : unsigned {Legacy=0,Down=1,One=2,Scaler=3};
constexpr const char* pathName(Path p) noexcept {return p==Path::Legacy?"legacy":p==Path::Down?"down":p==Path::One?"one":"scaler";}
// The 1:1 copy: the picture has exactly the image rect's size. Decided before any scaler object is made (no FSR / GSR
// object is needed for such a frame). bigFailed: the render-size map pictures could not be made; oneFailed: the copy
// failed its audit.
constexpr bool wantsOne(Size src,std::uint32_t rectW,std::uint32_t rectH,bool bigFailed,bool oneFailed) noexcept {
    return !isDefault(src) && !bigFailed && !oneFailed && src.w==rectW && src.h==rectH;
}
// A frame whose scaler (when it has FSR 1 or GSR 1) will read the render-size map picture. Only for a picture that fits
// inside the image rect: EASU and GSR are upscalers (a 12-tap neighbourhood around each output pixel), so a picture larger
// than the rect (200 % on a 1920x1080 window, or 150 % on a smaller one) would be under-sampled by them. Such a picture
// takes the down path: stage A's filtered blit to 1280x720, then the frame's scaler, as in stage A.
// (review) ... and only within the range AMD states for EASU (ffx_fsr1.h: "1x to 4x area"): a picture whose area is less
// than a quarter of the rect's (50 % .. 69 % on a 1920x1080 window; 75 % is exactly 4x) also takes the down path, where
// the scaler works from the 1280x720 copy (2.25x area on that window) as in stage A. The same bound is used for GSR 1
// (this lane's choice: one rule for both scalers; Qualcomm states no range in the sources here).
constexpr unsigned MaxScalerArea=4;
constexpr bool withinScalerRange(Size src,std::uint32_t rectW,std::uint32_t rectH) noexcept {
    return std::uint64_t{src.w}*src.h*MaxScalerArea>=std::uint64_t{rectW}*rectH;
}
constexpr bool wantsBig(Size src,std::uint32_t rectW,std::uint32_t rectH,bool bigFailed,bool one) noexcept {
    return withinScalerRange(src,rectW,rectH) && !isDefault(src) && !bigFailed && !one && src.w<=rectW && src.h<=rectH;
}
constexpr Path path(Size src,bool one,bool big) noexcept {
    return isDefault(src)?Path::Legacy:one?Path::One:big?Path::Scaler:Path::Down;
}
// The source size the frame's scaler constants (FsrEasuCon, GSR's ViewportInfo) and its CPU reference are computed for.
constexpr Size scalerSource(Size src,bool big) noexcept {
    return big?src:Size{};
}
// The extent of the picture an audit reads back as "the source": the render-size picture on the One and Scaler paths, the
// 1280x720 picture otherwise (on the Down path the audit's source is the presenter's own 1280x720 copy).
constexpr Size auditSource(Size src,bool one,bool big) noexcept {return one || big?src:Size{};}
constexpr std::size_t bytes(Size s) noexcept {return std::size_t{s.w}*s.h*4;}
// The audit's one source buffer serves every kind of audited frame of the run: render-size pictures (One, Scaler) and
// 1280x720 pictures (CPU frames, the down path's copy). So it is never smaller than either (a 75 % picture is smaller than
// 1280x720).
constexpr std::size_t auditBufferBytes(Size src) noexcept {return bytes(src)>SourceBytes?bytes(src):SourceBytes;}
// The pass-time slot of the 1:1 copy (direct_present.h PassSlots = 11: slots 0..9 are v463's).
constexpr unsigned OneSlot=10;

// The window's bytes for the 1:1 copy of a render-size RGBA source: inside the image rect each source texel through the
// tables, outside black, alpha 255 (BGRA byte order when bgra). False unless the source is exactly the rect's size.
inline bool referenceOne(const std::uint8_t* source,Size src,const Lut& lut,std::uint32_t width,std::uint32_t height,bool bgra,
                         std::vector<std::uint8_t>& out) {
    const auto l=layout(width,height);
    if(!l || !source || l->imageWidth!=src.w || l->imageHeight!=src.h)return false;
    out.assign(std::size_t{width}*height*4,0);
    for(std::size_t i=3;i<out.size();i+=4)out[i]=255;
    for(std::uint32_t y=0;y<src.h;++y)for(std::uint32_t x=0;x<src.w;++x) {
        const auto s=(std::size_t{y}*src.w+x)*4,d=(std::size_t(y+l->top)*width+x+l->left)*4;
        const std::uint8_t r=lut[source[s]],g=lut[256+source[s+1]],b=lut[512+source[s+2]];
        out[d]=bgra?b:r;out[d+1]=g;out[d+2]=bgra?r:b;
    }
    return true;
}
}
