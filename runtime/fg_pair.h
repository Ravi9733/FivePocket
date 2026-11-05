#pragma once
// frame-gen (lane B, frame generation; 2026-10-04), stage 1a: the PAIR logic. Our own code.
//
// While the option is on and its gate is open, every real frame is shown as TWO pictures two refreshes apart on the ~120 Hz
// panel: "between" = [the made picture, the real frame], "ahead" = [the real frame, the made picture]. In stage 1a the made
// picture is trivial (between: the previous real picture; ahead: the same real picture again), so the screen looks exactly as
// with the 30 fps cap alone; what runs for real is everything else: the gate, the second acquire, the second scaler chain, the
// two present calls and their timing. Stage 1b replaces only the recorder of the made picture (DESIGN-stage1.md).
// Switch files (files/private-trace/, polled by generic_renderer.cpp fgWindow() every 60 presents; none present = no decision
// anywhere differs from the build without this lane):
//   diag-fg-pair-between / diag-fg-pair-ahead   the two orders (both present: between)
//   diag-fg-hold    (modifier) space the pair with a timed wait of the present thread instead of desired present times
//   diag-fg-nocap   (modifier, diagnostic) do not require lane A's 30 fps cap (pairs of 2+1 refreshes then occur)
//   diag-fg-low     (modifier; fg-low, 2026-10-10) pairs also under lane A's caps of 6 and 8 refreshes per frame (diag-pace-20 /
//                   diag-pace-15 at ~120 Hz), their two pictures HALF THE FRAME apart (3 / 4 refreshes: 20 shown as 40, 15
//                   shown as 30), the rate gate by the phase rule below for the cap in force. Without this file every
//                   decision of this file is the one of the build before fg-low, for every input (the lane's differential
//                   test runs both headers side by side); with it, a frame under the 4-refresh cap is decided as before too.
//
// The gate (review defect 1: the first rule, "last interval 3.5-6.5 refreshes, 8 in a row, works uncapped", was open for 6-27 %
// of the measured uncapped driving frames): pairs need a real frame of 4 refreshes, which only lane A's 30 fps cap gives. So
//   (1) the frame's slot must come from lane A's pacer with 4 refreshes per frame (slotSource; no pacer or another cap: closed,
//       reason "cap"), unless diag-fg-nocap;
//   (2) the last GateRun real intervals were all GateLo..GateHi refreshes (under the cap the lower bound never binds; the
//       upper one closes the gate for GateRun frames after a stall).
// tests/fg_pair_test.cpp replays the measured v458 driving and house interval sequences through this file.
//
// Which picture carries the frame's tag: always the pair's FIRST picture (it is the one on the frame's slot, so lane A's slot
// feedback, the v337 lag rings and the latency probe keep meaning "the frame reached the screen"); the second picture carries
// an id of its own that can never evict a pending real frame from the 64-entry rings (review: (tag + 32) | bit 31 shared a ring
// slot with a real frame within three frames in 7.5 % of frames).
//
// Pure: every time is passed in (nanoseconds on CLOCK_MONOTONIC, the clock of actualPresentTime); no lock of its own (the
// presenter calls everything except setSwitches() / switches() under its mutex).
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <time.h>

