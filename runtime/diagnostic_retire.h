#pragma once
// ===== DIAGNOSTIC SKIP BUILD ONLY =====
// Attempt v44: the last-resort retirement of a graphics submission that no native handler accepted
// (formerly inline in trace_runner.cpp, diagnostic_skip::retire), moved to a header so a host test
// can drive it against a real GuestImportRegistry. Nothing is drawn. Only CPU-observable words are
// written (SHD/MEM_WRITE values, FABRICATED ZPD counts), tile-command IRQs run through the qualified
// handoff, then the read pointer advances. Output after this point is wrong by construction.
//
// Behaviour is v36's retire() except where marked v44:
//  - The registry mutex R is held only where registry state is touched: once to snapshot the ring
//    configuration, once to validate and publish (readWord, RPTR writeback, completion stores). The
//    ring segment and its indirect buffers are walked with R released. Publication revalidates the
//    snapshot (same ring, same writeback, readWord unchanged); a change fails closed exactly as v36's
//    `readWord!=previous` check did. v36 held R for the whole PM4 walk (up to ~18k words per IB).
//  - No per-word allocation or std::function: each buffer is read once into one owned byte vector and
//    walked as big-endian words; tile IRQ (35-word) and EndTiling tail (43-word) windows are parsed in
//    place (std::span) behind their parsers' own first-word test. v36 copied two vectors per IB word.
//  - The RPTR writeback and the scratch/extent stores that v36 wrote byte by byte are one aligned
//    release store each (put(...,2)), so a poller in another permit domain never sees a torn word and
//    sees every earlier store of the submission before the RPTR.
//  - Optional per-call timing (RetireTiming).
// Callers: the CP_RB_WPTR handler only, inside the graphics-submission exclusion (one caller at a
// time), so the namespace-scope state below is confined to the current submitter.
#include "diag_log_sample.h" // v218
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include "recomp_app_dir.h" // data-paths: the app's files directory.
#include <unistd.h>
#include <android/log.h>

#include "guest_address_space.h"
#include "guest_import_registry.h"
#include "gta5_tile_scene_continuation.h"
#include "occlusion_queries.h" // occlusion package: diag-oc-queries (exact ZPD sample counts; off: v436's fabricated ones)

namespace recomp::bootstrap { class GuestCooperativeExecutor; }

