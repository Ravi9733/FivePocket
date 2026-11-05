#pragma once
// framegen-1b (frame generation, stage 1b: the generator; 2026-10-05). Our own code. The PURE part: no Vulkan, no I/O, no lock.
//
// What the option is: while files/private-trace/diag-fg-between exists, lane A's fixed 30 fps cap (diag-pace-30) paces the game
// and the pair gate of fg_pair.h is open, every real frame k is shown as two pictures two refreshes apart: [a MADE picture that
// lies between the real frames k-1 and k, the real frame k]. The made picture is drawn by five small GPU passes in the
// presenter (fg_gen_presenter.inc; shaders fg_gen_*.frag on fg_gen_core.glsl) from inputs that exist at present time
// (local-analysis/agents/frame-gen/FEASIBILITY.md section 1): the two real pictures, the current frame's scene depth and the
// two frames' cameras (the tonemap draw's pixel constants c8-c15 and c130). Nothing of the game changes: its simulation, the
// frames it renders and when it reads input are untouched; with the switch file absent no decision anywhere differs.
//
// This file holds what can be decided without a GPU and is tested on the host (tests/fg_gen_test.cpp):
//   - the camera of a frame as the tonemap draw carries it, its validity test and the "cut" test between two frames;
//   - the cur -> prev matrix the passes use (double precision, handed over as float32);
//   - the per-frame decision: generate / keep this picture for the next pair / show the real frame alone;
//   - the push-constant block of the passes;
//   - the SLOT KEEPER: when made pictures do not keep their slot (shown after it too often, lost, or too expensive), or the
//     frames' inputs keep coming and going, the generator is switched off and real frames are shown alone, exactly as with
//     the cap alone; the presenter logs "RENDER_DIRECT_FG_GEN fallback=real-only why=..." (the marker the device plan reads);
//   - the capability marker the app's Settings screen looks for (lane menu-settings, Capabilities.FRAME_GENERATION_MARKER).
// fg-scaled (2026-10-10; every edit is marked fg-scaled): the generator works on the run's SOURCE picture, whatever render
// size it has (present_source.h: 80k x 45k for a scale of k sixteenths), not on 1280x720 alone:
//   - Geometry / geometry(): every picture's size from the source picture's; sizeOk(): which source pictures are taken;
//   - decide() and push() are given the source picture's size; FrameIn says which size the renderer handed;
//   - pictureIsFrame(): the renderer's rule for which present image is a frame's picture at a render scale.
// For a 1280x720 source every value, decision and byte below is what it was (tests: fg_scaled_diff_test feeds the same
// inputs to v513's file and to this one).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace recomp::fgg {

// ---- switch word (the presenter's fg.switches(); fg_pair.h owns bits 1, 2, 4, 8)
constexpr unsigned SwGenBetween=16u; // files/private-trace/diag-fg-between: the generator, between order (the renderer also sets fg_pair.h's SwBetween)
constexpr unsigned SwShow=32u;       // diag-fg-show: refused blocks tinted red, "did not move" blocks blue (to look at the classifier)
constexpr unsigned PairBetween=1u;   // fg_pair.h's SwBetween (not included here; fg_gen_presenter.inc asserts the two agree)
// The presenter's word once the generator's files were polled: stage 1a's bits as its own poll left them (a generator bit
// that is still in it goes away with its file); with diag-fg-between the pair's in-between order and the generator; the
// tint only together with the generator.
constexpr unsigned switchWord(unsigned stage1a,bool between,bool show) noexcept {
    return (stage1a&~(SwGenBetween|SwShow))|(between?PairBetween|SwGenBetween|(show?SwShow:0u):0u);
}

// The app's Settings row "Frame generation" is live only in a library that carries this text as the END of a string
// (android .../Capabilities.java FRAME_GENERATION_MARKER, lane menu-settings) AND the name diag-fg-between AND diag-pace-30.
// The presenter's first RENDER_DIRECT_FG_GEN line ends with it. The predicted-frame order is not built: the name of its
// switch file is deliberately in no string of this lane, so the app does not offer it.
#define RECOMP_FGG_CAPABILITY "recomp-cap:framegen1"
#define RECOMP_FGG_AUTO_CAPABILITY "recomp-cap:framegen-auto1" // pace-auto-pairs (v524): the picture is also made under the automatic pace's 30, 20 and 15 fps rungs

constexpr unsigned Width=1280,Height=720,Block=8,BlocksW=Width/Block,BlocksH=Height/Block; // (fg-scaled: the sizes of a 100 % picture, the game's own frame; a run's sizes are its Geometry)

