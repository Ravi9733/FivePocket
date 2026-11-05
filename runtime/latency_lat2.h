#pragma once
// latency-path package (2026-10-03): pure helpers of the second latency probe ("lat2"; tests/latency_lat2_test.cpp). Every
// caller uses them only while the v159 timeline is armed (files/private-trace/diag-timeline), so a run without an armed timeline
// executes v416's code plus one relaxed atomic load per site. Measurement only: nothing here reaches guest state or a frame.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace lat2 {
// VK_GOOGLE_display_timing's presentMargin is a uint64_t. Android's libvulkan (swapchain.cpp, TimingInfo::calculate) stores
// "composition latch - render complete" there: an int64_t that is NEGATIVE when the compositor latched the buffer before the
// GPU had finished it (SurfaceFlinger's unsignalled latch). v318 clamped the unsigned value (presentMargin / 100 us, max 65535),
// so every negative margin read as ">= 6.5 s". lat2 records the signed margin in 10 us units as an int16_t bit pattern;
// OutOfRange (the int16_t minimum) for |margin| > 327.67 ms.
constexpr std::uint16_t OutOfRange=0x8000;
constexpr std::uint16_t marginUnits(std::uint64_t margin) noexcept {
#if defined(LAT2_MUTANT_UNSIGNED) // test mutant: v318's unsigned reading (every negative margin is out of range)
    const std::int64_t s=margin>327670000ull?std::int64_t(327670001):std::int64_t(margin);
#else
    const std::int64_t s=static_cast<std::int64_t>(margin); // (two's complement: the libvulkan cast undone)
#endif
    if(s>327670000 || s<-327670000)return OutOfRange;
    return static_cast<std::uint16_t>(static_cast<std::int16_t>(s/10000)); // (truncates toward zero)
}
// The analysis side: the margin in 10 us units, or INT32_MIN for OutOfRange.
constexpr std::int32_t marginDecode(std::uint16_t arg) noexcept {
    return arg==OutOfRange?std::int32_t(-2147483647-1):std::int32_t(static_cast<std::int16_t>(arg));
}

// One reading thread's view of the pad: whether a read differs from the previous read made BY THE SAME THREAD. (v318's InputPoll
// compares with the previous read of ANY thread: the audio engine 64141000 reads the pad every few ms, so it takes the "first
// read" of almost every change and the game's own read of it is never marked.)
template<class S> struct ThreadView {
    S last{};bool have=false;
    template<class Same> bool update(const S& s,Same same) {
#if defined(LAT2_MUTANT_NO_UPDATE) // test mutant: the view never advances (every later read looks changed)
        const bool changed=have && !same(last,s);if(!have){last=s;have=true;}return changed;
#else
        const bool changed=have && !same(last,s);last=s;have=true;return changed;
#endif
    }
};
// The guest link register of the calling thread's latest XamInputGetState (set by the generated import bridge; InputRead's
// extra): the return address in the guest function that read the pad (e.g. inside sub_832262D8, the per-pad update that
// indexes the 84-byte pad objects at 0x83C00D74, or inside sub_83230E68), so a read can be tied to its guest call path.
inline thread_local std::uint32_t padReadLr=0;
// InputRead's arg: bit 0 this thread's view changed, bit 1 a synthetic step is active, bit 2 this read is v318's InputPoll (the
// first read of the change by any thread).
constexpr std::uint16_t readFlags(bool threadChanged,bool synthetic,bool firstOfChange) noexcept {
    return std::uint16_t((threadChanged?1u:0u)|(synthetic?2u:0u)|(firstOfChange?4u:0u));
}

// latency-path L1 (scheduling only): the Vulkan worker's nice level. Mode bit 0 (files/private-trace/diag-lat-worker-nice):
// -4 throughout; bit 1 (diag-lat-worker-boost): -4 from the render thread's queuePresent until the worker has finished that
// present item (the frame's backlog drain before vkQueuePresentKHR), 0 otherwise; neither: 0, as v416.
enum class WorkerPhase {Frame,PresentPending};
constexpr unsigned ModeAlways=1,ModeBoost=2;
constexpr int workerNice(unsigned mode,WorkerPhase phase) noexcept {
#if defined(LAT2_MUTANT_BOOST_ALWAYS) // test mutant: the boost also covers the rest of the frame
    return (mode&(ModeAlways|ModeBoost))?-4:0;
#else
    return (mode&ModeAlways)?-4:((mode&ModeBoost) && phase==WorkerPhase::PresentPending)?-4:0;
#endif
}
// Whether the worker must (re)apply its level after a present item: in boost mode the render thread raised it for the
// item, and when the mode changes the old level may still be in force.
constexpr bool workerReapply(unsigned mode,unsigned previousMode,int applied) noexcept {
    return (mode&ModeBoost) || (previousMode&ModeBoost) || workerNice(mode,WorkerPhase::Frame)!=applied;
}