namespace recomp::bootstrap { void noteNativeSubmissionMayCommit() noexcept; void noteNativeSubmissionMayCommitAt(unsigned site) noexcept; } // v183 (guest_import_registry.cpp)
namespace diagnostic_skip {
using Registry=recomp::bootstrap::GuestImportRegistry;
struct RingTag {using type=std::optional<Registry::GraphicsRingConfiguration> Registry::*;friend type get(RingTag);};
struct MutexTag {using type=std::mutex Registry::*;friend type get(MutexTag);};
// v44: the physical arena mapping used by resolvePhysicalRange (bound before workers start).
struct ArenaBaseTag {using type=std::uint32_t Registry::*;friend type get(ArenaBaseTag);};
struct ArenaSizeTag {using type=std::uint32_t Registry::*;friend type get(ArenaSizeTag);};
struct ArenaOffsetTag {using type=std::uint32_t Registry::*;friend type get(ArenaOffsetTag);};
template<class Tag,typename Tag::type M> struct Rob {friend typename Tag::type get(Tag){return M;}};
template struct Rob<RingTag,&Registry::graphicsRingConfiguration_>;
template struct Rob<MutexTag,&Registry::mutex_>;
template struct Rob<ArenaBaseTag,&Registry::physicalArenaBase_>;
template struct Rob<ArenaSizeTag,&Registry::physicalArenaSize_>;
template struct Rob<ArenaOffsetTag,&Registry::physicalAddressOffset_>;
inline recomp::bootstrap::GuestCooperativeExecutor* executor=nullptr;
inline recomp::gpu::DrawBinState currentBins; // Kick bin words seen before the IRQ being run.
// Attempt v1: scratch routing registers as retired kicks write them, seeded from the engine's
// retained registers whenever those change (a qualified path executed a newer write).
inline std::uint32_t scratchAddr=0,scratchMask=0,retainedScratchAddr=UINT32_MAX,retainedScratchMask=UINT32_MAX;
inline bool tailSkipped=false;inline std::uint32_t skippedTailContext=0; // v689: set by the dispatch for a second-pass tail.
inline std::uint32_t skippedTailScratch=0; // v700: the tail request's own scratch block (physical).

struct Stats {unsigned long long scratch=0,packets=0,ibs=0,draws=0,viz=0,zpd=0,shd=0,memWrites=0,interrupts=0,waits=0,regToMem=0,unknown=0,noops=0,ibFailures=0,pfd=0,type1=0,ext=0;};
inline unsigned long long retired=0;inline std::uint32_t zpdCounter=0;inline std::set<std::string> profiles;
inline void put(std::uint8_t* p,std::uint32_t v,unsigned mode) {
    const std::uint8_t b[4]{std::uint8_t(v),std::uint8_t(v>>8),std::uint8_t(v>>16),std::uint8_t(v>>24)};
    static constexpr unsigned order[4][4]{{0,1,2,3},{1,0,3,2},{3,2,1,0},{2,3,0,1}};
    // Attempt v30: fence/completion words are polled by guest threads in other permit domains
    // (resource busy checks). Publish an aligned word with one release store, so a poller never sees
    // a torn value (byte stores could show e.g. 0x22FF between 0x21FF and 0x2200) and sees every
    // earlier write of this submission first. Unaligned words keep the byte stores.
    // v44: mode 2 (8-in-32 swap) is the plain big-endian word; the RPTR, scratch and extent stores use it.
    std::uint8_t out[4];
    for(unsigned i=0;i<4;++i)out[i]=b[order[mode&3][i]];
    if(!(reinterpret_cast<std::uintptr_t>(p)&3)) {
        std::uint32_t word;std::memcpy(&word,out,4);
        __atomic_store_n(reinterpret_cast<std::uint32_t*>(p),word,__ATOMIC_RELEASE);
        return;
    }
    for(unsigned i=0;i<4;++i)p[i]=out[i];
}
using Interrupt=std::function<bool(std::uint32_t,const std::vector<std::uint8_t>&)>;

// v44: where one retirement spends its time (all fields optional reporting).
struct RetireTiming {
    std::uint64_t lockWaitNs=0,lockHoldNs=0,lockHoldMaxNs=0; // both R sections (snapshot, publish)
    std::uint64_t walkNs=0,irqNs=0;                          // walk with R released; IRQ phase + bin replay
    std::uint64_t words=0;unsigned ibs=0,irqs=0;
};

// v48c: an EndTiling tail (43 words, Gta5TileSceneContinuation) split across two indirect buffers.
// D3D's recording segments end at packet boundaries anywhere in the tail. Run omos8dhm (v51) froze
// when a tile replay IB ended two words short of its tail: that kick retired interrupts=1 waits=5
// scratch=4 tile_irqs=0 (every recognised tail kick has tile_irqs=1), the next kick carried a single
// packet, no continuation IRQ ran, and the tile worker waited on its event while the render thread
// spun on fence 0x1c1 (the v1/v6/v21/v23 signature). The CP raises the tail's interrupt when it
// executes the INTERRUPT packet (tail word 29): with it in the earlier IB the continuation runs with
// that IB (the words after it are fixed by the tail's own scratch block and context), else with the
// next depth-0 IB, whose first words must complete the tail. Callers are serialized (see above).
// files/private-trace/diag-tile-tail-contiguous-only restores v47 (43 contiguous words only).
struct SplitTail {
    std::uint32_t physical=0;             // the tail's first word
    std::vector<std::uint32_t> prefix;    // carried: its words in the earlier IB (interrupt not reached)
    std::vector<std::uint32_t> remainder; // delivered: the words the next IB is expected to begin with
    recomp::gpu::DrawBinState bins{};     // carried: the kick bins in effect at the tail's first word
};
inline std::optional<SplitTail> splitTail;
inline unsigned splitTailLogs=0;
// The tail EndTiling appends (gta5_tile_scene_continuation.h, gta5_tile_command_interrupt.h): mask
// reset, first-pass mask, WAIT_UNTIL, the predicated 35-word request, mask reset. Word 8 is the
// request's context and word 13 its scratch block | 2; no earlier word depends on either.
inline std::array<std::uint32_t,43> tileTailTemplate(std::uint32_t scratch,std::uint32_t context) {
    constexpr std::uint32_t cb=recomp::gpu::Gta5TileCommandInterrupt::Callback,mask=recomp::gpu::Gta5TileCommandInterrupt::CpuMask;
    constexpr std::uint32_t wait=0xC0043C01u,all=0xFFFFFFFFu;
    return {0xC0006000u,all,0xC0006000u,0x80000000u,0x5C8u,0x20000u,
            0x1057Cu,cb,context,0x578u,mask,
            wait,0x13u,scratch+0*4+2,mask,all,0x100u,
            wait,0x13u,scratch+5*4+2,context,all,0x100u,
            wait,0x13u,scratch+4*4+2,cb,all,0x100u,
            0xC0005401u,mask,0x45Eu,mask,
            wait,0x13u,scratch+0*4+2,0u,all,0x100u,
            0x57Cu,0x0BADF00Du,0xC0006000u,all};
}
inline bool splitTailsEnabled() {
    static unsigned checks=0;static bool enabled=true;
    if(!(checks++&63)) {
        const bool now=access(RECOMP_APP_PATH("/private-trace/diag-tile-tail-contiguous-only"),F_OK)!=0;
        if(now!=enabled || checks==1)__android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_TILE_TAIL_SPLIT_MODE split=%u",unsigned(now));
        enabled=now;
    }
    return enabled;
}

namespace detail {
using Clock=std::chrono::steady_clock;
inline std::uint32_t be32(const std::uint8_t* p) noexcept {
    return (std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3];
}
inline std::uint64_t ns(Clock::time_point from,Clock::time_point to) noexcept {
    const auto d=std::chrono::duration_cast<std::chrono::nanoseconds>(to-from).count();
    return d>0?static_cast<std::uint64_t>(d):0;
}
struct Store {std::uint32_t address,value;bool virt=false;std::uint8_t ocq=0;}; // occlusion: 1 = a report placeholder, 2 = its filler
using Ranges=std::vector<std::array<unsigned,3>>; // {first,end,irq index}

// The v36 walk over one owned big-endian word buffer. Reads cannot fail (the buffer is owned and
// the bounds checks below precede every read), so the v36 read-failure returns become unreachable.
struct Walk {
    recomp::bootstrap::GuestAddressSpace& space;
    std::uint32_t writeWord;
    std::vector<Store>& stores;
    std::vector<std::pair<std::uint32_t,std::vector<std::uint8_t>>>& irqs;
    std::vector<std::size_t>& irqStoreAt;
    std::vector<recomp::gpu::DrawBinState>& irqBins;
    recomp::gpu::DrawBinState& kickBins;
    std::vector<std::array<std::uint32_t,3>>& shdSeen;
    std::vector<std::array<std::uint32_t,2>>& walkedIbs;
    Stats& s;
    std::set<unsigned>& unknownOps;
    std::uint64_t& words;
    std::uint32_t sampleAddress=0;
    const std::uint8_t* carryBuffer=nullptr;std::uint32_t carryAt=0; // v48c: where a carried tail starts

    // Attempt v3: one register-write path for type-0 and type-1 packets.
    // SCRATCH_UMSK (0x1DC) and SCRATCH_ADDR (0x1DD) route SCRATCH_REGn: with bit n of UMSK
    // set, the CP stores the value big-endian at physical SCRATCH_ADDR+4n (the behaviour
    // Xenia's command processor models). The block below the RPTR writeback is D3D's fence
    // block (device+11024), written by EVENT_WRITE_SHD; attempt v1 stopped sending scratch
    // writes there (v683..v701 did, clobbering it).
    bool regWrite(unsigned reg,std::uint32_t v,std::uint32_t i,const Ranges& irqRanges) {
        if(reg==0x2325)sampleAddress=v;
        if(reg==0x1DC || reg==0x1DD) {
            (reg==0x1DC?scratchMask:scratchAddr)=v;
            static unsigned logged=0;
            if(logged++<32)__android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
                "RENDER_DIAG_SCRATCH_REG write=%u reg=%03x value=%08x",writeWord,reg,v);
        }
        if(reg>=0x578 && reg<=0x57F) {
            ++s.scratch;
            // A tile IRQ sequence's own scratch words are performed by its handoff.
            const bool inIrq=std::any_of(irqRanges.begin(),irqRanges.end(),[&](const auto& r){return i>=r[0] && i<r[1];});
            const unsigned slot=reg-0x578;
            if(!inIrq && ((scratchMask>>slot)&1u)) {
                if(!scratchAddr || std::uint64_t{scratchAddr}+slot*4+4>0x20000000u)return false;
                stores.push_back({0xA0000000u+scratchAddr+slot*4,v,true});
            }
        }
        return true;
    }