// ---- fg-scaled (2026-10-10): the sizes of the generator's pictures for a source picture of w x h.
// THE RULE AT THE PICTURE'S EDGE. A picture need not be a whole number of 8 x 8 blocks (960x540 is 67.5 block rows):
//   - the block maps have one block for every STARTED block: ceil(w / 8) x ceil(h / 8). The synthesis asks for the
//     block of every pixel (x / 8, y / 8), so a smaller map would be read outside;
//   - a block that crosses the picture's right or bottom edge is judged from the pixels it has inside the picture and
//     from nothing else (their sums divided by their number): the reference's rule (host-model/fg_reference.h);
//   - the terms' picture is whole blocks (blocksW x 8, blocksH x 8), so that the CLASSIFY pass reads 64 texels of every
//     block inside it. Its texels outside the source picture hold zero (fg_gen_core.glsl fgTermsEdge) and add nothing;
//   - the two low-passed pictures and the made picture have the source picture's size; the column sums are blocksW x 2;
//     the gain is 1x1. The GAIN pass counts whole blocks only (floor), as its text always did.
// edge(): a block crosses the edge. Then, and only then, the two passes of such a picture are drawn with the modules
// Termsedge and Classifyedge; a picture of whole blocks is drawn with v513's seven modules alone.
struct Geometry {
    unsigned w=Width,h=Height;                // the source picture, the two low-passed pictures, the made picture
    unsigned blocksW=BlocksW,blocksH=BlocksH; // the two block maps; blocksW is also the width of the column sums
    unsigned termsW=Width,termsH=Height;      // the terms' picture
    constexpr unsigned blocks() const noexcept {return blocksW*blocksH;}
    constexpr bool edge() const noexcept {return termsW!=w || termsH!=h;}
    // the bytes of the generator's own pictures (the objects line of the log): two low-passed pictures, two block maps,
    // the gain texel, the column sums (two rows), the terms (16 bytes a texel)
    constexpr unsigned bytes() const noexcept {return 2u*w*h*4u+2u*blocksW*blocksH*4u+4u+blocksW*2u*4u+termsW*termsH*16u;}
    friend constexpr bool operator==(const Geometry& a,const Geometry& b) noexcept {
        return a.w==b.w && a.h==b.h && a.blocksW==b.blocksW && a.blocksH==b.blocksH && a.termsW==b.termsW && a.termsH==b.termsH;
    }
    friend constexpr bool operator!=(const Geometry& a,const Geometry& b) noexcept {return !(a==b);}
};
constexpr Geometry geometry(unsigned w,unsigned h) noexcept {
    Geometry g;g.w=w;g.h=h;
    g.blocksW=(w+Block-1)/Block;g.blocksH=(h+Block-1)/Block;
    g.termsW=g.blocksW*Block;g.termsH=g.blocksH*Block;
    return g;
}
static_assert(geometry(Width,Height)==Geometry{} && !Geometry{}.edge() && Geometry{}.bytes()==22234884u,"a 1280x720 picture has v513's sizes");
// Which source pictures the generator takes: the renderer's sizes (present_source.h rsb::valid: 80k x 45k, exactly
// 16:9) from 640x360 (50 %) to 1280x720 (100 %): k = 8 .. 16.
// Larger pictures (k = 17 .. 32) stay refused (why=source), for two different reasons:
//   - from 1600x900 (k = 20) on, the GAIN pass's sums leave the range in which float32 holds every integer
//     (fg_gen_core.glsl pass 2: at most GainExactBlocks whole blocks; 1600x900 has 22,400), so its two steps would no
//     longer be exact;
//   - 1360x765 .. 1520x855 (k = 17 .. 19) are inside that bound and pass every host test of lane fg-scaled, but no
//     picture above 100 % has been looked at on a phone. MaxWidth is the one constant to raise for them.
constexpr unsigned MinWidth=640,MaxWidth=Width,GainExactBlocks=21931; // (765 x 21,931 = 16,777,215 = 2^24 - 1)
constexpr unsigned gainBlocks(unsigned w,unsigned h) noexcept {return (w/Block)*(h/Block);} // the whole blocks the GAIN pass counts
constexpr bool sizeOk(unsigned w,unsigned h) noexcept {
    return w>=MinWidth && w<=MaxWidth && w%80u==0 && std::uint64_t{h}*16u==std::uint64_t{w}*9u;
}
static_assert(sizeOk(Width,Height) && sizeOk(960,540) && sizeOk(640,360) && !sizeOk(1360,765) && !sizeOk(560,315),"the sizes the generator takes");
// (the largest picture sizeOk() takes has the most blocks: MaxWidth cannot be raised past the GAIN pass's exact range)
static_assert(MaxWidth%80u==0 && gainBlocks(MaxWidth,MaxWidth*9u/16u)<=GainExactBlocks,"the GAIN pass's sums must stay exact integers in float32 for every picture the generator takes");
// The renderer's rule for a frame's picture and depth (fg_gen_renderer.inc): the frame of a run at render scale k
// (sixteenths) is frameW x frameH = the 1280x720 frame's scaled extent. The present image is handed to the generator
// only when it has exactly that size, and a depth target only when it is drawn at that scale: then pixel (x, y) of the
// picture and texel (x, y) of the depth are the same place on the screen, as at 100 %. (Stage A of the render scale
// presents a 1280x720 blit of a smaller frame: that picture and the smaller depth do not match, and nothing is handed.)
constexpr bool pictureIsFrame(unsigned presentW,unsigned presentH,unsigned frameW,unsigned frameH) noexcept {
    return presentW==frameW && presentH==frameH;
}
enum BlockClass : unsigned {BlockCamera=0,BlockZero=1,BlockNone=3}; // a block's class: static world, did not move on screen, neither (the values the classify pass writes; fg_reference.h's)

struct K {
    // the algorithm's constants (DESIGN-stage1.md section 2, step 5; from the model, to be re-tuned on real pictures)
    static constexpr float Fail=10.0f,Ambiguous=1.5f,Disagree=40.0f;
    static constexpr float FailLo=4.0f,FailHi=32.0f;        // diag-fg-fail (text) may move the refusal threshold within this
    // a frame's camera (FEASIBILITY 1.3: gViewInverse orthonormal, dofProj = tan x, tan y, zA, zB)
    static constexpr double Orthonormal=1e-3,ViewProj=1e-3;
    // a cut: the camera moved or turned more between two real frames than driving can explain, or the lens changed
    static constexpr double CutMetres=5.0,CutDegrees=20.0,CutLens=0.10;
    // the slot keeper (ESTIMATES: no device run has set them; the first one should, DEVICE-PLAN.md step G3)
    static constexpr unsigned Window=64;                    // made pictures per verdict
    static constexpr unsigned LateMaxPct=40;                // more than this share shown after their slot: fall back
    static constexpr unsigned LateLo=5,LateHi=95;           // diag-fg-late-max (text, percent) may move it within this
    static constexpr unsigned LostMax=4;                    // pictures of pairs (made or real) not presented within one window: fall back
    static constexpr unsigned InterruptMax=4;               // (repair, check c2) this many interruptions of the inputs ...
    static constexpr unsigned InterruptSpan=600;            // ... within this many real frames (20 s at 30 fps): fall back
    static constexpr double GpuMaxUs=3000.0;                // mean GPU time of the five passes (DESIGN section 12, D6: 3.0 ms)
    static constexpr double GpuMaxLo=2000.0,GpuMaxHi=6000.0; // fg-gpu-max (v639): diag-fg-gpu-max (text, microseconds) may move it within this
    static constexpr unsigned GpuApart=2;                   // ... for pairs this many refreshes apart (fg_pair.h K::Apart: the 30 fps cap)
    static constexpr double CallMaxUs=4000.0;               // mean CPU time of the made picture's call (its acquire may wait)
    static constexpr unsigned MinSamples=16;                // GPU / call means need this many samples
    static constexpr unsigned Cooldown=600;                 // real frames without pairs after a fall-back (20 s at 30 fps)
    static constexpr unsigned MaxTrips=3;                   // fall-backs before the generator stays off for the run
    static constexpr unsigned Ring=8;                       // made pictures waiting for their display time
};

