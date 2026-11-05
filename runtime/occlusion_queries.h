#pragma once
// occlusion package (2026-10-03, on v436): exact Xenos ZPD sample counts for GTA V's D3D occlusion queries, behind
// files/private-trace/diag-oc-queries (off: v436's fabricated counts, unchanged).
//
// What the 360 does (guest code read in the generated corpus; no Xenia code used):
//   Issue sub_82210888, type 9 (D3DQUERYTYPE_OCCLUSION). BEGIN: sub_822106C0(dev,q,1): RB_SAMPLE_COUNT_ADDR (0x2325) =
//   phys(word q+28) + 32 (endian bits 0), EVENT_WRITE_ZPD (PM4 0x5B, initiator 21 = ZPASS_DONE). END: 0xFFFFFEED into the
//   end block's ZPass A/B (+16/+20, big-endian; a CPU stw, or MEM_WRITE + WAIT_REG_MEM in the stream when the previous
//   result was not retrieved yet), then sub_822106C0(dev,q,0): address phys(word q+28), EVENT_WRITE_ZPD. At each event the
//   GPU writes its free-running sample counters there: 8 little-endian words Total, ZFail, ZPass, StencilFail (A, B each).
//   A query issued while tiling (word q+148 > 1) uses one block per tile (predicated SET_CONSTANT2 writes of 0x2325).
//   GetData sub_8225D5C8 (GTA's only caller sub_8225D570 asks for 4 bytes): once the query's fence passed and the LAST tile's
//   end ZPass A/B are not both the sentinel: result = sum over tiles of (endA + endB) - (beginA + beginB), 32-bit.
//   phys(va) = (va & 0x1FFFFFFF) + 0x1000 when va >= 0xE0000000 (the guest's own conversion).
// What v436 does: the retirement walker writes a fabricated counter (+0x1000 per event) into Total/ZPass A/B at each ZPD:
//   every query reports 8192 samples (the sun's lens flare behind walls, water always on screen).
// What this does:
//   hook (render thread, around Issue, D3D call order = PM4 order): a marker {id, key = end block physical, begin}, stamped
//     with the renderer pipeline's push count; no queue item: the renderer places it before the next draw item it runs;
//   renderer (worker, rendererMutex): every draw between a begin marker and its end marker runs inside a Vulkan precise
//     occlusion query; a marker's position = the number of counted draws before it; per-draw counts after the slot fence;
//   walker (CP thread, PM4 stream order): a ZPD event matched to its marker. BEGIN: the begin block gets X in the A halves,
//     0 elsewhere, and ZPassB = the sentinel's bytes, so endB - beginB = 0 while endB keeps D3D's sentinel. END: a store
//     placeholder (writes no bytes) that arms a report when the submission's stores are applied (sync or deferred, in
//     stream order); a store applied later into an armed unpublished report's 64-byte block cancels the report (the GPU
//     would have written the report before that store);
//   publication (CP thread, every retirement): an armed report whose draws all have counts: the end block's Total, ZFail
//     and StencilFail words, then ZPassA = X + D as one aligned 32-bit store, the readiness flip (ZPassB stays the
//     sentinel): GetData then reads exactly D. No torn read: only one word of the readiness pair changes.
//   Fallbacks (counted, RENDER_DIAG_OCQ): unmatched events, multi-tile queries, non-zero endian bits: v436's fabricated
//     stores; a report whose counts never arrive (lost draw, pool overflow, interval > 2048 draws, > 1 s): X + 0x2000
//     (v436's per-query count, "visible"), so that no query can stay unready forever (823649C8 re-issues the sun query
//     only after a ready result).
// Not modelled: Total/ZFail/StencilFail deltas (Total = ZPass, the others 0; only a GetData with >= 16 bytes reads them,
// which GTA never issues); clears inside a query interval (not counted).
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace recomp::gpu::ocq {
inline constexpr std::uint32_t Sentinel=0xFFFFFEEDu;          // D3D's "counting" value (stored big-endian)
inline constexpr std::uint32_t SentinelLe=0xEDFEFFFFu;        // its bytes read as a little-endian word
inline constexpr std::uint32_t FallbackSamples=0x2000u;       // v436's fabricated per-query count (ZPass A + B)
inline constexpr unsigned MarkerCap=1024,SearchAhead=8,PosRing=4096,DrawRing=8192,BeginCap=64;
#if defined(RECOMP_OCQ_TEST_TIMEOUT_MS) // host harness: a slow (sanitized) run must not time reports out
inline constexpr std::uint64_t IntervalDrawCap=2048,TimeoutMs=RECOMP_OCQ_TEST_TIMEOUT_MS,UnarmedDropMs=5*RECOMP_OCQ_TEST_TIMEOUT_MS;
#else
inline constexpr std::uint64_t IntervalDrawCap=2048,TimeoutMs=1000,UnarmedDropMs=5000;
#endif

inline std::atomic<bool> enabled{false}; // diag-oc-queries (the renderer's window poll; host tests set it)

inline std::uint32_t physicalOf(std::uint32_t va) {return (va&0x1FFFFFFFu)+(((va>>20)+0x200u)&0x1000u);}

// The begin block (8 words from block + 32) the walker stores at a matched BEGIN event, in put() mode 0 (little-endian).
#if defined(RECOMP_OCQ_MUTANT_BEGIN_B0) // host test mutant: begin ZPassB = 0 (results off by the sentinel's value)
inline std::array<std::uint32_t,8> beginWords(std::uint32_t x) {return {x,0u,0u,0u,x,0u,0u,0u};}
#else
inline std::array<std::uint32_t,8> beginWords(std::uint32_t x) {return {x,0u,0u,0u,x,SentinelLe,0u,0u};}
#endif

struct Stats {
    std::uint64_t marks=0,voided=0,placed=0,counted=0,results=0,lostDraws=0,runaways=0,
        matchedBegin=0,matchedEnd=0,unmatched=0,stale=0,voidedMatched=0,voidedSkipped=0,unmatchedEndFallback=0,reports=0,armed=0,published=0,
        fallbackLost=0,fallbackTimeout=0,cancelled=0,unarmedDropped=0,noBegin=0,samples=0;
    std::array<std::uint32_t,8> lastD{};unsigned lastCount=0;
};

class Engine {
public:
    struct Marker {std::uint64_t id=0,stamp=0;std::uint32_t key=0;bool begin=false,voided=false;};
    struct Match {enum Kind {None,Begin,End,Voided} kind=None;std::uint64_t id=0;std::uint32_t key=0;bool begin=false;};

    // ---- hook (the D3D thread, in Issue order): returns the marker id
    std::uint64_t mark(std::uint32_t key,bool begin,std::uint64_t stamp) {
        std::lock_guard l(m_);
        const Marker mk{++nextId_,stamp,key,begin,false};
        if(walker_.size()>=MarkerCap){walker_.pop_front();++s_.stale;}
        walker_.push_back(mk);
        if(worker_.size()>=MarkerCap)worker_.pop_front(); // (never in practice: the worker places them at its next draw)
        worker_.push_back(mk);workerPending_.store(worker_.size(),std::memory_order_release);
        ++s_.marks;bumpLive();
        return mk.id;
    }
    // A multi-tile issue (word q+148 > 1, one sample block per tile, the address set by predicated SET_CONSTANT2 writes the walker
    // does not follow): the worker ignores it; the walker gives its event (met at a stale address) v436's fabricated block, but
    // at the query's last tile block (lastKey), the one D3D's GetData checks, instead of the stale address (another query's).
    void voidMarker(std::uint64_t id,std::uint32_t lastKey) {
        std::lock_guard l(m_);++s_.voided;
        for(auto& mk:walker_)if(mk.id==id){mk.voided=true;mk.key=lastKey;}
        for(auto& mk:worker_)if(mk.id==id)mk.voided=true;
    }

    // ---- renderer worker (under rendererMutex): markers stamped <= executed (the pipeline items finished before the draw
    // about to run) are placed before it; UINT64_MAX places every pending marker (no pipeline / a synchronous draw).
    void placeUpTo(std::uint64_t executed) {
        if(!workerPending_.load(std::memory_order_acquire))return;
        std::lock_guard l(m_);
        while(!worker_.empty() && worker_.front().stamp<=executed) {
            const auto mk=worker_.front();worker_.pop_front();
            if(mk.voided)continue;
#if defined(RECOMP_OCQ_MUTANT_PLACE_EARLY) // host test mutant: an END placed one counted draw early
            pos_[mk.id%PosRing]={mk.id,!mk.begin && counted_?counted_-1:counted_,true};++s_.placed;
#else
            pos_[mk.id%PosRing]={mk.id,counted_,true};++s_.placed;
#endif
            auto it=std::find_if(openKeys_.begin(),openKeys_.end(),[&](const auto& e){return e.first==mk.key;});
            if(mk.begin) {
                if(!open_)openSince_=counted_;
                ++open_;
                if(it==openKeys_.end())openKeys_.push_back({mk.key,1u});else ++it->second;
            } else if(it!=openKeys_.end() && it->second) {
                --open_;if(!--it->second)openKeys_.erase(it);
            }
        }
        workerPending_.store(worker_.size(),std::memory_order_release);
    }
    bool counting() const noexcept {return open_!=0;}
    // The next counted draw (call only while counting()); an interval open for more than IntervalDrawCap draws is closed.
    std::uint64_t countDraw() {
        const auto seq=counted_++;
        std::lock_guard l(m_);
        draws_[seq%DrawRing]={seq,0,0};++s_.counted;
        if(counted_-openSince_>IntervalDrawCap){closeAll();++s_.runaways;}
        return seq;
    }
    void drawResult(std::uint64_t seq,std::uint64_t samples) {
        std::lock_guard l(m_);auto& d=draws_[seq%DrawRing];
        if(d.seq==seq && d.state==0){d.samples=samples;d.state=1;++s_.results;}
    }
    void drawLost(std::uint64_t seq) {
        std::lock_guard l(m_);auto& d=draws_[seq%DrawRing];
        if(d.seq==seq && d.state==0){d.state=2;++s_.lostDraws;}
    }
    void closeAll() noexcept {open_=0;openKeys_.clear();}
    std::uint64_t countedDraws() const noexcept {return counted_;}

    // ---- walker (CP thread, stream order)
    bool active() const noexcept {return live_.load(std::memory_order_relaxed)!=0 || enabled.load(std::memory_order_relaxed);}
    Match match(std::uint32_t sampleAddress) {
        std::lock_guard l(m_);
        const std::uint32_t a=sampleAddress&~3u;const std::size_t n=std::min<std::size_t>(walker_.size(),SearchAhead);
        for(std::size_t i=0;i<n;++i) {
            const auto& mk=walker_[i];
            if(mk.voided || a!=(mk.begin?mk.key+32u:mk.key))continue;
            const Match r{mk.begin?Match::Begin:Match::End,mk.id,mk.key,mk.begin};
            for(std::size_t k=0;k<i;++k)if(!walker_[k].voided)++s_.stale;else ++s_.voidedSkipped;
            walker_.erase(walker_.begin(),walker_.begin()+std::ptrdiff_t(i+1));
            bumpLive();
            return r;
        }
#if !defined(RECOMP_OCQ_MUTANT_VOID_STALE) // host test mutant: a multi-tile event keeps v436's stale address
        if(!walker_.empty() && walker_.front().voided) { // no real marker matches: the event of the voided head
            const auto mk=walker_.front();walker_.pop_front();++s_.voidedMatched;bumpLive();
            return {Match::Voided,mk.id,mk.key,mk.begin};
        }
#endif
        ++s_.unmatched;
        return {};
    }
    // A matched BEGIN: the X its begin block gets (beginWords).
    std::uint32_t begin(std::uint64_t id,std::uint32_t key) {
        std::lock_guard l(m_);
        xBase_=(xBase_+0x40u)&0x3FFFFFC0u;
        auto it=std::find_if(begins_.begin(),begins_.end(),[&](const auto& b){return b.key==key;});
        if(it!=begins_.end())*it={key,id,xBase_};
        else {if(begins_.size()>=BeginCap)begins_.erase(begins_.begin());begins_.push_back({key,id,xBase_});}
        ++s_.matchedBegin;bumpLive();
        return xBase_;
    }
    // A matched END: the report's index (the placeholder store's value), or nullopt when its BEGIN was not a real one.
    std::optional<std::uint32_t> end(std::uint64_t id,std::uint32_t key,std::uint64_t nowMs) {
        std::lock_guard l(m_);
        auto it=std::find_if(begins_.begin(),begins_.end(),[&](const auto& b){return b.key==key;});
        if(it==begins_.end()){++s_.noBegin;return std::nullopt;}
        Report r;r.index=++nextReport_;r.key=key;r.beginId=it->id;r.endId=id;r.x=it->x;r.createdMs=nowMs;
        begins_.erase(it);reports_.push_back(r);
        lo_.store(std::min(lo_.load(std::memory_order_relaxed),key),std::memory_order_relaxed);
        hi_.store(std::max(hi_.load(std::memory_order_relaxed),key+64u),std::memory_order_relaxed);
        pending_.fetch_add(1,std::memory_order_release);
        ++s_.matchedEnd;++s_.reports;bumpLive();
        return r.index;
    }
    // An unmatched event at `sampleAddress`: an END of a key with a real BEGIN gets the fallback count (returns its X); a
    // BEGIN re-issue forgets the old real BEGIN (the walker then stores v436's fabricated block).
    std::optional<std::uint32_t> unmatched(std::uint32_t sampleAddress) {
        std::lock_guard l(m_);const std::uint32_t a=sampleAddress&~3u;
        auto it=std::find_if(begins_.begin(),begins_.end(),[&](const auto& b){return b.key==a;});
        if(it!=begins_.end()){const auto x=it->x;begins_.erase(it);++s_.unmatchedEndFallback;bumpLive();return x;}
        auto ib=std::find_if(begins_.begin(),begins_.end(),[&](const auto& b){return b.key+32u==a;});
        if(ib!=begins_.end()){begins_.erase(ib);bumpLive();}
        return std::nullopt;
    }

    // ---- store application (any thread, in stream order)
    void arm(std::uint32_t index) {
        std::lock_guard l(m_);
        for(auto& r:reports_)if(r.index==index && !r.armed){r.armed=true;++s_.armed;break;}
    }
    // The placeholder of key's END was applied: its report is the oldest unarmed one of that key (placeholders apply in
    // stream order, reports are made in stream order).
    void armKey(std::uint32_t key) {
        std::lock_guard l(m_);
        for(auto& r:reports_)if(r.key==key && !r.armed){r.armed=true;++s_.armed;break;}
    }
    // A store applied at `physical` (word address): cancels every armed, unpublished report whose query block holds it.
    void touched(std::uint32_t physical) {
#if defined(RECOMP_OCQ_MUTANT_NO_CANCEL) // host test mutant: no cancellation
        return;
#endif
        // (fast path, no lock: no report pending, or the store outside every pending report's block; a report's range is
        // published before pending_ counts it, and both only shrink when the queue empties, under m_)
        if(!pending_.load(std::memory_order_acquire) || physical<lo_.load(std::memory_order_relaxed) || physical>=hi_.load(std::memory_order_relaxed))return;
        std::lock_guard l(m_);
        for(auto& r:reports_)if(r.armed && !r.cancelled && physical>=r.key && physical<r.key+64u) {
            r.cancelled=true;pending_.fetch_sub(1,std::memory_order_relaxed);++s_.cancelled;
        }
    }
    std::uint32_t pendingReports() const noexcept {return pending_.load(std::memory_order_relaxed);}

    // ---- publication (CP thread): put(physical, value) stores one little-endian word (put() mode 0, release).
    template<class Put> unsigned publish(Put&& put,std::uint64_t nowMs) {
        if(reportsEmpty_.load(std::memory_order_relaxed))return 0;
        std::lock_guard l(m_);unsigned n=0;
        while(!reports_.empty()) {
            auto& r=reports_.front();
            if(r.cancelled){reports_.pop_front();continue;}
#if defined(RECOMP_OCQ_MUTANT_UNARMED) // host test mutant: published before its stores were applied
            r.armed=true;
#endif
            if(!r.armed) {
                if(nowMs-r.createdMs<=UnarmedDropMs)break;
                ++s_.unarmedDropped;pending_.fetch_sub(1,std::memory_order_relaxed);reports_.pop_front();continue;
            }
            bool lost=false;const auto d=resolve(r,lost);
#if defined(RECOMP_OCQ_MUTANT_NO_FALLBACK) // host test mutant: a report waits for counts that never come
            if(!d)break;
#endif
            if(!d && !lost && nowMs-r.createdMs<=TimeoutMs)break; // (stream order: later reports wait)
            std::uint32_t samples=FallbackSamples;
            if(d)samples=*d;else if(lost)++s_.fallbackLost;else ++s_.fallbackTimeout;
            const std::uint32_t v=r.x+samples;
            for(const unsigned w:{0u,4u,8u,12u,24u,28u})put(r.key+w,w==0?v:0u); // Total A/B, ZFail A/B, StencilFail A/B
            put(r.key+16,v); // ZPass A last: GetData sees the result from here on (ZPass B keeps D3D's sentinel)
#if defined(RECOMP_OCQ_MUTANT_WRITE_B) // host test mutant: ZPass B written too (0)
            put(r.key+20,0u);
#endif
            if(d){s_.samples+=samples;s_.lastD[s_.lastCount++%s_.lastD.size()]=samples;}
            ++s_.published;++n;pending_.fetch_sub(1,std::memory_order_relaxed);reports_.pop_front();
        }
        bumpLive();
        return n;
    }
    Stats stats() {std::lock_guard l(m_);return s_;}
    std::size_t reportQueue() {std::lock_guard l(m_);return reports_.size();} // (host tests)

private:
    struct Report {std::uint32_t index=0,key=0,x=0;std::uint64_t beginId=0,endId=0,createdMs=0;bool armed=false,cancelled=false;};
    struct Pos {std::uint64_t id=0,pos=0;bool placed=false;};
    struct Draw {std::uint64_t seq=~0ull,samples=0;unsigned char state=0;}; // state 0 pending, 1 known, 2 lost
    struct Begin {std::uint32_t key=0;std::uint64_t id=0;std::uint32_t x=0;};
    // D: the samples of every counted draw between the two markers (nullopt: not known yet, or lost).
    std::optional<std::uint32_t> resolve(const Report& r,bool& lost) {
        const auto& pb=pos_[r.beginId%PosRing];const auto& pe=pos_[r.endId%PosRing];
        if(pb.id!=r.beginId || pe.id!=r.endId){lost=pb.id>r.beginId || pe.id>r.endId;return std::nullopt;} // (overwritten)
        if(!pb.placed || !pe.placed)return std::nullopt;
        if(pe.pos<pb.pos || pe.pos-pb.pos>IntervalDrawCap){lost=true;return std::nullopt;}
        std::uint64_t sum=0;
        for(auto s=pb.pos;s<pe.pos;++s) {
            const auto& d=draws_[s%DrawRing];
            if(d.seq!=s || d.state==2){lost=true;return std::nullopt;}
            if(d.state==0)return std::nullopt;
            sum+=d.samples;
        }
        return std::uint32_t(sum);
    }
    void bumpLive() {
        live_.store(walker_.size()+begins_.size()+reports_.size(),std::memory_order_relaxed);
        reportsEmpty_.store(reports_.empty(),std::memory_order_relaxed);
        if(reports_.empty()){lo_.store(~0u,std::memory_order_relaxed);hi_.store(0,std::memory_order_relaxed);}
    }
    std::mutex m_;
    std::deque<Marker> walker_,worker_;std::uint64_t nextId_=0;
    std::atomic<std::size_t> workerPending_{0},live_{0};std::atomic<bool> reportsEmpty_{true};
    std::array<Pos,PosRing> pos_{};std::array<Draw,DrawRing> draws_{};
    std::vector<Begin> begins_;std::uint32_t xBase_=0;
    std::deque<Report> reports_;std::uint32_t nextReport_=0;std::atomic<std::uint32_t> pending_{0},lo_{~0u},hi_{0};
    // worker-only (rendererMutex)
    std::uint64_t counted_=0,openSince_=0;unsigned open_=0;std::vector<std::pair<std::uint32_t,unsigned>> openKeys_;
    Stats s_;
};
inline Engine& engine(){static Engine e;return e;}
} // namespace recomp::gpu::ocq

namespace recomp::gpu::generic {
// generic_renderer.cpp: the renderer pipeline's push count (0 without a pipeline): the stamp of a marker made now.
std::uint64_t ocqStamp() noexcept;
}