    bool run(const std::uint8_t* bytes,std::uint32_t n0,unsigned depth,const Ranges& irqRanges) {
        const auto read=[&](std::uint32_t k){return be32(bytes+std::size_t{k}*4);};
        words+=n0;
        std::uint32_t i=0;
        while(i<n0) {
            for(const auto& r:irqRanges)if(i==r[0] && r[2]<irqStoreAt.size()){irqStoreAt[r[2]]=stores.size();irqBins[r[2]]=kickBins;}
            if(bytes==carryBuffer && i==carryAt && splitTail)splitTail->bins=kickBins;
            const std::uint32_t h=read(i);
            // Attempt v3: 0 and 0x0BADF00D are one-word no-ops (as Xenia's command processor
            // treats them); parsing 0 as a type-0 write swallowed the next packet.
            if(h==0 || h==0x0BADF00Du){++s.noops;++i;continue;}
            const unsigned type=h>>30;
            if(type==2){++i;continue;}
            ++s.packets;
            if(type==1) { // Attempt v3: two register writes (index h&0x7FF, then (h>>11)&0x7FF).
                ++s.type1;
                if(std::uint64_t{i}+3>n0)return false;
                const std::uint32_t a=read(i+1),b=read(i+2);
                if(!regWrite(h&0x7FF,a,i,irqRanges) || !regWrite((h>>11)&0x7FF,b,i,irqRanges))return false;
                i+=3;continue;
            }
            const std::uint32_t n=((h>>16)&0x3FFF)+1;
            if(std::uint64_t{i}+1+n>n0)return false;
            std::array<std::uint32_t,3> d{};
            for(unsigned k=0;k<n && k<3;++k)d[k]=read(i+1+k);
            if(type==0) {
                const unsigned base=h&0x7FFF;const bool one=h&0x8000;
                for(unsigned k=0;k<n;++k) {
                    const unsigned reg=one?base:base+k;
                    if(reg==0x2325 || reg==0x1DC || reg==0x1DD || (reg>=0x578 && reg<=0x57F)) {
                        if(!regWrite(reg,read(i+1+k),i,irqRanges))return false;
                    }
                }
            } else switch((h>>8)&0x7F) {
            case 0x3F: case 0x37: { // Attempt v3: INDIRECT_BUFFER_PFD (0x37) runs like an IB.
                ++s.ibs;if(((h>>8)&0x7F)==0x37)++s.pfd;
                d[0]&=0x1FFFFFFFu; // Attempt v3: the CP masks IB pointers to 29 bits.
                if(depth>=4 || n<2){++s.ibFailures;break;}
                const auto at=0xA0000000u+d[0];const auto size=d[1]&0xFFFFF;
                // v44: one owned copy of the IB (v36 loaded it word by word into the same bytes, then
                // walked guest memory a second time); the IRQ scan and the walk read this one copy.
                std::vector<std::uint8_t> ib(std::size_t{size}*4);
                if(size)std::memcpy(ib.data(),space.translate(at),ib.size());
                Ranges ranges;
                const auto ibWord=[&](unsigned k){return be32(ib.data()+std::size_t{k}*4);};
                const auto tailBytes=[](const auto& words){std::vector<std::uint8_t> b;b.reserve(43*4);
                    for(const std::uint32_t w:words)for(unsigned j=0;j<4;++j)b.push_back(std::uint8_t(w>>(24-8*j)));return b;};
                unsigned scanFrom=0;unsigned carriedIrq=~0u;recomp::gpu::DrawBinState carriedBins{};
                if(depth==0 && splitTail) { // v48c: the rest of a tail an earlier IB ended inside
                    auto carry=std::move(*splitTail);splitTail.reset();
                    if(!carry.remainder.empty()) {
                        const unsigned n=unsigned(carry.remainder.size());bool match=size>=n;
                        for(unsigned k=0;match && k<n;++k)match=ibWord(k)==carry.remainder[k];
                        if(match){ranges.push_back({0,n,~0u});scanFrom=n;} // its scratch words were the handoff's
                        if(splitTailLogs++<64)__android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
                            "RENDER_DIAG_TILE_TAIL_SPLIT_REMAINDER write=%u ib=%08x size=%u words=%u match=%u first=%08x",
                            writeWord,d[0],size,n,unsigned(match),size?ibWord(0):0u);
                    } else if(splitTailsEnabled()) {
                        const unsigned m=unsigned(carry.prefix.size()),n=43-m;
                        std::vector<std::uint32_t> words(carry.prefix);
                        for(unsigned k=0;k<n && k<size;++k)words.push_back(ibWord(k));
                        auto bytes=tailBytes(words);
                        const bool ok=words.size()==43 && recomp::gpu::Gta5TileSceneContinuation::parse(carry.physical,bytes);
                        if(ok) {
                            carriedIrq=unsigned(irqs.size());carriedBins=carry.bins;
                            ranges.push_back({0,n,carriedIrq});irqs.emplace_back(carry.physical,std::move(bytes));
                            irqStoreAt.push_back(stores.size());irqBins.push_back(kickBins);scanFrom=n;
                        }
                        // (an IB ending in a mask reset or mask pair that no tail follows is common: not logged)
                        if((ok || m>=6) && splitTailLogs++<64)__android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
                            "RENDER_DIAG_TILE_TAIL_SPLIT write=%u tail=%08x ib=%08x size=%u depth=0 before=%u after=%u delivered=%u mode=with-next-ib",
                            writeWord,carry.physical,d[0],size,m,n,unsigned(ok));
                    }
                }
                for(unsigned k=scanFrom;k+35<=size;++k) {
                    const auto* p=ib.data()+std::size_t{k}*4;
                    // Both parsers require these first words; testing them first skips only calls that
                    // return nullopt, so the accepted windows are exactly v36's.
                    if(k+43<=size && be32(p)==0xC0006000u) { // DIAGNOSTIC v688: EndTiling tail (predicated IRQ) at any offset.
                        const std::span<const std::uint8_t> tail(p,std::size_t{43}*4);
                        if(recomp::gpu::Gta5TileSceneContinuation::parse(d[0]+k*4,tail)) {
                            static std::atomic<unsigned long long> tailLines{0}; // v218: sampled (diag_log_sample.h)
                            if(recomp::diag::sampled(tailLines))
                                ::recomp::diag::quietLogSampled(0,ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_TILE_TAIL write=%u ib=%08x offset=%u size=%u depth=%u",
                                    writeWord,d[0],k,size,depth);
                            ranges.push_back({k,k+43,unsigned(irqs.size())});irqs.emplace_back(d[0]+k*4,std::vector<std::uint8_t>(tail.begin(),tail.end()));
                            irqStoreAt.push_back(stores.size());irqBins.push_back(kickBins);k+=42;continue;
                        }
                    }
                    if(be32(p)==0x0001057Cu) {
                        const std::span<const std::uint8_t> window(p,std::size_t{35}*4);
                        if(recomp::gpu::Gta5TileCommandInterrupt::parse(d[0]+k*4,window)) {
                            ranges.push_back({k,k+35,unsigned(irqs.size())});irqs.emplace_back(d[0]+k*4,std::vector<std::uint8_t>(window.begin(),window.end()));
                            irqStoreAt.push_back(stores.size());irqBins.push_back(kickBins);k+=34;
                        }
                    }
                }
                if(depth==0 && size) { // v48c: does this IB end inside an EndTiling tail?
                    unsigned covered=scanFrom;for(const auto& r:ranges)covered=std::max(covered,std::min(r[1],size));
                    for(unsigned k=std::max(covered,size>42?size-42:0u);k<size;++k) {
                        if(ibWord(k)!=0xC0006000u)continue;
                        const unsigned m=size-k;
                        const auto t=tileTailTemplate(m>13?(ibWord(k+13)&~3u):0u,m>8?ibWord(k+8):0u);
                        bool prefix=true;for(unsigned j=0;prefix && j<m;++j)prefix=ibWord(k+j)==t[j];
                        if(!prefix || m<2 || !splitTailsEnabled())continue;
                        const std::uint32_t at=d[0]+k*4;
                        if(m>=31) { // the INTERRUPT packet (words 29-30) is in this IB
                            auto bytes=tailBytes(t);
                            const bool ok=recomp::gpu::Gta5TileSceneContinuation::parse(at,bytes).has_value();
                            if(ok) {
                                ranges.push_back({k,k+43,unsigned(irqs.size())});irqs.emplace_back(at,std::move(bytes));
                                irqStoreAt.push_back(stores.size());irqBins.push_back(kickBins);
                                splitTail=SplitTail{at,{},std::vector<std::uint32_t>(t.begin()+m,t.end())};
                            }
                            if(splitTailLogs++<64)__android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
                                "RENDER_DIAG_TILE_TAIL_SPLIT write=%u tail=%08x ib=%08x size=%u depth=0 before=%u after=%u delivered=%u mode=with-this-ib",
                                writeWord,at,d[0],size,m,43-m,unsigned(ok));
                        } else { // completed (or not) by the next depth-0 IB; its scratch words are the handoff's
                            SplitTail carry{at,{},{}};for(unsigned j=0;j<m;++j)carry.prefix.push_back(ibWord(k+j));
                            splitTail=std::move(carry);ranges.push_back({k,size,~0u});carryBuffer=ib.data();carryAt=k;
                        }
                        break;
                    }
                }
                if(depth==0)walkedIbs.push_back({d[0],size}); // Attempt v9: for the tail-expected evidence dump.
                // Attempt v3: a bad packet ends only its own buffer (the parent keeps going).
                if(!run(ib.data(),size,depth+1,ranges))++s.ibFailures;
                if(carryBuffer==ib.data())carryBuffer=nullptr;
                if(carriedIrq!=~0u)irqBins[carriedIrq]=carriedBins; // the bins at the tail's start, not at this IB's
                break;
            }
            case 0x22: case 0x36: ++s.draws;break;
            case 0x23: ++s.viz;break;
            case 0x58: ++s.shd;if(n>=3){stores.push_back({d[1],d[2]});if(shdSeen.size()<6)shdSeen.push_back(d);}break;
            case 0x5B:
                ++s.zpd;zpdCounter+=0x1000;
                if(sampleAddress)for(unsigned k=0;k<8;++k)stores.push_back({sampleAddress+k*4,(k==0||k==1||k==4||k==5)?zpdCounter:0});
                break;
            case 0x3D:
                ++s.memWrites;
                for(unsigned k=1;k<n;++k)stores.push_back({((d[0]&~3u)+(k-1)*4)|(d[0]&3),read(i+1+k)});
                break;
            case 0x54: ++s.interrupts;[[fallthrough]];
            case 0x3C: case 0x5A: {
                // DIAGNOSTIC: payloads of packets whose GPU effect is not modelled yet.
                static unsigned logged[128]{};const unsigned op=(h>>8)&0x7F;
                if(op==0x3C)++s.waits;
                // EVENT_WRITE_EXT: conservative full-screen tile extents {0,159,0,89} (8-pixel units,
                // big-endian 16-bit), the policy of gta5_tile_extent_publication.h. Never culls.
                if(op==0x5A && n>=2){const auto va=0xA0000000u+(d[1]&~3u);stores.push_back({va,0x0000009Fu,true});stores.push_back({va+4,0x00000059u,true});stores.push_back({va+8,0x00000001u,true});++s.shd;++s.ext;} // v3: z extents 0..1
                if(logged[op]++<64) {
                    char t[160];int w=std::snprintf(t,sizeof t,"op=%02x n=%u depth=%u",op,n,depth);
                    for(unsigned k=0;k<n && k<6 && w>0 && w<(int)sizeof t-12;++k)w+=std::snprintf(t+w,sizeof t-w," %08x",read(i+1+k));
                    __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_PACKET write=%u %s",writeWord,t);
                }
                break;
            }
            case 0x3E: ++s.regToMem;break;
            case 0x10: case 0x21: case 0x2A: case 0x2B: case 0x2D: case 0x2E: case 0x2F: case 0x25: case 0x27: case 0x3B:
            case 0x46: case 0x4A: case 0x4C: case 0x50: case 0x51: case 0x55: case 0x56: case 0x59: break;
            case 0x60: case 0x61: case 0x62: case 0x63: if(n>=1){const unsigned b=((h>>8)&0x7F)-0x60;kickBins.words[b]=d[0];kickBins.knownWords|=1u<<b;}break;
            default: ++s.unknown;unknownOps.insert((h>>8)&0x7F);break;
            }
            i+=1+n;
        }
        return true;
    }
};
} // namespace detail