// ---- the camera of one frame: the tonemap draw's pixel constants (words = float bits; register c: words 4c .. 4c+3)
struct Camera {
    float viewProj[16]{};     // c8-c11 (compared with the matrix rebuilt from the two below, never used for the picture)
    float viewInverse[16]{};  // c12-c15: rows right, up, -forward, position (row-vector convention)
    float dofProj[4]{};       // c130: tan(fov x / 2), tan(fov y / 2), zA, zB (reversed Z: depth = zA / distance - zB)
};
inline void takeCamera(Camera& c,const std::array<std::uint32_t,1024>& pixel) noexcept {
    std::memcpy(c.viewProj,pixel.data()+8*4,sizeof c.viewProj);
    std::memcpy(c.viewInverse,pixel.data()+12*4,sizeof c.viewInverse);
    std::memcpy(c.dofProj,pixel.data()+130*4,sizeof c.dofProj);
}

// Why a frame got no made picture (counted per reason in RENDER_DIRECT_FG_GEN nogen=).
enum class Why : unsigned {Ok=0,NoFrame,NoCamera,Orthonormal,Lens,ViewProj,NoDepth,DepthSize,Source,Objects,Cut,NoPrevious,TonemapDraws,Count};
inline const char* text(Why w) noexcept {
    static constexpr const char* names[]{"ok","no-frame","no-camera","orthonormal","lens","viewproj","no-depth","depth-size","source","objects","cut","no-previous","tonemap-draws"};
    return unsigned(w)<unsigned(Why::Count)?names[unsigned(w)]:"?";
}

using M4=std::array<double,16>; // row-major, row-vector convention (out_j = sum_k in_k m[k*4+j]), as the game's constants
inline M4 mul(const M4& a,const M4& b) noexcept {M4 r{};for(int i=0;i<4;++i)for(int j=0;j<4;++j)for(int k=0;k<4;++k)r[i*4+j]+=a[i*4+k]*b[k*4+j];return r;}
// The projection dofProj describes (view x, y, z with -z the distance) and its inverse (ndc x, ndc y, depth, 1 -> view).
inline M4 projection(const Camera& c) noexcept {const double tx=c.dofProj[0],ty=c.dofProj[1],zA=c.dofProj[2],zB=c.dofProj[3];return {1/tx,0,0,0, 0,1/ty,0,0, 0,0,zB,-1, 0,0,zA,0};}
inline M4 unprojection(const Camera& c) noexcept {const double tx=c.dofProj[0],ty=c.dofProj[1],zA=c.dofProj[2],zB=c.dofProj[3];return {tx,0,0,0, 0,ty,0,0, 0,0,0,1/zA, 0,0,-1,zB/zA};}

// Is this a camera the passes can use? gViewInverse orthonormal, the lens in range, and c8-c11 the view-projection that the
// other two registers rebuild (x, y and w columns; the z column has two forms inside one frame and is not compared).
inline Why checkCamera(const Camera& c) noexcept {
    const float* v=c.viewInverse;
    double worst=0;
    for(int a=0;a<3;++a)for(int b=0;b<3;++b){double d=0;for(int k=0;k<3;++k)d+=double(v[a*4+k])*double(v[b*4+k]);worst=std::max(worst,std::fabs(d-(a==b?1.0:0.0)));}
    if(!(worst<=K::Orthonormal) || !(std::fabs(double(v[15])-1.0)<=K::Orthonormal))return Why::Orthonormal;
    for(int k=0;k<3;++k)if(!std::isfinite(v[12+k]))return Why::Orthonormal;
    const double tx=c.dofProj[0],ty=c.dofProj[1],zA=c.dofProj[2],zB=c.dofProj[3];
    if(!(tx>0.05 && tx<5 && ty>0.05 && ty<5 && zA>0 && zA<100 && zB>0 && zB<1))return Why::Lens;
    M4 view{};
    for(int i=0;i<3;++i)for(int j=0;j<3;++j)view[i*4+j]=v[j*4+i];
    for(int j=0;j<3;++j)view[12+j]=-(double(v[12])*v[j*4]+double(v[13])*v[j*4+1]+double(v[14])*v[j*4+2]);
    view[15]=1;
    const M4 vp=mul(view,projection(c));
    double off=0,scale=1e-9;
    for(int i=0;i<4;++i)for(int j:{0,1,3}){off=std::max(off,std::fabs(vp[i*4+j]-double(c.viewProj[i*4+j])));scale=std::max(scale,std::fabs(vp[i*4+j]));}
    return off/scale<=K::ViewProj?Why::Ok:Why::ViewProj;
}

// The renderer's verdict on a frame's camera: the frame had exactly one tonemap draw (what the design measured: 33,901 of
// 33,901 frames), its registers were taken and pass checkCamera(). With two or more the first draw's registers were taken,
// and nothing says they are the scene's camera: such a frame gets no made picture.
inline Why cameraVerdict(unsigned tonemapDraws,bool taken,const Camera& c) noexcept {
    if(!tonemapDraws || !taken)return Why::NoCamera;
    if(tonemapDraws!=1)return Why::TonemapDraws;
    return checkCamera(c);
}

