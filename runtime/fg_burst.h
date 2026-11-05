#pragma once
// fg-burst (frame generation, lane B, stage 1a; local-analysis/agents/frame-gen/DESIGN-stage1.md section 11): the capture
// "fg-burst". Our own code. This header is the PURE part (no Vulkan, no renderer state): the 8-frame ring's order, the switch
// state machine, the part files' format and writer, the frame record (the tonemap draw's camera constants, the depth target,
// the present and display times) and the small table the presenter fills with present-call and display times. The Vulkan part
// is fg_burst_renderer.inc (Renderer members). Host tests: local-analysis/agents/fg-burst/tests/.
//
// Switch files (files/private-trace/, polled at the renderer's 60-present window, as every diag-fg-* file):
//   diag-fg-burst      present: ARMED. From the next frame on the last 8 real frames are kept in GPU images (the present
//                      image as blitted, the main depth target as float32, the stencil at the frame's first depth resolve) with
//                      the tonemap draw's constants. Absent: nothing of this runs beyond one test of a cached flag per hook.
//                      A file that is ALREADY THERE at the run's first poll (left by an earlier run) does not arm: one line
//                      (left_over=1) says so, and it arms only after it was absent at a poll of this run.
//   diag-fg-burst-go   its APPEARANCE while armed (absent at one poll, present at a later one) is the trigger: the ring is
//                      frozen, read back one frame per real frame and written under files/private-trace/fgburst/c<id>/ in
//                      parts of <= 60,000 bytes; then one RENDER_DIAG_FG_BURST line and the capture disarms itself (its
//                      memory is released). It arms again only after diag-fg-burst was absent at a poll.
// No file is written unless the trigger was seen. A refusal (render scale other than 100 %, a missing pipeline, a failed
// allocation, no room on the disk) logs RENDER_DIAG_FG_BURST refused=... and leaves the game as it was.
//
// A part file: a 16-byte header, then at most 59,984 payload bytes. Header (little endian):
//   0  u32 magic  'F','G','B','1'
//   4  u16 capture (the capture's id; every part of one capture carries the same)
//   6  u8  frame   (0 = the oldest of the ring ... 7 = the newest)
//   7  u8  kind    (1 colour RGBA8 1280x720, 2 depth float32 1280x720, 3 stencil u8 1280x720, 4 the frame's record, text)
//   8  u16 part    (0-based)
//  10  u16 parts   (of this frame and kind)
//  12  u16 bytes   (payload bytes in this part)
//  14  u16 sum     (the payload's check value: sum over its bytes of byte x (number of bytes from it to the end), modulo
//                  65521; one changed byte or two exchanged unequal bytes always change it)
// Name: fgb-<capture as 4 hex digits>-f<frame>-k<kind>-p<part as 3 digits>.bin
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