// ---- occlusion package (occlusion_queries.h, diag-oc-queries). Off (no marker, nothing pending) every call returns at once:
// v436's stores, unchanged.
// A physical completion store about to be applied: cancels every armed, unpublished report whose query block it writes.
// Report placeholders (ocq 1) and their fillers (ocq 2) re-store D3D's sentinel (bytes already there) and touch nothing.
inline void ocqBefore(const detail::Store& st) noexcept {
    if(st.virt || st.ocq)return;
    try{recomp::gpu::ocq::engine().touched(st.address&~3u);}catch(...){}
}
// ... just applied: a placeholder arms its report (after its bytes landed, so a publication can never be overwritten by it).
inline void ocqAfter(const detail::Store& st) noexcept {
    if(st.ocq!=1)return;
    try{recomp::gpu::ocq::engine().armKey((st.address&~3u)-16u);}catch(...){}
}
// At each retirement's start: every resolved report published (Engine::publish; nothing when none is pending).
[[gnu::noinline]] inline void ocqPublish(recomp::bootstrap::GuestAddressSpace& space) noexcept {
    auto& e=recomp::gpu::ocq::engine();
    if(!e.pendingReports())return;
    try {
        e.publish([&](std::uint32_t physical,std::uint32_t value) {
            if(auto* p=physical<0x20000000u?space.translate(0xA0000000u+physical):nullptr)put(p,value,0);
        },std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()));
    } catch(...) {}
}
// After the walk: v436's fabricated ZPD store groups of this submission (8 stores at sampleAddress + 4k, values c,c,0,0,c,c,0,0
// with c the counter after that event: from < c <= to in steps of 0x1000), in stream order, each matched to its marker:
//   BEGIN: the begin block's words (beginWords(X)); END with a real BEGIN: the report placeholder + 7 fillers (D3D's sentinel
//   re-stored at +16, big-endian: the bytes D3D's END wrote before this event); unmatched END of a real BEGIN: the fallback
//   count now; a multi-tile issue's event (its voided marker): v436's group moved to that query's last tile block (v436 wrote
//   it at the stale address: another query's block); anything else (off, unmatched, endian bits, an END without a real BEGIN):
//   v436's group, unchanged.
[[gnu::noinline]] inline void ocqRewrite(std::vector<detail::Store>& stores,std::uint32_t from,std::uint32_t to) noexcept {
    using recomp::gpu::ocq::Engine;
    auto& e=recomp::gpu::ocq::engine();
    if(from==to || !e.active())return;
    try {
        const std::uint32_t span=to-from;
        const auto group=[&](std::size_t i)->bool {
            const auto& s0=stores[i];const std::uint32_t c=s0.value,d=c-from;
            if(s0.virt || s0.ocq || ((s0.address&~3u)&31u) || !d || d>span || (d&0xFFFu))return false;
            for(unsigned k=0;k<8;++k) {
                const auto& s=stores[i+k];
                if(s.virt || s.ocq || s.address!=s0.address+4*k || s.value!=((k==0||k==1||k==4||k==5)?c:0u))return false;
            }
            return true;
        };
        const std::uint64_t now=std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        for(std::size_t i=0;i+8<=stores.size();) {
            if(!group(i)){++i;continue;}
            const std::uint32_t a=stores[i].address;
            if(!(a&3)) {
                const auto m=e.match(a);
                if(m.kind==Engine::Match::Begin) {
                    const auto w=recomp::gpu::ocq::beginWords(e.begin(m.id,m.key));
                    for(unsigned k=0;k<8;++k)stores[i+k].value=w[k];
                } else if(m.kind==Engine::Match::End) {
                    if(e.end(m.id,m.key,now))
                        for(unsigned k=0;k<8;++k)stores[i+k]={(m.key+16u)|2u,recomp::gpu::ocq::Sentinel,false,std::uint8_t(k==0?1:2)};
                } else if(m.kind==Engine::Match::Voided) { // a multi-tile issue: v436's fabricated block at its last tile block
                    if(m.key)for(unsigned k=0;k<8;++k)stores[i+k].address=(m.begin?m.key+32u:m.key)+4*k;
                } else if(const auto x=e.unmatched(a)) {
                    const std::uint32_t v=*x+recomp::gpu::ocq::FallbackSamples;
                    const std::array<std::uint32_t,8> w{v,0u,0u,0u,v,recomp::gpu::ocq::SentinelLe,0u,0u};
                    for(unsigned k=0;k<8;++k)stores[i+k].value=w[k];
                }
            }
            i+=8;
        }
    } catch(...) {}
}