// How far the camera went from `prev` to `cur`.
struct Step {double metres=0,degrees=0,lens=0;};
inline Step poseStep(const Camera& prev,const Camera& cur) noexcept {
    Step s;
    double d2=0,dot=0;
    for(int k=0;k<3;++k){const double d=double(cur.viewInverse[12+k])-double(prev.viewInverse[12+k]);d2+=d*d;dot+=double(cur.viewInverse[8+k])*double(prev.viewInverse[8+k]);}
    s.metres=std::sqrt(d2);s.degrees=std::acos(std::clamp(dot,-1.0,1.0))*57.29577951308232;
    for(int k=0;k<2;++k)s.lens=std::max(s.lens,std::fabs(double(cur.dofProj[k])/double(prev.dofProj[k])-1.0));
    return s;
}
inline bool isCut(const Step& s) noexcept {return !(s.metres<=K::CutMetres && s.degrees<=K::CutDegrees && s.lens<=K::CutLens);}

// The matrix of the passes: (ndc x, ndc y, depth, 1) of the CURRENT frame -> the PREVIOUS frame's clip position:
// inverse(P_cur) x [view_cur -> world -> view_prev] x P_prev. The relative pose is formed from the two gViewInverse with the
// positions subtracted first (world coordinates are of the order of 1e3 m: the difference of two products would lose the
// centimetres in float32). Double precision here, float32 for the GPU. (DESIGN-stage1.md section 2, step 1.)
inline std::array<float,16> curToPrev(const Camera& cur,const Camera& prev) noexcept {
    const float* c=cur.viewInverse;const float* p=prev.viewInverse;
    M4 rel{};
    for(int i=0;i<3;++i)for(int j=0;j<3;++j)rel[i*4+j]=double(c[i*4])*p[j*4]+double(c[i*4+1])*p[j*4+1]+double(c[i*4+2])*p[j*4+2]; // axis i of cur in the axes of prev
    for(int j=0;j<3;++j)rel[12+j]=(double(c[12])-p[12])*p[j*4]+(double(c[13])-p[13])*p[j*4+1]+(double(c[14])-p[14])*p[j*4+2];
    rel[15]=1;
    const M4 m=mul(mul(unprojection(cur),rel),projection(prev));
    std::array<float,16> f{};
    for(int i=0;i<16;++i)f[i]=float(m[i]);
    return f;
}

// ---- one real frame's inputs, handed by the renderer to the presenter just before the frame's present (render thread).
struct FrameIn {
    std::uint32_t tag=0;        // the present's tag (0: none)
    Why why=Why::NoCamera;      // Ok: the camera below passed checkCamera() and the depth view is the frame's scene depth
    Camera cam;
    std::uint64_t sourceView=0; // VkImageView of the picture this frame presents (the renderer's present image, GENERAL layout)
    std::uint64_t depthView=0;  // VkImageView (depth aspect, GENERAL layout), valid until this frame's presenter pass has run
    std::uint32_t depthW=0,depthH=0;
    std::uint64_t frame=0;      // the renderer's frame counter at the tonemap draw (the log)
    std::uint32_t tonemapDraws=0; // tonemap draws of the frame (the renderer's own count)
    std::uint16_t sourceW=Width,sourceH=Height; // fg-scaled: the size of the picture behind sourceView (the renderer says it; 1280x720 unless a render scale is applied). Two 16-bit fields in what was padding: the struct keeps its size, so every member of the presenter behind its copy keeps its offset
};
static_assert(sizeof(FrameIn)==192,"FrameIn keeps v513's size"); // fg-scaled

// ---- the decision for one planned pair (the presenter, under its mutex, before any picture of the frame is recorded).
enum class Act : unsigned {Dry=0,      // the generator's switch is absent: stage 1a's dry run, unchanged
                           NoPair,     // this frame has no usable inputs: the real frame is shown alone
                           Keep,       // usable, but nothing to pair it with (first frame, after a cut): [real, the same picture
                                       // again], and the picture, its low-pass and its camera are kept for the next frame
                           Generate};  // [the made picture between the kept frame and this one, real]
struct Kept {bool valid=false;Camera cam;};
struct Decision {Act act=Act::Dry;Why why=Why::Ok;std::array<float,16> m{};Step step;};
// in: this frame's inputs or null when the renderer handed none for `tag`; objects: the generator's GPU objects exist;
// w, h (fg-scaled; until v513 a flag "the presented picture is Width x Height"): the size of the presented pictures
// of this run, the presenter's own (present_source.h rsb::gpuSource). The generator takes the sizes sizeOk() names;
// the picture the renderer handed must have exactly that size (else its texels would be read as another picture's:
// no-frame), and the depth must cover it (every pass clamps its depth reads to the PICTURE: depth-size).
inline Decision decide(unsigned switches,const FrameIn* in,std::uint32_t tag,const Kept& kept,bool objects,unsigned w,unsigned h) noexcept {
    Decision d;
    if(!(switches&SwGenBetween))return d;
    d.act=Act::NoPair;
    if(!sizeOk(w,h)){d.why=Why::Source;return d;}
    if(!objects){d.why=Why::Objects;return d;}
    if(!in || !tag || in->tag!=tag || !in->sourceView || in->sourceW!=w || in->sourceH!=h){d.why=Why::NoFrame;return d;}
    if(in->why!=Why::Ok){d.why=in->why;return d;}
    if(!in->depthView){d.why=Why::NoDepth;return d;}
    if(in->depthW<w || in->depthH<h){d.why=Why::DepthSize;return d;}
    d.act=Act::Keep;
    if(!kept.valid){d.why=Why::NoPrevious;return d;}
    d.step=poseStep(kept.cam,in->cam);
    if(isCut(d.step)){d.why=Why::Cut;return d;}
    d.act=Act::Generate;d.why=Why::Ok;d.m=curToPrev(in->cam,kept.cam);
    return d;
}