namespace recomp::fg {

constexpr unsigned SwBetween=1u,SwAhead=2u,SwHold=4u,SwNoCap=8u,SwModes=SwBetween|SwAhead;
constexpr unsigned SwLow=64u; // fg-low: diag-fg-low (bits 16 and 32 of the presenter's word are fg_gen.h's)
// fg-late-relative: diag-fg-late-relative. Nothing in this file reads it: the presenter hands it to fg_gen.h's slot keeper, which
// then judges a made picture by where it lands relative to its own pair (PairShown below) instead of by lane A's slot.
constexpr unsigned SwLateRelative=128u;
enum Mode : unsigned {Off=0,Between=1,Ahead=2};
// Why a real frame got no second picture (counted per reason; RENDER_DIRECT_FG skip=).
enum Reason : unsigned {Shown=0,NotWanted,NoCap,Rate,Failed,SecondLost,Reasons};

struct K {
    static constexpr double GateLo=2.5,GateHi=6.5;          // refreshes per real frame (replay: tests/fg_pair_test.cpp)
    static constexpr unsigned GateRun=8;                    // real intervals in range, in a row, before the gate opens
    static constexpr unsigned CapRefreshes=4;               // lane A's 30 fps cap on the 8.397 ms grid
    static constexpr unsigned Apart=2;                      // refreshes between the two pictures of a pair
    static constexpr double NominalPeriod=8'333'333.0;      // the grid's first guess; tracked within +-5 % (the Thor: 8.397 ms)
    static constexpr std::int64_t LatchLead=9'800'000;      // present call returned -> its vsync (lane A's measured window)
    static constexpr std::int64_t FirstLead=6'600'000;      // present call begins -> first vsync it can make (review/display-cadence.txt: right for 89 % of frames)
    static constexpr std::int64_t CallInit=1'100'000;       // vkQueuePresentKHR, p50 (v458 run); then measured
    static constexpr std::int64_t HoldMax=30'000'000;       // a hold never lasts longer (a broken estimate must not park a picture)
    static constexpr float PhaseLo=0.33f,PhaseHi=0.67f;     // between: (I - 2) / I for I = 3..6 refreshes; ahead: 2 / I
    static constexpr unsigned Ring=64;                      // the tag rings of lag_v337.h and pace_caps.h
    static constexpr unsigned Recent=4,SpacingBins=6;
};

// fg-60hz (v570, 2026-10-21; the iPad's 60 Hz pairing, patches/ios-claude/fg_pair.h.patch): the pair constants of the panel's
// grid, chosen at run time by the refresh length lane A's pacer measures on the swapchain (Slot::period), not by platform.
// ~120 Hz (the Thor, the Fold): K's values, every decision as before. ~60 Hz (a panel held at 60 Hz, the iPad mini): lane A's
// 30 fps cap is 2 refreshes per real frame, the two pictures of a pair 1 refresh apart, and the rate gate keeps the same TIMES as
// the 120 Hz one (2.5 .. 6.5 refreshes of 8.33 ms = 1.25 .. 3.25 of 16.67 ms). The 60 Hz grid has no fg-low caps.
struct Grid {unsigned capRefreshes,apart;double gateLo,gateHi,nominalPeriod;};
inline constexpr Grid Grid120{K::CapRefreshes,K::Apart,K::GateLo,K::GateHi,K::NominalPeriod};
inline constexpr Grid Grid60{2u,1u,1.25,3.25,16'666'667.0};
// A refresh length of 12.5 ms or more (80 Hz and slower) is the 60 Hz grid; unknown (0) or shorter: the 120 Hz grid.
constexpr const Grid& gridFor(double periodNs) noexcept {return periodNs>=12'500'000.0?Grid60:Grid120;}
constexpr bool is60(const Grid& g) noexcept {return g.capRefreshes==Grid60.capRefreshes && g.apart==Grid60.apart;}

inline std::int64_t monotonicNs() noexcept {timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return std::int64_t(t.tv_sec)*1000000000ll+t.tv_nsec;}
// v572: the pairs' second pictures handed to the present call (the presenter counts them; the frame-rate counter, fps_overlay.h,
// reads the increase per window to show the displayed rate).
inline std::atomic<std::uint64_t> secondPresents{0};
// pace-fg-tuning (v460's first device run): the desired present time that puts a picture on the vsync `vsync`. The Thor's
// compositor shows a timed picture at the first vsync LATER THAN desired - one refresh: v460 asked for vsync - half a refresh
// and 7,029 of 7,152 second pictures came one refresh after the first instead of two (the other 123 followed a first picture
// that was itself a refresh early). The middle of the window that selects `vsync` is vsync + half a refresh. The same formula
// as lane A's recomp::pace::desiredFor (pace_caps.h; this file stays free of that include; tests/tuning_test.cpp compares them).
inline std::int64_t desiredFor(std::int64_t vsync,double period) noexcept {return vsync+std::int64_t(period/2);}

// Lane A's plan of a frame: the vsync it is paced for, the cap in refreshes per frame and the period estimate. All zero: unknown.
struct Slot {std::int64_t target=0;unsigned refreshes=0;double period=0;};
// Set once by the merged build (MERGE.md: six lines beside lane A's pacer). Null: no pacer in this build.
using SlotFn=bool(*)(std::uint32_t tag,Slot* out) noexcept;
inline std::atomic<SlotFn> slotSource{nullptr};

// ---- fg-low (2026-10-10): the caps below the 4-refresh one. Three rules, each a function of the cap in force:
// The caps (refreshes per real frame) that pairs take besides K::CapRefreshes while diag-fg-low is present: 6 and 8.
constexpr bool lowCap(unsigned capRefreshes,unsigned sw) noexcept {return (sw&SwLow)!=0 && (capRefreshes==6u || capRefreshes==8u);}
// Refreshes between the two pictures of a pair: half the frame under those caps (3 / 4), so both pictures are shown equally
// long; K::Apart for every other frame (the 4-refresh cap, diag-fg-nocap, and everything without diag-fg-low).
constexpr unsigned apartFor(unsigned capRefreshes,unsigned sw,const Grid& g=Grid120) noexcept {
    return is60(g)?g.apart:lowCap(capRefreshes,sw)?capRefreshes/2u:K::Apart; // (fg-60hz: the 60 Hz grid has no low caps)
}
// The rate gate's range for pictures `apart` refreshes apart, by the PHASE RULE: a pair is allowed while the made picture's
// phase (interval - apart) / interval stays within 1/3 .. 2/3, i.e. for whole intervals from 1.5 x apart (rounded up) to
// 3 x apart refreshes; the bounds lie half a refresh outside the first and the last of them. For apart = K::Apart this is
// K::GateLo .. K::GateHi exactly (asserted here; the range the 4-refresh cap has had since stage 1a); 4.5 .. 9.5 for
// apart = 3 (the 6-refresh cap), 5.5 .. 12.5 for apart = 4 (the 8-refresh cap).
constexpr double gateLoFor(unsigned apart) noexcept {return double((3u*apart+1u)/2u)-0.5;}
constexpr double gateHiFor(unsigned apart) noexcept {return double(3u*apart)+0.5;}
static_assert(gateLoFor(K::Apart)==K::GateLo && gateHiFor(K::Apart)==K::GateHi,"the phase rule gives the 4-refresh cap's gate");
// One real interval after another: true while pairs may be shown. `apart`: the spacing of the cap in force (fg-low).
struct Gate {
    unsigned run=0;
    bool step(double refreshes,unsigned apart=K::Apart,const Grid& g=Grid120) noexcept {
        const double lo=is60(g)?g.gateLo:gateLoFor(apart),hi=is60(g)?g.gateHi:gateHiFor(apart); // (fg-60hz: the same times as at 120 Hz)
        run=(refreshes>=lo && refreshes<=hi)?std::min(run+1u,K::GateRun):0u;
        return run>=K::GateRun;
    }
    bool open() const noexcept {return run>=K::GateRun;}
};
// (1) of the gate: lane A's cap is the 4-refresh one, or the diagnostic switch waives it; fg-low: or it is 6 or 8 refreshes
// and diag-fg-low is present.
constexpr bool capAllows(unsigned capRefreshes,unsigned sw,const Grid& g=Grid120) noexcept {
    return capRefreshes==g.capRefreshes || (sw&SwNoCap)!=0 || (!is60(g) && lowCap(capRefreshes,sw));
}
// Where the made picture lies. between: its share of the way from the previous real frame to this one when it is shown `apart`
// refreshes before this one; ahead: how far past this frame, in real intervals, a picture shown `apart` refreshes later lies.
inline float phaseFor(double interval,unsigned apart=K::Apart) noexcept {
    return interval>0?std::clamp(float((interval-double(apart))/interval),K::PhaseLo,K::PhaseHi):0.5f;
}
inline float stepFor(double interval,unsigned apart=K::Apart) noexcept {
    return interval>0?std::clamp(float(double(apart)/interval),K::PhaseLo,K::PhaseHi):0.5f;
}
// The second picture's id: bit 31 opposite to the frame tag's, a ring slot (id % Ring) that none of `recent` real tags uses
// (they may still be waiting for their display time in a ring), never 0.
inline std::uint32_t secondIdFor(std::uint32_t tag,std::uint32_t sequence,const std::array<std::uint32_t,K::Recent>& recent) noexcept {
    unsigned slot=(tag+K::Ring/2)%K::Ring;
    for(unsigned tries=0;tries<=K::Recent;++tries) {
        bool taken=tag%K::Ring==slot;
        for(const auto r:recent)taken=taken || (r && r%K::Ring==slot);
        if(!taken)break;
        slot=(slot+1)%K::Ring;
    }
    return ((tag^0x80000000u)&0x80000000u)|((sequence&0x1FFFFFFu)<<6)|slot;
}

struct Plan {
    Mode mode=Off;Reason reason=NotWanted;
    std::uint32_t secondId=0;      // the pair's second picture (0: no pair)
    bool hold=false;               // the present thread spaces the pair (no desired present times)
    bool timed=false;              // pace-fg-tuning: both pictures carry a desired present time (lane A then plans the frame as timed)
    double interval=0;             // this real frame's interval in refreshes
    float phase=0.5f,step=0.5f;    // stage 1b's inputs (logged in 1a)
};

// One RENDER_DIRECT_FG line's counters (since the last take()) and the state at its end.
struct Snapshot {
    unsigned switches=0,gateRun=0,cap=0;bool grid=false;double periodNs=0;
    std::uint64_t real=0,pairs=0,between=0,ahead=0,held=0,overrun=0,idClash=0;
    std::array<std::uint64_t,Reasons> skip{};
    std::array<std::uint64_t,K::SpacingBins> spacing{}; // refreshes from a pair's first picture to its second (last bin: more)
    std::array<std::uint64_t,K::SpacingBins+2> realGap{}; // refreshes between consecutive FIRST pictures (last bin: more)
    std::uint64_t holdNs=0,lagFirstNs=0,lagFirstN=0,lagSecondNs=0,lagSecondN=0,dptLate=0;
    double phaseSum=0;
    // fg-low (appended; counters only): the pair spacing of the cap in force at the end (refreshes; apartFor), `spacing`'s
    // last bin (5 or more) by length - 5, 6, 7, 8 or more - and `realGap`'s last bin (7 or more) by length - 7 .. 13, 14 or more.
    unsigned apart=0;
    std::array<std::uint64_t,4> spacingLong{};
    std::array<std::uint64_t,8> realGapLong{};
    bool grid60=false; // fg-60hz (appended): the 60 Hz grid's constants were in force at the end
};

// fg-late-relative: what a display time said about a pair (Pairs::displayed's result). second: the picture was a live pair's
// SECOND picture; then first = that pair's first picture (the frame's tag), apart = the spacing the pair was planned with, and
// spacing = the refreshes from the first picture's display time to this one (the number `spacing=` counts), or -1 when the
// first picture has no earlier display time: it was not on the screen before its partner (lost, replaced, or shown after it).
struct PairShown {bool second=false;std::uint32_t first=0;unsigned apart=0;int spacing=0;};

class Pairs {
public:
    // ---- any thread
    void setSwitches(unsigned bits) noexcept {sw.store(bits,std::memory_order_relaxed);}
    unsigned switches() const noexcept {return sw.load(std::memory_order_relaxed);}