// v183 (goal step 2): `defer`, when set, takes the completion stores of a submission without tile interrupts (fences,
// query results, tile extents, scratch writes) to run after the renderer's pending reads, in queue order; the read
// pointer is still published here. It returns false to keep them synchronous. A submission with interrupts, a failed
// walk, or no `defer` first calls recomp::bootstrap::noteNativeSubmissionMayCommit() (the handler's barrier hook) and
// applies them here, as v182 did after its unconditional barrier.
using DeferStores=std::function<bool(std::vector<detail::Store>&&)>;
// v211: `deferIrqs`, when set, may take a walked submission's tile interrupts with all its completion stores, the
// store index before each interrupt, their bin states and the stream-final bin words (it swaps them out of the vectors
// it is given) and deliver them later itself; it returns false to leave them here. Taken: no drain, no interrupt and no
// store here; the read pointer is still published (as the v183 deferred path does).
struct DeferredInterrupts {
    std::vector<detail::Store> stores;std::vector<std::size_t> irqStoreAt;
    std::vector<std::pair<std::uint32_t,std::vector<std::uint8_t>>> irqs;std::vector<recomp::gpu::DrawBinState> irqBins;
    recomp::gpu::DrawBinState kickBins;std::uint32_t writeWord=0;
};
using DeferIrqs=std::function<bool(DeferredInterrupts&)>;
inline std::atomic<unsigned long long> deferredIrqSubmissions{0};
inline std::atomic<unsigned long long> deferredSubmissions{0},deferredStores{0},syncSubmissions{0};
// v194 (H16a, opt-in files/private-trace/diag-generic-irq-no-drain): a tile-interrupt submission with NO completion
// stores (its scratch words are the IRQ's own, performed by the handoff) publishes nothing a queued draw's reads
// depend on, so it does not drain the renderer before its interrupt (v183-v193 drained at site 20).
inline std::atomic<unsigned long long> irqNoDrainSubmissions{0};
inline bool irqNoDrainEnabled() {
    static const bool on=::access(RECOMP_APP_PATH("/private-trace/diag-generic-irq-no-drain"),F_OK)==0;
    return on;
}
inline bool retire(Registry& registry,recomp::bootstrap::GuestAddressSpace& space,std::uint32_t writeWord,
                   const Interrupt& interrupt,RetireTiming* timing=nullptr,const DeferStores& defer={},const DeferIrqs& deferIrqs={}) {
    using detail::Clock;
    ocqPublish(space); // occlusion package: reports resolved since the last retirement
    const std::uint32_t ocqFrom=zpdCounter; // occlusion package: the fabricated counter before this walk
    std::vector<detail::Store> stores;std::vector<std::pair<std::uint32_t,std::vector<std::uint8_t>>> irqs;
    std::uint32_t scratchVa=0;unsigned irqSkipped=0;
    std::vector<std::size_t> irqStoreAt;std::vector<recomp::gpu::DrawBinState> irqBins;recomp::gpu::DrawBinState kickBins; // DIAGNOSTIC v688
    std::vector<std::array<std::uint32_t,3>> shdSeen; // Attempt v2: EVENT_WRITE_SHD initiator/address/value, first 6.
    std::vector<std::array<std::uint32_t,2>> walkedIbs; // Attempt v9: depth-0 IB address/size of this submission.
    Stats s;std::set<unsigned> unknownOps;bool walked=false;std::uint32_t previous=0,count=0;
    std::uint64_t walkedWords=0;
    const auto lockTimed=[&](Clock::time_point requested,Clock::time_point acquired,Clock::time_point released) {
        if(!timing)return;
        timing->lockWaitNs+=detail::ns(requested,acquired);
        const auto held=detail::ns(acquired,released);
        timing->lockHoldNs+=held;timing->lockHoldMaxNs=std::max(timing->lockHoldMaxNs,held);
    };
    // v44 phase 1 (R): snapshot the ring configuration. Nothing else here is registry state.
    std::uint32_t ringVirtual=0,ringWords=0,writeBackVirtual=0;
    {
        const auto requested=timing?Clock::now():Clock::time_point{};
        std::lock_guard lock(registry.*get(MutexTag{}));
        const auto acquired=timing?Clock::now():Clock::time_point{};
        const auto& config=registry.*get(RingTag{});
        const bool configured=config && config->writeBack;
        if(configured) {
            ringVirtual=config->virtualAddress;ringWords=config->bytes/4;previous=config->readWord;
            writeBackVirtual=config->writeBack->virtualAddress;
        }
        lockTimed(requested,acquired,timing?Clock::now():Clock::time_point{});
        if(!configured)return false;
    }
    scratchVa=writeBackVirtual-60;
    if(!ringWords || writeWord>=ringWords || previous>=ringWords)return false;
    if(!space.base())return false; // v36: every read failed, then the writeback translation failed.
    // v44 phase 2 (no lock): walk the owned copies of the ring segment and its indirect buffers.
    const auto walkStart=timing?Clock::now():Clock::time_point{};
    {
        count=(writeWord+ringWords-previous)%ringWords;
        std::vector<std::uint8_t> segment(std::size_t{count}*4);
        const std::uint32_t first=std::min(count,ringWords-previous);
        if(first)std::memcpy(segment.data(),space.translate(ringVirtual+previous*4),std::size_t{first}*4);
        if(count>first)std::memcpy(segment.data()+std::size_t{first}*4,space.translate(ringVirtual),std::size_t{count-first}*4);
        detail::Walk walk{space,writeWord,stores,irqs,irqStoreAt,irqBins,kickBins,shdSeen,walkedIbs,s,unknownOps,walkedWords};
        walked=walk.run(segment.data(),count,0,detail::Ranges{});
        ocqRewrite(stores,ocqFrom,zpdCounter); // occlusion package
    }
    const auto walkEnd=timing?Clock::now():Clock::time_point{};
    const bool deferring=defer && walked && irqs.empty(); // v183
    bool irqsDeferred=false; // v211
    if(deferIrqs && walked && !irqs.empty()) {
        DeferredInterrupts d{std::move(stores),std::move(irqStoreAt),std::move(irqs),std::move(irqBins),kickBins,writeWord};
        irqsDeferred=deferIrqs(d);
        if(!irqsDeferred){stores=std::move(d.stores);irqStoreAt=std::move(d.irqStoreAt);irqs=std::move(d.irqs);irqBins=std::move(d.irqBins);}
        else {stores.clear();irqStoreAt.clear();irqs.clear();irqBins.clear();kickBins={};deferredIrqSubmissions.fetch_add(1,std::memory_order_relaxed);}
    }
    const bool storeFreeIrq=!irqsDeferred && defer && walked && !irqs.empty() && stores.empty() && irqNoDrainEnabled(); // v194
    if(storeFreeIrq)irqNoDrainSubmissions.fetch_add(1,std::memory_order_relaxed);
    else if(!deferring && !irqsDeferred)recomp::bootstrap::noteNativeSubmissionMayCommitAt(20); // v183 site 20: drain the renderer before any completion
    unsigned irqRan=0,irqFailed=0,failures=0;std::size_t applied=0;
    const auto apply=[&](std::size_t end) {
        for(;applied<end && applied<stores.size();++applied) {
            const auto& st=stores[applied];
            if(st.virt){auto* p=space.translate(st.address);if(!p){++failures;continue;}
                put(p,st.value,2);continue;} // v44: one release store (v36: four byte stores, same bytes).
            const auto physical=st.address&~3u;
            auto* p=physical<0x20000000u?space.translate(0xA0000000u+physical):nullptr;
            if(!p){++failures;continue;}
            ocqBefore(st); // occlusion package (unconditional, noexcept: this lambda's CFG unchanged)
            put(p,st.value,st.address&3);
            ocqAfter(st);
        }
    };
    // Attempt v9: a delivered begin-tiling command IRQ (35 words) means the pass's EndTiling tail (43
    // words) must follow. A submission carrying EXT extents while a tail is expected, without one, is the
    // v1/v6 freeze signature (pass and tail merged; no continuation IRQ). Dump its IBs (up to 2 times).
    {
        static bool expectTail=false;static unsigned tailDumps=0;
        bool hasTail=false,hasCommandIrq=false;
        for(const auto& q:irqs){hasTail|=q.second.size()==43*4;hasCommandIrq|=q.second.size()==35*4;}
        if(expectTail && !hasTail && s.ext && tailDumps<2) {
            ++tailDumps;
            __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_TAIL_EXPECTED_MISSING write=%u prev=%u ibs=%zu packets=%llu draws=%llu ext_or_shd=%llu",
                writeWord,previous,walkedIbs.size(),s.packets,s.draws,s.shd);
            for(const auto& ib:walkedIbs)for(unsigned k0=0;k0<ib[1];k0+=32) {
                char line[32*9+48];int lw=std::snprintf(line,sizeof line,"ib=%08x off=%u",ib[0],k0);
                for(unsigned k=k0;k<ib[1] && k<k0+32 && lw>0 && lw<(int)sizeof line-10;++k){
                    const auto* p=space.translate(0xA0000000u+ib[0]+k*4);
                    lw+=std::snprintf(line+lw,sizeof line-lw," %02x%02x%02x%02x",p?p[0]:0,p?p[1]:0,p?p[2]:0,p?p[3]:0);}
                __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_IB_WORDS %s",line);
                if(k0%(32*64)==0)usleep(2000); // let logcat keep up; losing lines defeats the dump
            }
        }
        if(hasTail)expectTail=false;
        else if(hasCommandIrq)expectTail=true;
    }
    for(std::size_t j=0;j<irqs.size();++j) { // DIAGNOSTIC v688: stream order.
        apply(irqStoreAt[j]);currentBins=irqBins[j];tailSkipped=false;
        const bool ran=interrupt && interrupt(irqs[j].first,irqs[j].second);
        if(ran)++irqRan;
        else if(tailSkipped) { // v689: unpredicated type-0 scratch writes of a skipped pass.
            // v700: the tile request's scratch block, NOT the RPTR-60 D3D fence block.
            const auto scratch=[&](unsigned slot,std::uint32_t v){auto* p=skippedTailScratch?space.translate(0xA0000000u+skippedTailScratch+slot*4):nullptr;if(!p){++failures;return;}put(p,v,2);};
            scratch(0,4);scratch(4,0x836BE1C8u);scratch(5,skippedTailContext);scratch(4,0x0BADF00Du);++irqSkipped;
        } else ++irqFailed;
    }
    const auto irqEnd=timing?Clock::now():Clock::time_point{};
    // v44 phase 3 (R): revalidate the snapshot the walk used, then publish. v36 checked readWord only
    // (it walked under R); a walk outside R also requires the same ring and writeback.
    {
        const auto requested=timing?Clock::now():Clock::time_point{};
        std::lock_guard lock(registry.*get(MutexTag{}));
        const auto acquired=timing?Clock::now():Clock::time_point{};
        auto& config=registry.*get(RingTag{});
        bool published=false;
        if(config && config->writeBack && config->readWord==previous && config->virtualAddress==ringVirtual &&
           config->bytes/4==ringWords && config->writeBack->virtualAddress==writeBackVirtual) {
            if(!deferring)apply(stores.size()); // v183: deferred stores are handed over after R is released
            if(auto* wb=space.translate(config->writeBack->virtualAddress)) {
                put(wb,writeWord,2); // v44: one release store after every completion store (v36: bytes).
                config->readWord=writeWord;++retired;published=true;
            }
        }
        lockTimed(requested,acquired,timing?Clock::now():Clock::time_point{});
        if(!published)return false;
    }
    if(irqsDeferred){} // v211: the deferred interrupts' stores are the deliverer's (none remain here)
    else if(deferring) { // v183
        deferredSubmissions.fetch_add(1,std::memory_order_relaxed);deferredStores.fetch_add(stores.size(),std::memory_order_relaxed);
        if(!stores.empty() && !defer(std::move(stores))) { // refused (not pipelined): apply now, after the barrier hook
            recomp::bootstrap::noteNativeSubmissionMayCommitAt(21);apply(stores.size());
        }
    } else syncSubmissions.fetch_add(1,std::memory_order_relaxed);
    const auto binStart=timing?Clock::now():Clock::time_point{};
    if(kickBins.knownWords && interrupt){currentBins=kickBins;(void)interrupt(0,{});} // v689: stream-final bin words.
    if(timing) {
        timing->walkNs+=detail::ns(walkStart,walkEnd);
        timing->irqNs+=detail::ns(walkEnd,irqEnd)+detail::ns(binStart,Clock::now());
        timing->words+=walkedWords;timing->ibs+=unsigned(s.ibs);timing->irqs+=unsigned(irqs.size());
    }
    static std::atomic<unsigned long long> kickLines{0}; // v218: sampled; failures always (diag_log_sample.h)
    if(recomp::diag::sampled(kickLines) || irqFailed || failures || s.ibFailures || !unknownOps.empty() || !walked)
    { // Attempt v2: one compact line per retired submission, for monitoring and fence forensics.
        const auto be=[&](std::uint32_t a)->std::uint32_t{const auto* p=space.translate(a);
            return p?(std::uint32_t{p[0]}<<24)|(std::uint32_t{p[1]}<<16)|(std::uint32_t{p[2]}<<8)|p[3]:0xFFFFFFFFu;};
        char t[420];int w=std::snprintf(t,sizeof t,"n=%llu prev=%u write=%u pk=%llu ibs=%llu draws=%llu shd=%llu zpd=%llu noop=%llu ibfail=%llu pfd=%llu t1=%llu walk=%u irq=%zu/%u/%u/%u fence=%08x issued=%08x",
            retired,previous,writeWord,s.packets,s.ibs,s.draws,s.shd,s.zpd,s.noops,s.ibFailures,s.pfd,s.type1,unsigned(walked),irqs.size(),irqRan,irqSkipped,irqFailed,
            be(scratchVa),be(0x1004cd00u+11036));
        for(const auto& e:shdSeen)if(w>0 && w<(int)sizeof t-40)w+=std::snprintf(t+w,sizeof t-w," shd=%08x:%08x=%08x",e[0],e[1],e[2]);
        ::recomp::diag::quietLogSampled(int(irqFailed!=0)|int(failures!=0)|int(s.ibFailures!=0)|int(!unknownOps.empty())|int(!walked),ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_KICK %s",t); // quiet-log: every 16th unless a failure
    }
    std::string ops;for(auto op:unknownOps){char t[8];std::snprintf(t,sizeof t,"%02x,",op);ops+=t;}
    char key[160];std::snprintf(key,sizeof key,"%llu/%llu/%llu/%llu/%llu/%llu/%s/%u",s.ibs,s.draws,s.zpd,s.shd,s.interrupts,s.waits,ops.c_str(),unsigned(walked));
    const bool fresh=profiles.insert(key).second;
    static std::atomic<unsigned long long> irqRetiredLines{0}; // v218: IRQ submissions sampled (diag_log_sample.h)
    if(fresh || retired<=16 || !(retired&(retired-1)) || (!irqs.empty() && (irqFailed || recomp::diag::sampled(irqRetiredLines))))
        ::recomp::diag::quietLogSampled(int(retired<=16)|int(!(retired&(retired-1)))|(int(!irqs.empty())&int(irqFailed!=0)),ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_RETIRED_SUBMISSION previous=%u write=%u words=%u scratch=%llu packets=%llu ibs=%llu draws=%llu viz=%llu zpd=%llu shd=%llu mem_writes=%llu interrupts=%llu waits=%llu reg_to_mem=%llu unknown=%llu unknown_ops=%s walk_ok=%u store_failures=%u tile_irqs=%zu irq_ran=%u irq_failed=%u irq_skipped=%u new_profile=%u profiles=%zu total=%llu executed=0 fabricated_zpd=1\n",
            previous,writeWord,count,s.scratch,s.packets,s.ibs,s.draws,s.viz,s.zpd,s.shd,s.memWrites,s.interrupts,s.waits,s.regToMem,s.unknown,
            ops.empty()?"-":ops.c_str(),unsigned(walked),failures,irqs.size(),irqRan,irqFailed,irqSkipped,unsigned(fresh),profiles.size(),retired);
    return true;
}