// ---- the push-constant block of every pass (fg_gen_core.glsl `pc`): 96 bytes.
struct Push {
    float m[16];        // m0..m3: the rows of curToPrev
    float a[4];         // picture width, height, phase (0 = the previous real frame, 1 = the current one), DISAGREE
    float b[4];         // FAIL, AMBIGUOUS, show (0 / 1), unused
};
static_assert(sizeof(Push)==96,"the passes' push block");
// (fg-scaled: w, h = the source picture's size, Geometry::w and ::h; every pass reads the picture's size from here.)
inline Push push(const std::array<float,16>& m,float phase,float fail,bool show,unsigned w=Width,unsigned h=Height) noexcept {
    Push p{};
    std::memcpy(p.m,m.data(),sizeof p.m);
    p.a[0]=float(w);p.a[1]=float(h);p.a[2]=phase;p.a[3]=K::Disagree;
    p.b[0]=fail;p.b[1]=K::Ambiguous;p.b[2]=show?1.0f:0.0f;p.b[3]=0.0f;
    return p;
}

// ---- the passes and what each of the four bindings holds in them (fg_gen_core.glsl: uA, uB, uD, uE). One table for the
// presenter (the descriptor sets it writes) and for the host (tests/fg_gen_cpu.h binds the same pictures), so the offline
// evaluator runs the passes on the inputs the phone gives them. A binding a pass does not read holds one it does.
enum class Role : unsigned {Source=0, // the current real picture (the renderer's present image)
                            Prev,     // the previous real picture (the presenter's copy)
                            LowCur,   // the 4 x 4 box of the current picture (written by the LOW pass of this frame)
                            LowPrev,  // the same of the previous picture (written by the LOW pass of the frame before)
                            Depth,    // the current frame's scene depth
                            Gain,     // the 1x1 gain picture
                            Blocks0,Blocks1, // the two block maps (classify writes 0; the three resolve rounds go 0 -> 1 -> 0 -> 1)
                            GainCols, // framegen-cost: the gain's column sums (BlocksW x GainColsRows; row 0 the current picture, row 1 the previous)
                            Terms,    // framegen-cost-b: CLASSIFY's per-pixel terms (TermsW x TermsH, three 32-bit floats per texel: RGBA32F)
                            Count};
// framegen-cost (2026-10-06): the GAIN pass is drawn in two steps, GAIN COLUMNS then GAIN (fg_gen_core.glsl says why the
// gain picture keeps its bytes). PassGainCols is appended, so the other passes keep their numbers; in a frame it is drawn
// between LOW and GAIN.
// framegen-cost-b (2026-10-06, stage B1): the CLASSIFY pass is drawn in two steps, TERMS then CLASSIFY (fg_gen_core.glsl
// says why the block map keeps its bytes). PassTerms is appended; in a frame it is drawn between GAIN and CLASSIFY, with
// the pictures CLASSIFY read until v468. CLASSIFY reads the terms alone.
enum Pass : unsigned {PassLow=0,PassGain,PassClassify,PassResolve01,PassResolve10,PassSynth,PassGainCols,PassTerms,Passes};
constexpr unsigned GainColsRows=2;
constexpr unsigned TermsW=Width,TermsH=Height,TermsTexelBytes=16; // framegen-cost-b: one RGBA32F texel per pixel (14,745,600 bytes) (fg-scaled: for a 1280x720 picture; a run's size is Geometry::termsW x termsH)
inline constexpr Role Bindings[Passes][4]={
    {Role::Source,Role::Source,Role::Source,Role::Source},      // LOW (uA through the LINEAR sampler)
    {Role::GainCols,Role::GainCols,Role::GainCols,Role::GainCols}, // GAIN (framegen-cost: from the column sums; was LowCur, LowPrev)
    {Role::Terms,Role::Terms,Role::Terms,Role::Terms},          // CLASSIFY -> Blocks0 (framegen-cost-b: from the terms; was LowCur, LowPrev, Depth, Gain)
    {Role::Blocks0,Role::Blocks0,Role::Blocks0,Role::Blocks0},  // RESOLVE Blocks0 -> Blocks1 (rounds 1 and 3)
    {Role::Blocks1,Role::Blocks1,Role::Blocks1,Role::Blocks1},  // RESOLVE Blocks1 -> Blocks0 (round 2)
    {Role::Source,Role::Prev,Role::Terms,Role::Blocks1},        // SYNTHESIS -> the made picture (fg-fixer, v630: the terms, whose A is the near depth TERMS stored; until v629 the depth)
    {Role::LowCur,Role::LowPrev,Role::LowCur,Role::LowCur},     // GAIN COLUMNS -> GainCols (framegen-cost: the pictures GAIN read until v467)
    {Role::LowCur,Role::LowPrev,Role::Depth,Role::Gain},        // TERMS -> Terms (framegen-cost-b: the pictures CLASSIFY read until v468)
};
inline constexpr Pass ResolveRounds[3]={PassResolve01,PassResolve10,PassResolve01};