    // ---- under the presenter's mutex from here on.
    // A real frame arrives at the presenter (before any of its pictures is presented). `timed`: display times and desired
    // present times are available; `thread`: a present thread exists (a hold is possible); `usable`: the made picture's objects
    // exist and have not failed.
    Plan real(std::uint32_t tag,std::int64_t now,const Slot& s,bool timed,bool thread,bool usable) noexcept {
        const unsigned w=switches();
        Plan p;
        const double P=s.period>0?s.period:period;
        // fg-60hz: lane A's measured refresh length picks the grid; a change restarts our own vsync estimate and the gate there
        if(s.period>0 && &gridFor(s.period)!=grid){grid=&gridFor(s.period);period=grid->nominalPeriod;gridSamples=0;gate.run=0;}
        const Grid& g=*grid;
        const unsigned apart=apartFor(s.refreshes,w,g); // fg-low: K::Apart unless diag-fg-low and a cap of 6 or 8 refreshes
        // the interval: between the two frames' slots when lane A planned both (exact refreshes), else on our own clock
        double interval=0;
        if(lastNow) interval=(s.target && lastTarget)?double(s.target-lastTarget)/P:double(now-lastNow)/P;
        const bool open=lastNow?gate.step(interval,apart,g):false;
        lastNow=now;lastTarget=s.target;lastCap=s.refreshes;
        p.interval=interval;p.phase=phaseFor(interval,apart);p.step=stepFor(interval,apart);
        ++c.real;
        recent[recentAt++%K::Recent]=tag;
        for(auto& r:pairs)if(r.live && r.second==tag){r.live=false;++c.idClash;} // (a real tag equal to a live second id: that pair is forgotten)
        if(!(w&SwModes))p.reason=NotWanted;
        else if(!capAllows(s.refreshes,w,g))p.reason=NoCap;
        else if(!open)p.reason=Rate;
        else if(!usable)p.reason=Failed;
        else {
            p.mode=(w&SwBetween)?Between:Ahead;p.reason=Shown;
            p.secondId=secondIdFor(tag,sequence++,recent);
            // desired present times need the frame's slot and display timing; otherwise (or on request) the present thread holds
            const bool dpt=timed && s.target && !(w&SwHold);
            p.hold=!dpt && thread;p.timed=dpt;
            auto& r=pairs[pairAt++%pairs.size()];
            r=Pair{};r.live=true;r.first=tag;r.second=p.secondId;r.dpt=dpt;r.hold=p.hold;r.period=P;r.apart=apart;
            r.target=dpt?s.target:0;
        }
        if(p.mode==Off)++c.skip[p.reason];
        else {++c.pairs;++(p.mode==Between?c.between:c.ahead);c.phaseSum+=double(p.mode==Between?p.phase:p.step);}
        return p;
    }
    // The planned pair's second picture was not presented (its acquire or its pass failed): the frame is a single picture.
    void secondLost(std::uint32_t tag) noexcept {
        for(auto& r:pairs)if(r.live && r.first==tag){r.live=false;++c.skip[SecondLost];} // (pairs counts planned pairs)
    }
    // ---- present thread (under the presenter's mutex). The desired present time of `id`; `base` is what the build would pass
    // without this lane (0, or lane A's value for the frame's tag). Also the moment the picture's present call begins.
    std::uint64_t desired(std::uint32_t id,std::uint64_t base,std::int64_t now) noexcept {
        if(!anyLive())return base;
        for(auto& r:pairs) {
            if(!r.live)continue;
            if(r.first==id) {
                r.firstCall=now;
                // both pictures of a timed pair carry a time: the first cannot be shown before the frame's slot
                return r.dpt?std::max<std::uint64_t>(base,std::uint64_t(desiredFor(r.target,r.period))):base;
            }
            if(r.second==id) {
                r.secondCall=now;
                return r.dpt?std::uint64_t(desiredFor(r.target+std::int64_t(double(r.apart)*r.period),r.period)):0;
            }
        }
        return base;
    }
    // The present call of `id` returned at `now`. True when `id` is a pair's second picture (the caller then keeps it out of
    // the v337 call ring: its deliberate two refreshes are not display lag).
    bool called(std::uint32_t id,std::int64_t now) noexcept {
        if(!anyLive())return false;
        for(auto& r:pairs)if(r.live && r.second==id) {
            if(r.secondCall && now>r.secondCall)callNs+=(std::clamp<std::int64_t>(now-r.secondCall,100'000,8'000'000)-callNs)/8;
            return true;
        }
        return false;
    }
    // Before the present call of `id`: when it is a held pair's second picture, the time its call should begin (false: present now).
    bool holdUntil(std::uint32_t id,std::int64_t now,std::int64_t* until) noexcept {
        if(!anyLive())return false;
        for(auto& r:pairs)if(r.live && r.hold && r.second==id && r.firstCall) {
            const std::int64_t apart=std::int64_t(double(r.apart)*r.period);
            // with a grid: the first picture's vsync + the pair's refreshes, minus the latch lead and the call itself;
            // without: that many refreshes after the first call began (equal spacing on our own clock)
            std::int64_t t=gridOk(now)?vsyncAtOrAfter(r.firstCall+K::FirstLead)+apart-K::LatchLead-callNs:r.firstCall+apart;
            // (fg-low: a pair further apart than K::Apart may be held that many refreshes longer; nothing more otherwise)
            t=std::min(t,now+K::HoldMax+std::int64_t((double(r.apart)-double(grid->apart))*r.period)); // (fg-60hz: 0 at the grid's own spacing)
            if(t<=now)return false;
            *until=t;++c.held;r.holdFrom=now;return true;
        }
        return false;
    }
    // The hold of `id` ended at `now`; `superseded`: a newer real frame arrived first (the picture goes out at once, late).
    void holdEnded(std::uint32_t id,std::int64_t now,bool superseded) noexcept {
        for(auto& r:pairs)if(r.live && r.second==id && r.holdFrom){c.holdNs+=std::uint64_t(std::max<std::int64_t>(0,now-r.holdFrom));r.holdFrom=0;}
        if(superseded)++c.overrun;
    }
    // `id` reached the screen at `actual` (VK_GOOGLE_display_timing). (fg-late-relative: the result; no decision here reads it)
    PairShown displayed(std::uint32_t id,std::int64_t actual) noexcept {
        PairShown out;
        gridSample(actual);
        if(!anyLive() && !lastFirstShown)return out;
        for(auto& r:pairs) {
            if(!r.live)continue;
            if(r.first==id) {
                r.firstShown=actual;
                if(r.firstCall && actual>r.firstCall){c.lagFirstNs+=std::uint64_t(actual-r.firstCall);++c.lagFirstN;}
                if(r.dpt && std::llround(double(actual-r.target)/r.period)>0)++c.dptLate;
                if(lastFirstShown && actual>lastFirstShown) {
                    const long long gap=std::llround(double(actual-lastFirstShown)/period);
                    ++c.realGap[std::size_t(std::clamp<long long>(gap,0,K::SpacingBins+1))];
                    if(gap>=K::SpacingBins+1)++c.realGapLong[std::size_t(std::min<long long>(gap-(K::SpacingBins+1),7))]; // fg-low (a counter only)
                }
                lastFirstShown=actual;
            } else if(r.second==id) {
                out.second=true;out.first=r.first;out.apart=r.apart;out.spacing=-1; // fg-late-relative
                if(r.firstShown && actual>=r.firstShown) {
                    const long long apart=std::llround(double(actual-r.firstShown)/r.period);
                    out.spacing=int(std::min<long long>(apart,1'000'000)); // fg-late-relative
                    ++c.spacing[std::size_t(std::clamp<long long>(apart,0,K::SpacingBins-1))];
                    if(apart>=K::SpacingBins-1)++c.spacingLong[std::size_t(std::min<long long>(apart-(K::SpacingBins-1),3))]; // fg-low (a counter only)
                }
                if(r.secondCall && actual>r.secondCall){c.lagSecondNs+=std::uint64_t(actual-r.secondCall);++c.lagSecondN;}
                r.live=false;
            }
        }
        return out;
    }
    // The pairs are over (the switch went away, the swapchain was replaced): nothing stays planned.
    void reset() noexcept {for(auto& r:pairs)r=Pair{};lastFirstShown=0;}
    Snapshot take() noexcept {
        Snapshot s=c;c=Snapshot{};
        s.switches=switches();s.gateRun=gate.run;s.cap=lastCap;s.grid=gridSamples>=4;s.periodNs=period;
        s.apart=apartFor(lastCap,s.switches,*grid); // fg-low
        s.grid60=is60(*grid); // fg-60hz
        return s;
    }
    bool gateOpen() const noexcept {return gate.open();}

private:
    struct Pair {
        bool live=false,dpt=false,hold=false;std::uint32_t first=0,second=0;
        std::uint32_t apart=K::Apart; // fg-low: refreshes from the pair's first picture to its second (in the padding behind `second`: the record keeps its size)
        std::int64_t target=0,firstCall=0,secondCall=0,firstShown=0,holdFrom=0;double period=K::NominalPeriod;
    };
    std::atomic<unsigned> sw{0};
    Gate gate;
    const Grid* grid=&Grid120; // fg-60hz: the grid of lane A's last measured refresh length
    std::int64_t lastNow=0,lastTarget=0,lastFirstShown=0,callNs=K::CallInit;unsigned lastCap=0;
    std::array<std::uint32_t,K::Recent> recent{};unsigned recentAt=0;std::uint32_t sequence=1;
    std::array<Pair,4> pairs{};unsigned pairAt=0;
    // our own vsync estimate (hold mode and the spacing counters; lane A's is used whenever its slot is known)
    double period=K::NominalPeriod;std::int64_t phase=0,lastSample=0;unsigned gridSamples=0;
    Snapshot c;

    bool anyLive() const noexcept {for(const auto& r:pairs)if(r.live)return true;return false;}
    bool gridOk(std::int64_t now) const noexcept {return gridSamples>=4 && now-lastSample<1'500'000'000;}
    std::int64_t vsyncAtOrAfter(std::int64_t t) const noexcept {return phase+std::int64_t(std::ceil(double(t-phase)/period)*period);}
    // Display times lie on the panel's grid: the newest one is the phase; the period follows whole-refresh gaps slowly.
    void gridSample(std::int64_t d) noexcept {
        if(!gridSamples || d-lastSample>2'000'000'000){phase=d;lastSample=d;gridSamples=1;return;}
        if(d<=lastSample)return;
        const double k=std::round(double(d-lastSample)/period);
        if(k>=1.0) {
            const double per=double(d-lastSample)/k;
            if(std::fabs(per-period)<0.3*period/k) {
                period=std::clamp(period+(per-period)*std::max(0.05,1.0/double(gridSamples)),0.95*grid->nominalPeriod,1.05*grid->nominalPeriod);
                ++gridSamples;
            }
        }
        phase=d;lastSample=d;
    }
};

} // namespace recomp::fg