// latency-path motion probe (files/private-trace/diag-lat-motion): the end-to-end check of WHICH displayed frame first shows
// a synthetic camera nudge. The presenter copies a band of rows across the middle of each armed frame's swapchain image; the
// watcher reduces it to one gray row at half width and estimates the horizontal shift against the previous watched frame.
// bandToRow: rows x width pixels (RGBA8 or BGRA8, alpha ignored) -> width/2 values, each the sum of a 2-pixel x rows block of
// (r + 2g + b) (so up to 1020 * 2 * rows fits 16 bits for rows <= 32).
inline void bandToRow(const std::uint8_t* px,unsigned width,unsigned rows,std::uint16_t* out) noexcept {
    const unsigned w2=width/2;
    for(unsigned x=0;x<w2;++x)out[x]=0;
    for(unsigned y=0;y<rows;++y) {
        const std::uint8_t* p=px+std::size_t(y)*width*4;
        for(unsigned x=0;x<w2;++x) {
            const std::uint8_t* a=p+8*x;
            out[x]=std::uint16_t(out[x]+a[0]+2*a[1]+a[2]+a[4]+2*a[5]+a[6]); // (r/b order does not matter: r + 2g + b)
        }
    }
}
// The shift s in [-maxShift, maxShift] that minimises the mean absolute difference between cur[x] and prev[x - s] (the picture
// moved right by s half-width pixels), over the x where both exist; bestMad16 / zeroMad16 are that minimum and the s = 0
// difference, in 1/16 units of the row's value scale. Ties keep the smallest |s| (a static picture reads 0).
struct Shift {int shift=0;std::uint32_t bestMad16=0,zeroMad16=0;};
inline Shift estimateShift(const std::uint16_t* prev,const std::uint16_t* cur,unsigned n,int maxShift) noexcept {
    Shift r;bool have=false;std::uint64_t best=0;
    for(int k=0;k<=2*maxShift;++k) {
        const int s=(k&1)?-((k+1)/2):(k/2); // 0, -1, 1, -2, 2, ...: smallest |s| first
#if defined(LAT2_MUTANT_SHIFT_SIGN) // test mutant: the sign of the motion flipped
        const int t=-s;
#else
        const int t=s;
#endif
        const unsigned lo=t>0?unsigned(t):0u,hi=t<0?n-unsigned(-t):n;
        if(hi<=lo+16)continue;
        std::uint64_t sad=0;
        for(unsigned x=lo;x<hi;++x){const int d=int(cur[x])-int(prev[x-t]);sad+=std::uint64_t(d<0?-d:d);}
        const std::uint64_t mad16=sad*16/(hi-lo);
        if(s==0)r.zeroMad16=std::uint32_t(mad16);
        if(!have || mad16<best){best=mad16;r.shift=s;have=true;}
    }
    r.bestMad16=std::uint32_t(best);
    return r;
}
// FrameShift's arg: the shift as int8 (clamped) in the low byte, the s = 0 difference (zeroMad16 / 16, max 255) high.
constexpr std::uint16_t shiftArg(const Shift& s) noexcept {
    const int c=s.shift<-127?-127:s.shift>127?127:s.shift;
    const unsigned z=s.zeroMad16/16>255?255u:s.zeroMad16/16;
    return std::uint16_t((std::uint8_t(std::int8_t(c)))|(z<<8));
}

// The presenter's GPU-done watcher hand-off, per acquire slot. Vulkan requires external synchronisation of a fence for
// vkResetFences / vkQueueSubmit / vkDestroyFence but not for vkWaitForFences: the presenter marks slot k watched after its
// submit and hands the fence to the watcher thread, which releases the slot once its vkWaitForFences (and timestamp read)
// returned; the presenter's next host access to slot k (waitAcquireSlot, before any reset, submit or destroy) first waits
// until the slot is released.
class SlotWatch {
public:
    static constexpr unsigned Slots=64;
    bool begin(unsigned k) noexcept {if(k>=Slots)return false;w[k].store(true,std::memory_order_release);return true;}
    void end(unsigned k) noexcept {if(k<Slots)w[k].store(false,std::memory_order_release);}
    bool watched(unsigned k) const noexcept {return k<Slots && w[k].load(std::memory_order_acquire);}
    // Yields until slot k is released (the watcher's own wait is bounded by its timeout).
    void waitReleased(unsigned k) const noexcept {
#if !defined(LAT2_MUTANT_NO_HANDSHAKE) // test mutant: the presenter does not wait for the watcher
        while(watched(k))std::this_thread::yield();
#else
        (void)k;
#endif
    }
private:
    std::array<std::atomic<bool>,Slots> w{};
};
}