// ---- framegen-cost (2026-10-06): the GPU time of each pass of a made picture (the log's pass_us= line).
// v467 times a made picture with two stamps: one in front of LOW (written at TOP_OF_PIPE) and one behind SYNTHESIS
// (BOTTOM_OF_PIPE). Their difference is fg_us, the number the slot keeper judges; that stays as it is, for every timed
// made picture. Every PassEvery-th timed made picture carries PassStamps more stamps, each written at BOTTOM_OF_PIPE
// outside a render pass: one directly behind v467's first stamp, then one behind each step. By the Vulkan rule such a
// stamp is taken when every command before it has finished, so the difference of two neighbours is the time from the
// end of one step to the end of the next: the step's whole render pass, with whatever the GPU does to begin it and to
// write its target out. The parts of one picture add up to that picture's fg_us exactly.
//   before     v467's first stamp -> everything submitted before the generator's passes has finished. v467's stamp may
//              be taken as soon as the earlier commands have STARTED (the rule for TOP_OF_PIPE), not when they have
//              finished, so fg_us can hold the end of the game's own frame; this part says how much of it does.
//   low .. synth   the seven steps (resolve = the three rounds together). framegen-cost-b: terms and classify are the
//              two steps of what was one part, classify, until v468: their sum is that part's successor.
// The other timed made pictures keep v467's two stamps and nothing else: their mean total (plain_us) beside the split
// pictures' (pass_sum) says what the extra stamps themselves cost. Nothing here is estimated: a picture whose stamps
// were not ready when asked for is counted (unread) and left out; one with a part above OddUs (a stamp out of order or
// a wrapped counter) is counted (odd) and left out.
enum Part : unsigned {PartBefore=0,PartLow,PartGainCols,PartGain,PartTerms,PartClassify,PartResolve,PartSynth,Parts}; // (framegen-cost-b: PartTerms)
constexpr unsigned PassStamps=Parts-1; // the extra stamps of a split picture, in the order they are written: behind v467's first stamp, LOW, GAIN COLUMNS, GAIN, TERMS, CLASSIFY, the third RESOLVE round
constexpr unsigned PassEvery=3;        // one timed made picture in three is split (odd: not in step with the two low-passed pictures or the five swapchain images)
constexpr bool splitDue(std::uint64_t timedMade) noexcept {return timedMade%PassEvery==0;}
struct PassTimes {
    static constexpr double OddUs=1e6;
    std::array<double,Parts> us{};           // the parts' sums over the split pictures that were read, microseconds
    std::uint64_t n=0,unread=0,odd=0;        // split pictures: read and taken; not ready when asked for; read and refused
    double plainUs=0;std::uint64_t plainN=0; // pictures with v467's two stamps only: their totals
    // One split picture. start, end: v467's two stamps; mid: its PassStamps stamps. Raw counter values; mask: the
    // counter's valid bits; nsPerTick: the device's timestamp period. False: refused (counted as odd).
    bool split(std::uint64_t start,const std::uint64_t* mid,std::uint64_t end,std::uint64_t mask,double nsPerTick) noexcept {
        std::array<double,Parts> part{};std::uint64_t prev=start&mask;
        for(unsigned k=0;k<Parts;++k) {
            const std::uint64_t t=(k<PassStamps?mid[k]:end)&mask;
            part[k]=double((t-prev)&mask)*nsPerTick/1e3;prev=t;
            if(!(part[k]<=OddUs)){++odd;return false;}
        }
        for(unsigned k=0;k<Parts;++k)us[k]+=part[k];
        ++n;return true;
    }
    void notReady() noexcept {++unread;}
    void plain(double totalUs) noexcept {plainUs+=totalUs;++plainN;}
    double mean(Part p) const noexcept {return n?us[p]/double(n):0.0;}
    double sum() const noexcept {double s=0;for(const double v:us)s+=v;return n?s/double(n):0.0;} // the split pictures' mean total
    double plainMean() const noexcept {return plainN?plainUs/double(plainN):0.0;}
    bool any() const noexcept {return n || unread || odd || plainN;}
};

// ---- tuning files (text, one number): absent, unreadable or out of range = the default. `text` may be null.
inline float tuned(const char* text,float lo,float hi,float fallback) noexcept {
    if(!text)return fallback;
    char* end=nullptr;const float v=std::strtof(text,&end);
    return end!=text && std::isfinite(v) && v>=lo && v<=hi?v:fallback;
}

// ---- the exposure gain as the gain pass stores it in its 1x1 RGBA8 picture (R = high byte, G = low byte of a 16-bit value)
inline float gainOf(unsigned r,unsigned g) noexcept {return 0.75f+float(r*256u+g)*(0.5f/65535.0f);}

// ---- an audited block map (160 x 90 RGBA8 as the resolve pass leaves it: R = class, G = still open, B = cost x 4)
struct Shares {unsigned blocks=0,camera=0,zero=0,none=0,other=0,open=0;double costSum=0;
    double pct(unsigned n) const noexcept {return blocks?100.0*double(n)/double(blocks):0.0;}
    double meanCost() const noexcept {return blocks?costSum/double(blocks):0.0;}};
inline Shares shares(const std::uint8_t* rgba,unsigned blocks) noexcept {
    Shares s;s.blocks=blocks;
    for(unsigned i=0;i<blocks;++i) {
        const std::uint8_t* b=rgba+std::size_t(i)*4;
        if(b[0]==BlockCamera)++s.camera;else if(b[0]==BlockZero)++s.zero;else if(b[0]==BlockNone)++s.none;else ++s.other;
        s.open+=b[1]>=128;s.costSum+=double(b[2])/4.0;
    }
    return s;
}

// ---- the slot keeper. Fed by the presenter with what happened to each made picture; says whether pairs may be planned.
// A made picture is the pair's FIRST picture: it carries the frame's tag and belongs on the frame's slot (lane A's vsync for
// the frame). It "keeps its slot" when it reaches the screen at that vsync. When too many do not, generation is worse than
// the cap alone, and the keeper falls back: no pair is planned (the presenter passes usable = false to fg_pair.h, so every
// decision is the cap's) for Cooldown real frames, then one more try; after MaxTrips tries it stays off for the run.
// Repair after check c2 (2026-10-05):
//   - INPUTS THAT COME AND GO. A frame whose inputs are refused is shown alone, the next usable one is kept, the one after
//     gets a made picture: every such interruption costs the screen one real-frame gap of 6 refreshes and one of 2 where
//     the cap alone gives 4 and 4. The per-frame decision has no memory, so the keeper counts the interruptions (a refused
//     frame right after a frame that was shown with a made picture; a stretch of refused frames, e.g. a loading screen, is
//     ONE, and refused frames while no picture is being made are none): InterruptMax of them within InterruptSpan real
//     frames fall back (why=inputs). That fall-back cools down and retries like the others but is never final: it says
//     nothing about the device, and the scene that caused it ends.
//   - A pair's REAL frame that is not presented after its made picture took the slot counts as a lost picture (realLost()).
//   - A full window that brought fewer than half of its display times cannot be judged for lateness: it is counted
//     (blindWindows()) and the presenter says so once ("RENDER_DIRECT_FG_GEN keeper=blind ...").
// What the keeper still accepts for ever, by its limits: up to LateMaxPct % late pictures and LostMax - 1 lost pictures in
// every window, InterruptMax - 1 interruptions per InterruptSpan frames; a picture two or more refreshes late counts as
// one late picture.
class SlotKeeper {
public:
    enum class Trip : unsigned {None=0,Late,Lost,Gpu,Call,Disabled,Inputs};
    static const char* text(Trip t) noexcept {
        static constexpr const char* names[]{"none","late","lost","gpu-time","call-time","disabled","inputs"};
        return unsigned(t)<7?names[unsigned(t)]:"?";
    }
    struct Window {unsigned made=0,shown=0,late=0,lost=0,gpuN=0,callN=0,interrupted=0;double gpuUs=0,callUs=0;
        unsigned rel=0,relLate=0,relOff=0; // fg-late-relative (pairShown): made pictures judged by their pair; of them: not shown before their real frame; at another distance than planned
        unsigned latePct() const noexcept {return shown?unsigned(100ull*late/shown):0u;}                    // (for the log: rounded down)
        bool lateOver(unsigned pct) const noexcept {return 100ull*late>std::uint64_t(pct)*shown;}           // (the rule: exact)
        double gpuMean() const noexcept {return gpuN?gpuUs/double(gpuN):0.0;}
        double callMean() const noexcept {return callN?callUs/double(callN):0.0;}};