// v44: GuestImportRegistry::submitNativeRuntimeMaintenancePoints (reached through the renderer's
// submitRendererRuntimeMaintenance) opens a budget-256 EXCLUSIVE scope (every permit domain parks)
// before any admission check. In v36 it ran for every submission once a scene target existed, ~95
// times a second, and never completed after the prologue's write 100. This returns false only when
// that call must return false: every condition below is one the registry checks on the same inputs
// (the ring configuration, the three ring words and the parent IB's words, read through the same
// physical arena mapping as resolvePhysicalRange), namely reject(6) previous+3==writeWord, the ring
// decode, inspectPm4IndirectGraph's IB layout/limit/reader and inspectRuntimeMaintenanceEnvelope's
// parent-IB checks. Those inputs cannot change before the registry reads them: readWord moves only
// in the submission handler chain (this thread, graphics-submission exclusion held) and the words
// are the submitter's own queued commands. Skipping changes only reporting (no gate, no
// RENDER_RUNTIME_MAINTENANCE line, no rejection record).
inline bool runtimeMaintenanceCandidate(Registry& registry,recomp::bootstrap::GuestAddressSpace& space,std::uint32_t writeWord) {
    std::uint32_t previous=0,ringVirtual=0,ringBytes=0,arenaBase=0,arenaSize=0,arenaOffset=0;
    {
        std::lock_guard lock(registry.*get(MutexTag{}));
        const auto& config=registry.*get(RingTag{});
        if(!config || !config->writeBack)return false;              // reject(3) / reject(6)
        previous=config->readWord;ringVirtual=config->virtualAddress;ringBytes=config->bytes;
        arenaBase=registry.*get(ArenaBaseTag{});arenaSize=registry.*get(ArenaSizeTag{});arenaOffset=registry.*get(ArenaOffsetTag{});
    }
    if(std::uint64_t{previous}+3!=writeWord || writeWord>=ringBytes/4)return false; // reject(6)
    const auto* ring=space.translate(ringVirtual+previous*4);
    if(!ring || detail::be32(ring)!=0xC0013F00u)return false;        // envelope: root {C0013F00,address,words}
    const auto address=detail::be32(ring+4),words=detail::be32(ring+8);
    // Graph: aligned physical IB inside 512 MiB, at most maxWords(32768)-3 root words; envelope: >=32 words.
    if(words<32 || words>32765 || (address&3) || std::uint64_t{address}+std::uint64_t{words}*4>0x20000000u)return false;
    // Reader: resolvePhysicalRange maps the physical range into the bound arena or rejects it.
    if(address<arenaOffset || std::uint64_t{address-arenaOffset}+std::uint64_t{words}*4>arenaSize)return false;
    const auto* ib=space.translate(static_cast<std::uint32_t>(arenaBase+(address-arenaOffset)));
    if(!ib)return false;
    constexpr std::array<std::uint32_t,4> scissor{0x22080,0,0,0x01800500};
    constexpr std::array<std::uint32_t,6> idle{0x5C8,0x20000,0xE00,1,0xE40,1};
    for(unsigned k=0;k<scissor.size();++k)if(detail::be32(ib+k*4)!=scissor[k])return false;
    for(unsigned k=0;k<idle.size();++k)if(detail::be32(ib+(13+k)*4)!=idle[k])return false;
    return true;
}
} // namespace diagnostic_skip
// ===== end DIAGNOSTIC SKIP helper =====