namespace recomp::fgb {

inline constexpr unsigned Frames=8;                 // the ring: the last 8 real frames
inline constexpr unsigned Width=1280,Height=720;    // the present image; rows 0-719 of the main depth target
inline constexpr std::size_t Texels=std::size_t{Width}*Height;
inline constexpr std::size_t ColorBytes=Texels*4,DepthBytes=Texels*4,WordBytes=Texels*4,StencilBytes=Texels;
inline constexpr std::size_t PartLimit=60000,HeaderBytes=16,PayloadLimit=PartLimit-HeaderBytes;
inline constexpr std::uint32_t Magic=0x31424746u;   // the bytes 'F','G','B','1'
enum Kind : std::uint8_t {KindColor=1,KindDepth=2,KindStencil=3,KindMeta=4};
inline constexpr std::uint64_t TonemapShader=0x08dfab6c16ec854cull; // post_fidelity.h PostfxShaders[0], "tonemap"
inline constexpr unsigned CameraWords=36;           // pixel c8-c15 (32 words), then c130 (4 words)

inline std::int64_t monotonicNs() noexcept {timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return std::int64_t(t.tv_sec)*1000000000ll+t.tv_nsec;}

// ---- the ring's order. Slot = which of the 8 image sets a frame is stored in; a stored frame overwrites the oldest.
struct Ring {
    std::uint64_t count=0;                                            // frames stored since the capture was armed
    unsigned next() const noexcept {return unsigned(count%Frames);}   // the slot the frame being rendered will take
    void stored() noexcept {++count;}
    unsigned held() const noexcept {return unsigned(std::min<std::uint64_t>(count,Frames));}
    // the slot of the j-th oldest held frame (j = 0: the oldest; j = held() - 1: the newest)
    unsigned slotOf(unsigned j) const noexcept {
        return unsigned((count-held()+j)%Frames);
    }
};

// ---- a diag-fg-burst left on the device by an earlier run (a killed run, a forgotten file) must not arm the next run by
// itself: armed, the capture holds 84 MiB and adds work to every frame, which would silently change any later measurement.
// The renderer asks this at each poll until the capture was armed once in the run.
struct Launch {
    enum class Is : std::uint8_t {Absent,MayArm,LeftOverFirst,LeftOverStill};
    std::uint8_t seen=0; // 0: no poll yet; 1: the file was absent at a poll of this run; 2: it was there before any such poll
    Is poll(bool armFile) noexcept {
        if(!armFile){seen=1;return Is::Absent;}
        if(seen==1)return Is::MayArm;
        const bool first=seen==0;seen=2;
        return first?Is::LeftOverFirst:Is::LeftOverStill;
    }
};

// ---- the switch state machine (renderer worker thread only; driven by the 60-present window and by the renderer's results).
enum class State : std::uint8_t {Idle,Arming,Armed,Draining,Latched};
enum class Act : std::uint8_t {None,Arm,Cancel,Disarm,Trigger,Unlatch};
struct Control {
    State state=State::Idle;
    bool goSeen=false;   // diag-fg-burst-go was present at the previous poll
    bool goEdge=false;   // its appearance was seen while armed (the trigger waits for a full ring)
    bool goStale=false;  // it was already present when the capture was armed (it has to disappear first)
    bool on() const noexcept {return state!=State::Idle && state!=State::Latched;}
    // The 60-present window. armFile / goFile: whether the two files exist now; held: frames in the ring.
    Act window(bool armFile,bool goFile,unsigned held) noexcept {
        Act a=Act::None;
        switch(state) {
        case State::Idle:
            if(armFile){state=State::Arming;goEdge=false;goStale=goFile;a=Act::Arm;}
            break;
        case State::Arming: // (the renderer allocates at its next present; the file went away before that)
            if(!armFile){state=State::Idle;a=Act::Cancel;}
            break;
        case State::Armed:
            if(!armFile){state=State::Idle;goEdge=false;a=Act::Disarm;break;}
            if(goFile && !goSeen)goEdge=true;
            if(!goFile)goStale=false;
            if(goEdge && held>=Frames){state=State::Draining;goEdge=false;a=Act::Trigger;}
            break;
        case State::Draining: break; // (a started read-out finishes whatever the files do)
        case State::Latched:
            if(!armFile){state=State::Idle;a=Act::Unlatch;}
            break;
        }
        goSeen=goFile;
        return a;
    }
    void armed(bool ok) noexcept {if(state==State::Arming)state=ok?State::Armed:State::Latched;}
    void refused() noexcept {state=State::Latched;goEdge=false;}       // (a refusal at the trigger, or a failure while armed)
    void closed() noexcept {state=State::Latched;}
};

// ---- part files
struct Header {std::uint16_t capture=0;std::uint8_t frame=0,kind=0;std::uint16_t part=0,parts=0,bytes=0,sum=0;};
// A payload's check value: the running byte sums added up (= sum of byte i x (n - i)), modulo the prime 65521. A part has at
// most 59,984 bytes, so both factors of a difference (a byte's change below 256; a distance below the part's length) are
// nonzero modulo the prime: any ONE changed byte and any TWO exchanged unequal bytes change the value (a plain byte sum does
// not see an exchange). 64-bit sums cannot overflow below 2^27 bytes.
inline constexpr std::uint32_t CheckPrime=65521u;
inline std::uint16_t checksum(const std::uint8_t* p,std::size_t n) noexcept {
    std::uint64_t run=0,total=0;for(std::size_t i=0;i<n;++i){run+=p[i];total+=run;}
    return std::uint16_t(total%CheckPrime);
}
inline void packHeader(const Header& h,std::uint8_t* o) noexcept {
    const auto u16=[&](unsigned at,std::uint16_t v){o[at]=std::uint8_t(v&0xFF);o[at+1]=std::uint8_t(v>>8);};
    o[0]=std::uint8_t(Magic&0xFF);o[1]=std::uint8_t((Magic>>8)&0xFF);o[2]=std::uint8_t((Magic>>16)&0xFF);o[3]=std::uint8_t(Magic>>24);
    u16(4,h.capture);o[6]=h.frame;o[7]=h.kind;u16(8,h.part);u16(10,h.parts);u16(12,h.bytes);u16(14,h.sum);
}
inline unsigned partsFor(std::size_t bytes) noexcept {return unsigned((bytes+PayloadLimit-1)/PayloadLimit);}
struct PartName {char text[40];};
inline PartName partName(std::uint16_t capture,unsigned frame,unsigned kind,unsigned part) noexcept {
    PartName n{};std::snprintf(n.text,sizeof n.text,"fgb-%04x-f%u-k%u-p%03u.bin",unsigned(capture),frame,kind,part);return n;
}
// Writes one object (one frame's colour, depth, stencil or record) as its parts. sink(name, header16, payload, bytes) writes
// one file and returns false when it could not; the writer stops there. Returns the parts written (partsFor(bytes) = all).
template<class Sink> unsigned writeObject(Sink&& sink,std::uint16_t capture,unsigned frame,unsigned kind,const std::uint8_t* data,std::size_t bytes) {
    const unsigned parts=partsFor(bytes);
    for(unsigned p=0;p<parts;++p) {
        const std::size_t at=std::size_t{p}*PayloadLimit;
        const std::size_t n=std::min<std::size_t>(PayloadLimit,bytes-at);
        Header h;h.capture=capture;h.frame=std::uint8_t(frame);h.kind=std::uint8_t(kind);h.part=std::uint16_t(p);h.parts=std::uint16_t(parts);
        h.bytes=std::uint16_t(n);
        h.sum=checksum(data+at,n);
        std::uint8_t head[HeaderBytes];packHeader(h,head);
        if(!sink(partName(capture,frame,kind,p).text,head,data+at,n))return p;
    }
    return parts;
}
// The stencil of a depth word picture (RGBA8, R = the word's bits 0-7 = the stencil; generic_depth_word.hlsl).
inline void stencilOfWords(const std::uint8_t* words,std::uint8_t* out,std::size_t texels) noexcept {
    for(std::size_t i=0;i<texels;++i)out[i]=words[i*4];
}

// ---- the device's sink: one file per part in the capture's directory, created exclusively (an existing file is never
// overwritten or truncated) and written with write(2). A part is at most 60,000 bytes: the app's process limits every file
// to 64 KB (RLIMIT_FSIZE; generic_renderer.cpp clog()).
static_assert(PartLimit<=60000 && HeaderBytes+PayloadLimit==PartLimit,"a part file stays below the 64 KB file limit");
struct DirSink {
    char dir[200]{};unsigned files=0,failures=0;std::uint64_t bytes=0;int lastErrno=0;
    bool operator()(const char* name,const std::uint8_t* head,const std::uint8_t* data,std::size_t n) noexcept {
        char path[256];
        if(std::snprintf(path,sizeof path,"%s/%s",dir,name)>=int(sizeof path)){++failures;return false;}
        const int fd=::open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
        if(fd<0){lastErrno=errno;++failures;return false;}
        const bool ok=all(fd,head,HeaderBytes) && all(fd,data,n);
        if(!ok)lastErrno=errno;
        if(::close(fd)!=0 || !ok){++failures;return false;}
        ++files;bytes+=HeaderBytes+n;return true;
    }
    static bool all(int fd,const std::uint8_t* p,std::size_t n) noexcept {
        while(n){const auto w=::write(fd,p,n);if(w<0){if(errno==EINTR)continue;return false;}if(w==0)return false;p+=w;n-=std::size_t(w);}
        return true;
    }
};
// The capture's own directory <base>/c<id as 4 hex digits>, made new: an id whose directory exists is skipped (8 tries), so
// parts of two captures never share a directory and nothing of an earlier capture is touched. False: no directory.
inline bool makeCaptureDir(const char* base,std::uint16_t wanted,std::uint16_t* id,char* dir,std::size_t cap) noexcept {
    if(::mkdir(base,0700)!=0 && errno!=EEXIST)return false;
    for(unsigned k=0;k<8;++k) {
        std::uint16_t c=std::uint16_t(wanted+k);if(!c)c=1;
        if(std::snprintf(dir,cap,"%s/c%04x",base,unsigned(c))>=int(cap))return false;
        if(::mkdir(dir,0700)==0){*id=c;return true;}
        if(errno!=EEXIST)return false;
    }
    return false;
}
inline bool roomFor(const char* base,std::uint64_t bytes) noexcept {
    struct statvfs s{};
    return ::statvfs(base,&s)==0 && std::uint64_t(s.f_bavail)*std::uint64_t(s.f_frsize)>=bytes;
}

// ---- one frame's record
struct Camera { // the tonemap draw of the frame being rendered whose constants were taken (reset at every present)
    std::uint32_t draws=0;        // tonemap draws whose constants were taken in this frame. In the renderer this is 0 or 1: its
                                  // hook runs for the frame's FIRST tonemap draw only (the block of the RENDER_POSTFX line).
                                  // How many tonemap draws the frame had is Meta::tonemapDraws (the design measured exactly 1)
    std::uint32_t atDraw=0;       // the frame's draw number of the last one taken (1 = the frame's first draw)
    std::uint64_t frame=0;        // the renderer's frame counter at that draw
    std::array<std::uint32_t,CameraWords> words{}; // pixel c8-c15, c130 (float bits)
};
// The tonemap draw's pixel constants: registers c8-c15 (words 32-63) and c130 (words 520-523).
inline void takeCamera(Camera& c,const std::array<std::uint32_t,1024>& pixel,std::uint64_t frame,std::uint32_t atDraw) noexcept {
    std::memcpy(c.words.data(),pixel.data()+8*4,32*sizeof(std::uint32_t));
    std::memcpy(c.words.data()+32,pixel.data()+130*4,4*sizeof(std::uint32_t));
    ++c.draws;c.atDraw=atDraw;c.frame=frame;
}
struct Meta {
    std::uint16_t capture=0;unsigned index=0,frames=0;        // filled at the read-out: position in the capture (0 = oldest)
    std::uint64_t present=0;                                  // the renderer's present serial (consecutive real frames differ by 1)
    std::uint64_t rendererFrame=0;                            // the renderer's frame counter at the present
    std::uint32_t tag=0;                                      // the queued present's tag (0: an unqueued present)
    bool color=false,depth=false,stencil=false;
    std::uint32_t draws=0,resolves=0;                         // draws / resolves of the frame
    std::uint32_t tonemapDraws=0;                             // tonemap draws of the frame (the renderer's own count, RENDER_POSTFX draws=)
    // the depth target read at the present: the rule "newest write stamp among depth targets of pitch 1280"
    unsigned depthBase=0,depthFormat=0,depthPitch=0,depthWidth=0,depthHeight=0,depthCandidates=0,depthPending=0;int depthVkFormat=0;
    std::uint64_t depthStamp=0,base0Stamp=0;bool depthResidue=false,depthIsStencilSource=false;
    // the stencil: the source of the frame's first depth resolve from a depth target of pitch 1280
    unsigned stencilBase=0,stencilFormat=0,stencilPitch=0;std::uint32_t stencilAtDraw=0,stencilAtResolve=0;
    Camera camera;
    // times (CLOCK_MONOTONIC ns; 0 = not known): the present call's begin and return, the display time (the pacer's feedback)
    std::int64_t callBeginNs=0,callEndNs=0,displayNs=0;
    // lane A's plan of the frame, when it was paced: its slot (vsync), the cap in refreshes per frame, the refresh estimate
    bool paced=false;std::int64_t slotNs=0;unsigned capRefreshes=0;double periodNs=0;
};
// The record as text: "key=value" lines; floats with 9 significant digits (exact for binary32) and their bits in hex.
inline std::size_t formatMeta(char* out,std::size_t cap,const Meta& m) noexcept {
    std::size_t n=0;
    const auto put=[&](const char* f,auto... a){if(n<cap){const int k=std::snprintf(out+n,cap-n,f,a...);if(k>0)n=std::min(cap,n+std::size_t(k));}};
    put("fgburst=1\ncapture=%04x\nframe=%u\nframes=%u\n",unsigned(m.capture),m.index,m.frames);
    put("width=%u\nheight=%u\n",Width,Height);
    put("present=%llu\nrenderer_frame=%llu\ntag=%u\n",(unsigned long long)m.present,(unsigned long long)m.rendererFrame,m.tag);
    put("color=%u\ndepth=%u\nstencil=%u\ncamera=%u\n",unsigned(m.color),unsigned(m.depth),unsigned(m.stencil),unsigned(m.camera.draws!=0));
    put("draws=%u\nresolves=%u\n",m.draws,m.resolves);
    put("depth_key=%u:%u:%u\ndepth_size=%ux%u\ndepth_vkformat=%d\ndepth_stamp=%llu\ndepth_base0_stamp=%llu\ndepth_candidates=%u\n"
        "depth_pending_clears=%u\ndepth_residue=%u\ndepth_is_stencil_source=%u\n",m.depthBase,m.depthFormat,m.depthPitch,m.depthWidth,m.depthHeight,
        m.depthVkFormat,(unsigned long long)m.depthStamp,(unsigned long long)m.base0Stamp,m.depthCandidates,m.depthPending,unsigned(m.depthResidue),
        unsigned(m.depthIsStencilSource));
    put("stencil_key=%u:%u:%u\nstencil_at_draw=%u\nstencil_at_resolve=%u\n",m.stencilBase,m.stencilFormat,m.stencilPitch,m.stencilAtDraw,m.stencilAtResolve);
    put("tonemap_draws=%u\ntonemap_at_draw=%u\ntonemap_frame=%llu\n",m.tonemapDraws,m.camera.atDraw,(unsigned long long)m.camera.frame);
    for(unsigned r=0;r<9;++r) {
        const unsigned reg=r<8?8+r:130;float v[4];std::memcpy(v,m.camera.words.data()+r*4,sizeof v);
        put("c%u=%.9g,%.9g,%.9g,%.9g\n",reg,double(v[0]),double(v[1]),double(v[2]),double(v[3]));
        put("c%ux=%08x,%08x,%08x,%08x\n",reg,m.camera.words[r*4],m.camera.words[r*4+1],m.camera.words[r*4+2],m.camera.words[r*4+3]);
    }
    put("call_begin_ns=%lld\ncall_end_ns=%lld\ndisplay_ns=%lld\n",(long long)m.callBeginNs,(long long)m.callEndNs,(long long)m.displayNs);
    put("paced=%u\nslot_ns=%lld\ncap_refreshes=%u\nperiod_ns=%.3f\n",unsigned(m.paced),(long long)m.slotNs,m.capRefreshes,m.periodNs);
    return n;
}

// ---- present-call and display times, by tag (the presenter's thread writes: Presenter::fgDesired / fgCalled / fgDisplayed, the
// places fg_pair.h and pace_caps.h already hear of them; the renderer reads at the read-out). The newest 64 pictures (the 8
// frames of the ring, the 10 or so presents of the read-out, twice that when pairs are shown).
struct Times {
    struct Rec {std::uint32_t tag=0;std::int64_t begin=0,end=0,shown=0;};
    static constexpr unsigned N=64;
    void begin(std::uint32_t tag,std::int64_t ns) noexcept {
        if(!tag)return;
        std::lock_guard l(m);
        Rec* r=find(tag);
        if(!r){r=&recs[next++%N];*r=Rec{};r->tag=tag;}
        r->begin=ns;
    }
    void end(std::uint32_t tag,std::int64_t ns) noexcept {std::lock_guard l(m);if(Rec* r=find(tag))r->end=ns;}
    void shown(std::uint32_t tag,std::int64_t ns) noexcept {
        std::lock_guard l(m);
        if(Rec* r=find(tag);r && !r->shown)r->shown=ns;
    }
    Rec get(std::uint32_t tag) noexcept {std::lock_guard l(m);const Rec* r=find(tag);return r?*r:Rec{};}
    void clear() noexcept {std::lock_guard l(m);recs={};next=0;}
private:
    Rec* find(std::uint32_t tag) noexcept {if(tag)for(auto& r:recs)if(r.tag==tag)return &r;return nullptr;}
    std::mutex m;std::array<Rec,N> recs{};unsigned next=0;
};
inline std::atomic<bool> timesOn{false}; // the capture is armed: the presenter's three hooks record (else: one load each)
inline Times& times() noexcept {static Times t;return t;}
[[gnu::noinline]] inline void noteBegin(std::uint32_t tag) noexcept {times().begin(tag,monotonicNs());}
[[gnu::noinline]] inline void noteEnd(std::uint32_t tag) noexcept {times().end(tag,monotonicNs());}
[[gnu::noinline]] inline void noteShown(std::uint32_t id,std::uint64_t actualNs) noexcept {times().shown(id,std::int64_t(actualNs));}
// The presenter's hooks (direct_present.h): one relaxed load and a branch unless a capture is armed.
inline void presentBegin(std::uint32_t tag) noexcept {if(timesOn.load(std::memory_order_relaxed))noteBegin(tag);}
inline void presentEnd(std::uint32_t tag) noexcept {if(timesOn.load(std::memory_order_relaxed))noteEnd(tag);}
inline void displayed(std::uint32_t id,std::uint64_t actualNs) noexcept {if(timesOn.load(std::memory_order_relaxed))noteShown(id,actualNs);}

} // namespace recomp::fgb