    bool allows() const noexcept {return state==Run;}
    bool final() const noexcept {return state==Off;}
    unsigned trips() const noexcept {return tripCount;}
    unsigned cooldownLeft() const noexcept {return state==Cool?cool:0u;}
    void lateMax(unsigned pct) noexcept {latePct=std::clamp(pct,K::LateLo,K::LateHi);}
    // fg-gpu-max (v639): the GPU-time limit at K::GpuApart (diag-fg-gpu-max; default K::GpuMaxUs = the build before). Thor v638
    // (run xwfefhcu): three trips at 3154 .. 3183 us with late=0 lost=0 - every made picture on time - left the generator off.
    void gpuMax(double us) noexcept {gpuBase=std::clamp(us,K::GpuMaxLo,K::GpuMaxHi);}
    double gpuMax() const noexcept {return gpuBase;}
    unsigned lateMax() const noexcept {return latePct;}
    unsigned blindWindows() const noexcept {return blind;}          // full windows whose lateness could not be judged
    const Window& blindWindow() const noexcept {return blindSeen;}  // the last of them

    // A real frame arrived while the generator's switch is on. True when a cool-down just ended (the presenter logs the retry).
    bool frame() noexcept {
        if(state!=Cool)return false;
        if(cool>1){--cool;return false;}
        state=Run;cool=0;w=Window{};pending={};forgetInputs();
        return true;
    }
    // Once per real frame while the generator's switch is on and pairs may be planned. usable: this frame's inputs can be
    // used (a camera cut is usable: that frame is kept); madeBefore: the real frame before this one was shown with a made
    // picture in front of it. A refused frame after such a frame is an interruption.
    void inputs(bool usable,bool madeBefore) noexcept {
        if(state!=Run)return;
        ++frames;
        if(usable || !madeBefore)return;
        ++w.interrupted;
        std::uint64_t& oldest=interruptAt[interruptN++%interruptAt.size()]; // (the interruption InterruptMax - 1 before this one)
        if(oldest && frames-oldest<K::InterruptSpan)inputsTrip=true;
        oldest=frames;
    }
    // fg-late-relative (diag-fg-late-relative; the presenter says which rule holds before every verdict). On: a made picture
    // is judged by where it lands RELATIVE TO ITS OWN PAIR - late only when it is not on the screen before its real frame
    // (pairShown: spacing <= 0) - instead of by lane A's slot, which a display that shows every picture one or two refreshes
    // after its planned vsync misses for all of them while the pairs are spaced as planned. The lateness verdict, the blind
    // rule and the windows handed out (verdict's `seen`, window(), blindWindow()) then hold these numbers in shown / late.
    // Lost pictures, interruptions, GPU time and call time are judged as before. Off: nothing differs from the build before.
    void lateRelative(bool on) noexcept {relative=on;}
    // fg-60hz-late (v571; the iPad's ios-fg-late rule, patches/ios-claude/fg_gen.h.patch): on a 60 Hz panel (fg_pair.h gridFor:
    // the presenter says it before every verdict) a picture reaches the screen a constant refresh after lane A's planned vsync
    // (v570 on the Thor at 60 Hz: dpt_late for every pair, spacing exactly 1, the keeper fell back "late 100 %" three times and
    // stayed off). There late = shown at least half a refresh after the observed on-time latency: a running baseline of
    // (actual - target) that takes every new minimum at once and otherwise follows slowly (5 % per picture); a missed slot
    // still counts. On ~120 Hz (off): the rule of the build before, unchanged. A change of rule starts the baseline afresh.
    void lagBaseline(bool on) noexcept {if(on!=lagRule){lagRule=on;lagBase=-1;}}
    // fg-gpu-limit (v523). The spacing of the pairs being planned now, in refreshes (fg_pair.h apartFor; the presenter says it
    // before every verdict). The GPU-time limit is K::GpuMaxUs for pairs K::GpuApart refreshes apart - 3.0 ms of the 30 fps
    // cap's 33.3 ms frame - and the same share of a longer frame for pairs further apart: 4.5 ms at 3 refreshes (the 20 fps
    // cap), 6.0 ms at 4 (the 15 fps cap). Found on the Fold (run 23): under a held 15 fps cap its graphics chip, clocked down
    // because it has less to do, takes 3.1 to 3.2 ms for the five passes, and the fixed limit switched the generator off for
    // good while every made picture was shown on time. A spacing below K::GpuApart counts as K::GpuApart. When the spacing
    // changes, the window starts afresh (a window's means belong to one pace); a cool-down or a final fall-back stays.
    // Never called, or always with K::GpuApart (every build without diag-fg-low): nothing differs from the build before.
    void apart(unsigned refreshes) noexcept {
        const unsigned a=std::max(refreshes,K::GpuApart);
        if(a==apartNow)return;
        apartNow=a;
        if(state==Run){w=Window{};pending={};awaiting={};}
    }
    double gpuMaxUs() const noexcept {return gpuBase*double(apartNow)/double(K::GpuApart);} // (fg-gpu-max: gpuBase = K::GpuMaxUs without the file)
    // The pair whose FIRST picture is `id` has its second picture on the screen (fg_pair.h PairShown): spacing = refreshes
    // from the first picture's display time to the second's (-1: the first was not shown before it), apart = as planned.
    // Counted for made pictures only (planned() knew `id`); under either rule (only lateRelative() says which one decides).
    void pairShown(std::uint32_t id,int spacing,unsigned apart) noexcept {
        if(state!=Run || !id)return;
        for(auto& a:awaiting)if(a==id) {
            a=0;++w.rel;
            if(spacing<=0)++w.relLate;else if(unsigned(spacing)!=apart)++w.relOff;
            break;
        }
    }
    // A made picture was handed to the present thread: its id (the frame's tag), the vsync it belongs on, the refresh length.
    // target 0: the pair is spaced without a slot (no display timing): it counts, its lateness cannot be judged.
    void planned(std::uint32_t id,std::int64_t target,double period) noexcept {
        if(state!=Run)return;
        ++w.made;
        if(id)awaiting[awaitAt++%K::Ring]=id; // fg-late-relative
        if(id && target && period>0)pending[at++%K::Ring]=Pending{id,target,period};
    }
    // A picture reached the screen (VK_GOOGLE_display_timing).
    void shown(std::uint32_t id,std::int64_t actual) noexcept {
        if(state!=Run || !id)return;
        for(auto& p:pending)if(p.id==id) {
            ++w.shown;
            if(lagRule) { // fg-60hz-late
                const double dl=double(actual-p.target);
                if(lagBase<0 || dl<lagBase)lagBase=dl;else lagBase+=(dl-lagBase)*0.05;
                if(std::llround((dl-lagBase)/p.period)>0)++w.late;
            } else if(std::llround(double(actual-p.target)/p.period)>0)++w.late;
            p=Pending{};
            break;
        }
    }
    void lost() noexcept {if(state==Run){++w.lost;++w.made;}}            // a planned made picture was not presented
    void realLost() noexcept {if(state==Run)++w.lost;}                   // its real frame was not presented after it (the made picture is counted already)
    void gpu(double us) noexcept {if(state==Run && us>=0){w.gpuUs+=us;++w.gpuN;}}   // the five passes' GPU time
    void call(double us) noexcept {if(state==Run && us>=0){w.callUs+=us;++w.callN;}} // the made picture's call on the frame's thread
    // The verdict, asked once per real frame: a reason exactly when the keeper falls back NOW (*seen = the window that did it).
    // Lost pictures and interruptions are judged as they happen; everything else when a window of K::Window made pictures is
    // full. Lateness needs display times for at least half of the window (a full window with fewer is counted as blind), the
    // two means K::MinSamples samples.
    Trip verdict(Window* seen=nullptr) noexcept {
        if(state!=Run)return Trip::None;
        Trip t=Trip::None;
        if(w.lost>=K::LostMax)t=Trip::Lost;
        else if(inputsTrip)t=Trip::Inputs;
        else if(w.made>=K::Window) {
            const Window j=judged(); // fg-late-relative (w itself unless the relative rule is on)
            if(j.shown<K::Window/2){++blind;blindSeen=j;}
            if(j.shown>=K::Window/2 && j.lateOver(latePct))t=Trip::Late;
            else if(w.gpuN>=K::MinSamples && w.gpuMean()>gpuMaxUs())t=Trip::Gpu; // fg-gpu-limit: the limit of the pace in force
            else if(w.callN>=K::MinSamples && w.callMean()>K::CallMaxUs)t=Trip::Call;
            else w=Window{}; // (a picture still waiting for its display time counts in the next window)
        }
        if(t==Trip::None)return t;
        if(seen)*seen=judged();
        if(t==Trip::Inputs){state=Cool;cool=K::Cooldown;} // (never final, not one of the MaxTrips tries: see the top)
        else {
            ++tripCount;
            if(tripCount>=K::MaxTrips){state=Off;cool=0;}else {state=Cool;cool=K::Cooldown;}
        }
        w=Window{};pending={};forgetInputs();
        return t;
    }
    // The generator cannot run in this run at all (its objects could not be made).
    void disable() noexcept {state=Off;cool=0;w=Window{};pending={};forgetInputs();}
    // The switch went away: when it appears again the keeper starts afresh (the limit set by lateMax() is kept).
    void reset() noexcept {const unsigned pct=latePct;const double g=gpuBase;*this=SlotKeeper{};latePct=pct;gpuBase=g;} // (fg-gpu-limit: the presenter says the spacing again at the next frame)
    Window window() const noexcept {return judged();} // (fg-late-relative: by value)

private:
    struct Pending {std::uint32_t id=0;std::int64_t target=0;double period=0;};
    enum State : unsigned {Run=0,Cool,Off};
    State state=Run;unsigned cool=0,tripCount=0,latePct=K::LateMaxPct,at=0,blind=0;
    Window w,blindSeen;std::array<Pending,K::Ring> pending{};
    // the inputs' interruptions: the real-frame numbers of the last InterruptMax - 1 of them (0: none)
    std::uint64_t frames=0;std::array<std::uint64_t,K::InterruptMax-1> interruptAt{};unsigned interruptN=0;bool inputsTrip=false;
    void forgetInputs() noexcept {interruptAt={};interruptN=0;inputsTrip=false;}
    // fg-late-relative: the rule in force, the made pictures whose pair has not been seen on the screen yet, and the window as
    // the rule in force judges it (shown / late: against the slot, or - relative - of the pairs seen)
    bool relative=false;std::array<std::uint32_t,K::Ring> awaiting{};unsigned awaitAt=0;
    unsigned apartNow=K::GpuApart; // fg-gpu-limit
    double gpuBase=K::GpuMaxUs;    // fg-gpu-max
    bool lagRule=false;double lagBase=-1; // fg-60hz-late: the rule in force; the observed on-time display latency (ns; -1 = none yet)
    Window judged() const noexcept {Window j=w;if(relative){j.shown=w.rel;j.late=w.relLate;}return j;}
};

} // namespace recomp::fgg
