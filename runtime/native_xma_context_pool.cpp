// XMA layout, packet traversal and PCM conversion adapted from Xenia:
// src/xenia/apu/{xma_context.cc,xma_helpers.h,xma_decoder.cc}.
// Output-block scheduling also follows ReXGlue src/audio/xma_context.cpp
// (Tom Clay, 2026). BSD-3-Clause; see licenses/Xenia-BSD-3-Clause.txt.
//
// Concurrency (multicore plan WP4, local-analysis/parallel-design/plan.md). Lock order:
// R (registry mutex) > L_xma > L_xma_ctx > L_phys > L_audio > leaves.
// - L_xma (poolLock): context slots, pending_, control_, lastContext_, the frame/PCM counters,
//   the first recorded failure and the diagnostic kicked set. Process-wide; the runner owns one
//   pool. XMACreateContext/XMAReleaseContext reach allocate()/release() with R held (R > L_xma).
// - L_xma_ctx (Context::lock): one context's decoder state and its decode/lock/clear. Taken
//   hand-over-hand while holding L_xma, never the other way round; release() waits for an
//   in-flight decode on it before the Context is destroyed.
// - map_ may take R (registry.mapNativeAudioPhysical), so it is NEVER called with an XMA lock
//   held: decode() first resolves the output and input buffers from a lock-free snapshot of the
//   context words, then decodes under L_xma_ctx and uses a mapping only if the words still name
//   the same range (a changed range means no progress this call; the next kick/poll resolves it).
// - Guest context words are read and published as aligned 32-bit big-endian atomics. Decoder
//   write-back merges only decoder-owned fields into the current guest words (one CAS per word,
//   Xenia StoreContextMerged), so a concurrent guest store to a guest-owned field is kept.
// - failure()/failureContext() are read without a lock by the accessing thread. Cooperative
//   builds keep the exact contract (they describe the latest access; one guest runs at a time).
//   Multicore builds (RECOMP_GUEST_THREADS != 0) record the pool's first failure once and never
//   clear it (a failed access stops the run), so a reader never races a writer.
// With one guest thread at a time (cooperative mode) the words, PCM, counters and failures are
// the same as the previous synchronous implementation.
//
// v72: start subframe skip (context word 1 bits 24-26). Evidence (original PPC, our own reading):
// the audio engine's stream submit sub_83140060 seeks a new voice to sample S by setting the input
// read offset to frame F=(S-128)/512 (sub_831360B0) and, through sub_836A9A40, word 1 bits 24-26 to
// K=((S-128)%512)/128, and records the voice position [20] as F*512+K*128. The consumer
// (sub_83140340) then discards [12]-[20] samples and ends the voice when [20] reaches the
// frame-aligned stream length [8] (sub_83136DE0). That accounting only holds if the decoder omits
// the first K subframes of the first frame it decodes after the context is set up; the XAudio
// layer's sub_8365A3D0 writes the same field together with the read offset. v71 decoded every
// subframe of that frame (it used the field only as an output reserve), so a seeked voice yields
// K*128 samples per channel more than the title counts. At the voice's end sub_83140340 then
// consumes one sample past [8], switches to the next queued voice with that surplus still in r23,
// and its unbounded skip loop (loc_83140450) waits for the new voice's never-refreshed local
// context copy: the hostage-scene audio spin. Applied once per fresh decoder state (allocation or
// clear command), before any loop jump; the guest words are not changed. The output-reserve use of
// the same field is kept as in v71. files/private-trace/diag-xma-no-start-skip restores v71.
//
// audio-cost (stage D package, 2026-10-03; v427 profile perf-audit/audio-prof-v427-1): the audio engine thread 64141000
// spends 9.6 / 14.3 ms per house / outdoor frame on the big cores, 2.7 / 3.8 ms of it in this pool + the codec. Three exact
// fast paths, each behind a bit of recomp_au_switches (the renderer's 60-present window poll stores it from
// files/private-trace/diag-au-bits / -pcm / -pump, diag-au-all = all three; 0 = the v427 code paths, unchanged):
//  bit 0 (diag-au-bits): compressed-frame assembly copies bit spans byte-wise (copyBitSpan) instead of one bit per loop
//        iteration (v427: ~9 instructions per bit with a store/reload of Context::assembled; 0.30 / 0.43 ms per frame).
//        Same stops: the 15-bit length header, the frame length, the packet end, the 4096-byte assembly bound; exactly the
//        same bits of Context::compressed are written (stale bits beyond the frame stay, as they reach the codec padding).
//  bit 1 (diag-au-pcm): writeXmaPcmBlocksSpan writes each 256-byte block (128 scalar samples) in 8/16-sample NEON groups
//        with the block's ring position computed once (v427: three udiv per sample for % capacity and / channels;
//        0.27 / 0.41 ms per frame). Same bounds checks and result; clamp/scale/truncate are the same IEEE operations under
//        the same FPCR (AArch64 Advanced SIMD honours FPCR.FZ/RMode like scalar FP; the codec admits only finite samples).
//  bit 2 (diag-au-pump): the 7FEA1818 progress-poll pump first evaluates, on the same lock-free word snapshot and with the
//        same output-buffer map_ call that decode()'s phase 1 makes, whether decode() could do anything: output buffer not
//        valid, or every decode() failure check passes and the ring lacks room for one consumption quantum (+ reserve).
//        Then the pump skips that context's phase 2 (pool + context lock round trips, second word read, diagnostic state):
//        decode() would have returned true there without touching guest words, decoder state or PCM. Anything else runs
//        the unchanged decode(). Decoding only fills a ring (free space never grows during a concurrent decode), so a
//        snapshot that shows a full ring stays full until the guest consumes (a guest write the original could equally
//        have missed by locking first). v427: the pump's per-context phase 1 + 2 cost ~5% of the thread (0.5 / 0.75 ms
//        per frame), its skip-eligible share unknown until measured (counters RENDER_DIAG_AU pump_skipped / pump_decoded).
//        Only the diagnostic per-context decode counter (RENDER_DIAG_XMA_CTX decodes=) no longer counts skipped calls.
#include "native_xma_context_pool.h"
#include "native_xma_pcm_blocks.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <unistd.h>
#include <android/log.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include "recomp_app_dir.h" // data-paths: the app's files directory.
// audio-cost: the in-run switch bits (see the header comment) and the pump counters the renderer logs.
extern "C" {
std::atomic<unsigned> recomp_au_switches{0};
std::atomic<unsigned long long> recomp_au_pump_skipped{0}, recomp_au_pump_decoded{0};
}

namespace recomp::audio {
namespace {
#if !defined(RECOMP_AU_MUTANT)
#define RECOMP_AU_MUTANT 0 // host tests only: 1 bit copy drops a span's last bit, 2 PCM rounds to nearest, 3 pump skips
#endif                     // a context with stop bits set, 4 pump skips a ring with exactly one quantum + reserve free
// audio-cost bit 0: copy n bits, MSB-first within bytes, from src bit s to dst bit d. Writes exactly bits [d, d+n) of dst
// (the bytes holding them keep every other bit) and reads only the bytes holding src bits [s, s+n).
void copyBitSpan(std::uint8_t* dst, unsigned d, const std::uint8_t* src, unsigned s, unsigned n) {
    if (RECOMP_AU_MUTANT == 1 && n > 1) --n;
    const auto one = [&] {
        const unsigned v = (src[s / 8] >> (7 - s % 8)) & 1U;
        dst[d / 8] = std::uint8_t((dst[d / 8] & ~(1U << (7 - d % 8))) | (v << (7 - d % 8)));
        ++s; ++d; --n;
    };
    while (n && (d & 7)) one();
    const unsigned bytes = n / 8;
    if (bytes) {
        std::uint8_t* out = dst + d / 8;
        const std::uint8_t* in = src + s / 8;
        const unsigned sh = s & 7;
        if (!sh) std::memcpy(out, in, bytes);
        else for (unsigned k = 0; k < bytes; ++k) out[k] = std::uint8_t((in[k] << sh) | (in[k + 1] >> (8 - sh)));
        d += 8 * bytes; s += 8 * bytes; n -= 8 * bytes;
    }
    while (n) one();
}
// audio-cost bit 1: writeXmaPcmBlocks (native_xma_pcm_blocks.h) with the same preconditions and result. A block is 128
// scalar samples = 256 bytes; write is 256-aligned and capacity a multiple of 256, so a block never wraps inside.
#if defined(__aarch64__)
inline int32x4_t pcmScale4(const float* p) { // clamp to [-1, 1], * 32767, truncate toward zero (FCVTZS)
    const float32x4_t v = vminq_f32(vmaxq_f32(vld1q_f32(p), vdupq_n_f32(-1.0f)), vdupq_n_f32(1.0f));
#if RECOMP_AU_MUTANT == 2
    return vcvtnq_s32_f32(vmulq_f32(v, vdupq_n_f32(32767.0f)));
#else
    return vcvtq_s32_f32(vmulq_f32(v, vdupq_n_f32(32767.0f)));
#endif
}
#endif
inline std::uint16_t pcmScale1(float f) {
    return static_cast<std::uint16_t>(static_cast<std::int16_t>(std::clamp(f, -1.0f, 1.0f) * 32767.0f));
}
inline void pcmGroup8(const float* f, std::uint8_t* out) { // 8 consecutive samples of one channel -> 16 BE bytes (mono)
#if defined(__aarch64__)
    const int16x8_t v = vcombine_s16(vmovn_s32(pcmScale4(f)), vmovn_s32(pcmScale4(f + 4)));
    vst1q_u8(out, vrev16q_u8(vreinterpretq_u8_s16(v)));
#else
    for (unsigned i = 0; i < 8; ++i) { const auto v = pcmScale1(f[i]); out[2 * i] = std::uint8_t(v >> 8); out[2 * i + 1] = std::uint8_t(v); }
#endif
}
inline void pcmGroup8x2(const float* l, const float* r, std::uint8_t* out) { // 8 L/R pairs -> 32 BE bytes (stereo)
#if defined(__aarch64__)
    const int16x8_t L = vcombine_s16(vmovn_s32(pcmScale4(l)), vmovn_s32(pcmScale4(l + 4)));
    const int16x8_t R = vcombine_s16(vmovn_s32(pcmScale4(r)), vmovn_s32(pcmScale4(r + 4)));
    vst1q_u8(out, vrev16q_u8(vreinterpretq_u8_s16(vzip1q_s16(L, R))));
    vst1q_u8(out + 16, vrev16q_u8(vreinterpretq_u8_s16(vzip2q_s16(L, R))));
#else
    for (unsigned i = 0; i < 8; ++i) {
        const auto a = pcmScale1(l[i]), b = pcmScale1(r[i]);
        out[4 * i] = std::uint8_t(a >> 8); out[4 * i + 1] = std::uint8_t(a);
        out[4 * i + 2] = std::uint8_t(b >> 8); out[4 * i + 3] = std::uint8_t(b);
    }
#endif
}
bool writeXmaPcmBlocksSpan(const NativeXmaFrameDecoder::Frame& frame, unsigned firstBlock, unsigned blocks,
                           std::uint8_t* output, unsigned capacity, unsigned& write) {
    if (!output || !capacity || capacity%256 || write>=capacity || write%256 ||
        frame.channels<1 || frame.channels>2 || frame.samples!=512 ||
        firstBlock>frame.channels*4 || blocks>frame.channels*4-firstBlock ||
        blocks>capacity/256) return false;
    for (unsigned b=firstBlock;b<firstBlock+blocks;++b) {
        std::uint8_t* out=output+write;
        if (frame.channels==1) {
            const float* f=frame.planar[0].data()+b*128;
            for (unsigned g=0;g<16;++g) pcmGroup8(f+8*g,out+16*g);
        } else {
            const float* l=frame.planar[0].data()+b*64;
            const float* r=frame.planar[1].data()+b*64;
            for (unsigned g=0;g<8;++g) pcmGroup8x2(l+8*g,r+8*g,out+32*g);
        }
        write=write+256==capacity ? 0 : write+256;
    }
    return true;
}
// v72 start subframe skip switch: -1 not yet read, 0 off (v71), 1 on. Read once, lazily.
std::atomic<int> startSkipMode{-1};
std::atomic<unsigned long long> startSkipApplied{0};
bool startSkipEnabled() {
    int mode=startSkipMode.load(std::memory_order_acquire);
    if (mode>=0) return mode!=0;
    const int read=::access(RECOMP_APP_PATH("/private-trace/diag-xma-no-start-skip"),F_OK)==0 ? 0 : 1;
    if (startSkipMode.compare_exchange_strong(mode,read,std::memory_order_acq_rel))
        __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_XMA_START_SKIP enabled=%d (v72; diag-xma-no-start-skip %s)",
            read,read?"absent":"present: v71 behaviour");
    return startSkipMode.load(std::memory_order_acquire)!=0;
}
// DIAGNOSTIC v72: per-context decoder state published at the end of each decode (for the audio
// spin probe in ppc_recomp.254.cpp). Plain relaxed atomics; one writer per context at a time.
struct DiagnosticXmaState {
    std::atomic<unsigned> assembled{0},length{0},skipPackets{0},pcmBlock{0},pcmEndBlock{0},flags{0},decodes{0};
};
std::array<DiagnosticXmaState,NativeXmaContextPool::ContextCount> diagnosticXmaStates{};
std::atomic<std::uint32_t> diagnosticXmaVirtualBase{0};
std::atomic<const void*> diagnosticXmaPool{nullptr};
#if defined(RECOMP_GUEST_THREADS) && RECOMP_GUEST_THREADS != 0
constexpr bool KeepFirstFailure=true;
#else
constexpr bool KeepFirstFailure=false;
#endif
std::mutex& poolLock() { static auto* lock=new std::mutex; return *lock; } // L_xma
// XMA locks held by this thread (poolLock and context locks). map_ runs only at zero.
thread_local unsigned xmaLocksHeld=0;
// Failure reason and PCM bytes of the current thread's last decode() call.
thread_local const char* callFailure=nullptr;
thread_local std::uint64_t callPcmBytes=0;
struct PoolGuard {
    PoolGuard() {poolLock().lock();++xmaLocksHeld;}
    ~PoolGuard() {--xmaLocksHeld;poolLock().unlock();}
    PoolGuard(const PoolGuard&)=delete;
    PoolGuard& operator=(const PoolGuard&)=delete;
};

std::uint32_t* word32(const std::uint8_t* p) {return reinterpret_cast<std::uint32_t*>(const_cast<std::uint8_t*>(p));}
std::uint32_t loadWord(const std::uint8_t* p) {return __builtin_bswap32(__atomic_load_n(word32(p),__ATOMIC_ACQUIRE));}
void storeWord(std::uint8_t* p,std::uint32_t v) {__atomic_store_n(word32(p),__builtin_bswap32(v),__ATOMIC_RELEASE);}
void keepWordBits(std::uint8_t* p,std::uint32_t keep) {__atomic_fetch_and(word32(p),__builtin_bswap32(keep),__ATOMIC_ACQ_REL);}
void scrubContext(std::uint8_t* p) {for (unsigned i=0;i<16;++i) storeWord(p+4*i,0);}

// DIAGNOSTIC v697: last 64 context events, dumped by the runner. Leaf lock.
struct DiagnosticXmaEvent {char op='?';unsigned context=0;std::uint32_t w0=0,w1=0,w9=0;std::chrono::steady_clock::time_point at{};};
std::array<DiagnosticXmaEvent,64> diagnosticXmaEvents{};unsigned diagnosticXmaNext=0;unsigned long long diagnosticXmaTotal=0;
std::mutex diagnosticXmaMutex;
void diagnosticXmaRecord(char op,unsigned context,const std::uint8_t* words) {
    std::lock_guard lock(diagnosticXmaMutex);auto& e=diagnosticXmaEvents[diagnosticXmaNext];diagnosticXmaNext=(diagnosticXmaNext+1)%64;++diagnosticXmaTotal;
    e.op=op;e.context=context;e.at=std::chrono::steady_clock::now();
    const auto be=[&](unsigned o)->std::uint32_t{return words?loadWord(words+o):0;};e.w0=be(0);e.w1=be(4);e.w9=be(36);
}
// DIAGNOSTIC v696: contexts kicked and not since locked/cleared/released, per pool. Under L_xma.
std::map<const void*,std::bitset<NativeXmaContextPool::ContextCount>>& diagnosticKicked() {
    static std::map<const void*,std::bitset<NativeXmaContextPool::ContextCount>> kicked;return kicked;
}
std::atomic<unsigned long long> diagnosticPumps{0},diagnosticPumpFailures{0};
constexpr unsigned PacketBits = 2048 * 8;
unsigned bit(const std::uint8_t* p, unsigned offset) {
    return (p[offset / 8] >> (7 - offset % 8)) & 1;
}
unsigned firstFrame(const std::uint8_t* packet) {
    return 32 + ((packet[0] & 3) << 13) + (packet[1] << 5) + (packet[2] >> 3);
}
struct Words {
    std::array<std::uint32_t, 16> w;
    explicit Words(const std::uint8_t* p) { for (unsigned i=0;i<16;++i) w[i]=loadWord(p+4*i); }
    unsigned get(unsigned word, unsigned shift, std::uint32_t mask) const { return (w[word] >> shift) & mask; }
    void set(unsigned word, unsigned shift, std::uint32_t mask, unsigned value) {
        w[word] = (w[word] & ~(mask << shift)) | ((value & mask) << shift);
    }
    unsigned current() const { return w[4] >> 31; }
    bool valid(unsigned b) const { return get(0,20+b,1); }
    unsigned packets() const { return get(current(),0,4095); }
    unsigned offset() const { return get(2,0,0x3FFFFFF); }
    void offset(unsigned bits) { set(2,0,0x3FFFFFF,bits); }
    void swap() { set(0,20+current(),1,0); w[4] ^= 0x80000000U; offset(0); }
};
// Decoder-owned fields are taken from the decoder's copy; clear-only fields are cleared in guest
// memory only where this decoder cleared them since the previous publication. Everything else
// (buffer pointers and sizes, loop window, format, the guest read offset) stays the guest's.
struct MergeMask {unsigned index;std::uint32_t owned,clearOnly;};
constexpr std::array<MergeMask,4> DecoderFields{{
    {0,(255U<<12)|(31U<<27),3U<<20}, // loop count, output write offset | input buffer valid
    {1,0,1U<<31},                    // output buffer valid
    {2,0x3FFFFFFU,0},                // input buffer read offset
    {4,(31U<<26)|(1U<<31),0},        // packet metadata, current input buffer
}};
void publish(std::uint8_t* stored,const Words& baseline,const Words& working) {
    for (const auto& f:DecoderFields) {
        const auto owned=working.w[f.index]&f.owned;
        const auto cleared=f.clearOnly&baseline.w[f.index]&~working.w[f.index];
        auto* p=word32(stored+4*f.index);
        auto raw=__atomic_load_n(p,__ATOMIC_ACQUIRE);
        for (;;) {
            const auto current=__builtin_bswap32(raw);
            const auto merged=((current&~f.owned)|owned)&~cleared;
            if (merged==current || __atomic_compare_exchange_n(p,&raw,__builtin_bswap32(merged),false,
                    __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) break;
        }
    }
}
// One buffer range resolved before decoding (map_ may block on the registry mutex).
struct BufferMap {
    std::uint32_t address=0,bytes=0;std::uint8_t* host=nullptr;bool resolved=false;
    bool names(std::uint32_t a,std::uint32_t n) const {return resolved && address==a && bytes==n;}
};
struct DecodeMaps {BufferMap output;std::array<BufferMap,2> input;};
// audio-cost bit 2: true only when decode() on these words returns true having changed nothing: the output buffer is not
// valid (its first phase-2 test), or every test decode() fails on before its free-space test passes (stop/interrupt mode,
// loop window, ring bounds, output mapping) and the ring lacks room for one quantum plus the reserve. The map_ call is the
// one decode()'s phase 1 makes for the same words (phase 1 maps the inputs only when there is room, i.e. never here).
template<class Map> bool pumpIdle(const Words& s,const Map& map) {
    if (!s.get(1,31,1)) return true;
    if (s.get(9,30,3) && RECOMP_AU_MUTANT!=3) return false;
    const auto loops=s.get(0,12,255),loopStart=s.get(3,0,0x3FFFFFF),loopEnd=s.get(4,0,0x3FFFFFF);
    if (loops && (loopStart<32 || loopStart>loopEnd || s.get(1,17,7)>4 ||
        (loopStart==loopEnd && s.get(1,17,7)>s.get(1,12,3)))) return false;
    const unsigned capacity=s.get(0,22,31)*256,read=s.get(9,0,31)*256,write=s.get(0,27,31)*256;
    if (!capacity || read>=capacity || write>=capacity) return false;
    if (!map(s.w[7],capacity,true)) return false;
    const unsigned freeBytes=write>=read ? capacity-write+read : read-write;
    const unsigned count=std::max(1U,s.get(1,20,15)),reserve=s.get(1,24,7)*256;
    if (RECOMP_AU_MUTANT==4) return freeBytes<=count*256+reserve;
    return freeBytes<count*256+reserve;
}
// L_xma then L_xma_ctx. Returns the context locked (xmaLocksHeld counts it), or null.
template<class Slots> auto acquireContext(Slots& contexts,unsigned index) -> decltype(contexts[index].get()) {
    PoolGuard pool;
    auto* context=contexts[index].get();
    if (!context || context->dying.load(std::memory_order_relaxed)) return nullptr;
    context->lock.lock();++xmaLocksHeld;
    return context;
}
template<class Context> void releaseContext(Context* context) {--xmaLocksHeld;context->lock.unlock();}
}
// v168 (goal step 1, baseline profile 2026-09-27): every guest voice allocates a context, and every new
// context's decoder opened the codec: ~25% of the audio engine thread's CPU was FFmpeg's table building
// (split_radix_permutation, sincos, ff_mdct_init, build_table, ff_init_vlc_sparse). Released decoders are
// flushed (NativeXmaFrameDecoder::reset, the state a context clear already relies on as "fresh") and parked
// by their configured format; a context takes one of its stream's format instead of reopening the codec.
// Bounded (PerFormat per format, Fresh unconfigured). files/private-trace/diag-xma-no-decoder-reuse: v167.
namespace {
struct DecoderCache {
    static constexpr std::size_t PerFormat = 32, Fresh = 32;
    std::mutex lock;
    std::array<std::vector<std::unique_ptr<NativeXmaFrameDecoder>>, 8> formats;
    std::vector<std::unique_ptr<NativeXmaFrameDecoder>> fresh;
};
DecoderCache& decoderCache() { static DecoderCache cache; return cache; }
bool decoderReuse() {
    static const bool enabled = ::access(RECOMP_APP_PATH("/private-trace/diag-xma-no-decoder-reuse"), F_OK) != 0;
    return enabled;
}
int decoderFormat(std::uint32_t rate, unsigned channels) {
    const int r = rate == 24000 ? 0 : rate == 32000 ? 1 : rate == 44100 ? 2 : rate == 48000 ? 3 : -1;
    return r < 0 || channels < 1 || channels > 2 ? -1 : r * 2 + int(channels - 1);
}
std::unique_ptr<NativeXmaFrameDecoder> takeDecoder(int format) {
    if (!decoderReuse()) return nullptr;
    auto& cache = decoderCache();
    std::lock_guard guard(cache.lock);
    auto& list = format < 0 ? cache.fresh : cache.formats[std::size_t(format)];
    if (list.empty()) return nullptr;
    auto decoder = std::move(list.back());
    list.pop_back();
    return decoder;
}
void parkDecoder(std::unique_ptr<NativeXmaFrameDecoder> decoder, int format) {
    if (!decoder || !decoderReuse()) return;
    decoder->reset(); // the flushed state a context clear (command 3) already treats as fresh
    auto& cache = decoderCache();
    std::lock_guard guard(cache.lock);
    auto& list = format < 0 ? cache.fresh : cache.formats[std::size_t(format)];
    if (list.size() < (format < 0 ? DecoderCache::Fresh : DecoderCache::PerFormat)) list.push_back(std::move(decoder));
}
}
struct NativeXmaContextPool::Context {
    std::mutex lock; // L_xma_ctx
    std::atomic<bool> dying{false}; // release() in progress; set under L_xma
    std::unique_ptr<NativeXmaFrameDecoder> decoder; // v168: recycled through decoderCache()
    int decoderFormat = -1; // v168: format the decoder is configured for (-1: not configured)
    std::array<std::uint8_t, 4096> compressed{};
    unsigned assembled = 0, length = 0, skipPackets = 0;
    NativeXmaFrameDecoder::Frame pcm;
    unsigned pcmBlock = 0, pcmEndBlock = 0;
    bool loopStartSkipPending = false, frameIsLoopEnd = false;
    // v72: the next decoded frame is the first since allocation/clear (start subframe skip).
    bool startSkipPending = true;
};
// v72 test seam: -1 reads the switch file again on next use, 0 forces v71, 1 forces the skip.
void nativeXmaStartSkipForTest(int mode) noexcept { startSkipMode.store(mode<0 ? -1 : mode!=0,std::memory_order_release); }
unsigned long long nativeXmaStartSkipAppliedForTest() noexcept { return startSkipApplied.load(); }
// DIAGNOSTIC v72: log the pool's decoder state for the context at a guest virtual address.
void diagnosticDumpXmaContext(std::uint32_t address) {
    const auto base=diagnosticXmaVirtualBase.load(std::memory_order_relaxed);
    if (!base || address<base || (address-base)%NativeXmaContextPool::ContextBytes ||
        address-base>=NativeXmaContextPool::PoolBytes) {
        __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_XMA_CTX address=%08x outside pool base=%08x",address,base);
        return;
    }
    const unsigned i=(address-base)/NativeXmaContextPool::ContextBytes;
    const auto& s=diagnosticXmaStates[i];
    bool kicked=false;
    {
        PoolGuard pool;
        const auto* owner=diagnosticXmaPool.load(std::memory_order_relaxed);
        auto& all=diagnosticKicked();
        const auto found=all.find(owner);
        kicked=found!=all.end() && found->second[i];
    }
    const unsigned f=s.flags.load(std::memory_order_relaxed);
    __android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
        "RENDER_DIAG_XMA_CTX ctx=%u kicked=%u decodes=%u assembled=%u length=%u skip_packets=%u pcm=%u/%u loop_skip_pending=%u "
        "loop_end_frame=%u start_skip_pending=%u start_skip_mode=%d",
        i,kicked,s.decodes.load(std::memory_order_relaxed),s.assembled.load(std::memory_order_relaxed),
        s.length.load(std::memory_order_relaxed),s.skipPackets.load(std::memory_order_relaxed),
        s.pcmBlock.load(std::memory_order_relaxed),s.pcmEndBlock.load(std::memory_order_relaxed),
        f&1,(f>>1)&1,(f>>2)&1,startSkipMode.load(std::memory_order_relaxed));
}
void diagnosticDumpXma() {
    if(!diagnosticXmaMutex.try_lock())return;std::lock_guard lock(diagnosticXmaMutex,std::adopt_lock);
    const auto now=std::chrono::steady_clock::now();
    for(unsigned k=0;k<64;++k){const auto& e=diagnosticXmaEvents[(diagnosticXmaNext+k)%64];if(e.op=='?')continue;
        __android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
            "RENDER_DIAG_XMA_EVENT op=%c ctx=%u valid=%u enabled=%u write=%u read=%u cap=%u w0=%08x w1=%08x w9=%08x age_ms=%lld total=%llu",
            e.op,e.context,(e.w0>>20)&3,e.w1>>31,(e.w0>>27)&31,e.w9&31,(e.w0>>22)&31,e.w0,e.w1,e.w9,
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(now-e.at).count(),diagnosticXmaTotal);}
}
// Test/diagnostic seam: XMA locks the calling thread holds (0 whenever map_ is called).
unsigned nativeXmaLocksHeldForTest() noexcept { return xmaLocksHeld; }
NativeXmaContextPool::NativeXmaContextPool(std::uint32_t v, std::uint32_t p,
        std::uint8_t* storage, MapPhysical map)
    : virtualBase_(v), physicalBase_(p), storage_(storage), map_(std::move(map)) {
    if (ready()) std::memset(storage_,0,PoolBytes);
    diagnosticXmaVirtualBase.store(v,std::memory_order_relaxed); // DIAGNOSTIC v72 (the runner owns one pool)
    diagnosticXmaPool.store(this,std::memory_order_relaxed);
}
NativeXmaContextPool::~NativeXmaContextPool() {
    const void* self=this;
    diagnosticXmaPool.compare_exchange_strong(self,nullptr);
}
bool NativeXmaContextPool::ready() const {
    // Context words are accessed as aligned 32-bit atomics (the arena is page-aligned in practice).
    return storage_ && !(reinterpret_cast<std::uintptr_t>(storage_) & 3) && map_ && virtualBase_ && physicalBase_ &&
        !(virtualBase_ & 255) && !(physicalBase_ & 255) &&
        std::uint64_t(virtualBase_) + PoolBytes <= 0x100000000ULL &&
        std::uint64_t(physicalBase_) + PoolBytes <= 0x100000000ULL;
}
// Reports the current call's failure through callFailure; access() records the pool's first.
bool NativeXmaContextPool::fail(const char* why) { callFailure = why; return false; }
std::uint32_t NativeXmaContextPool::allocate() {
    if (!ready()) return 0;
    PoolGuard pool;
    for (unsigned i=0;i<ContextCount;++i) if (!contexts_[i]) {
        auto c=std::make_unique<Context>();
        c->decoder=takeDecoder(-1);
        if (!c->decoder) c->decoder=std::make_unique<NativeXmaFrameDecoder>();
        if (!c->decoder->available()) return 0;
        (void)startSkipEnabled(); // v72: read the switch (and log it) once, at the first context
        scrubContext(storage_+i*ContextBytes);
        contexts_[i]=std::move(c);
        diagnosticXmaRecord('A',i,storage_+i*ContextBytes); // DIAGNOSTIC v697
        return virtualBase_+i*ContextBytes;
    }
    return 0;
}
bool NativeXmaContextPool::release(std::uint32_t v) {
    if (v < virtualBase_ || (v-virtualBase_)%ContextBytes || v-virtualBase_>=PoolBytes) return false;
    const auto i=(v-virtualBase_)/ContextBytes;
    Context* context=nullptr;
    {
        PoolGuard pool;
        context=contexts_[i].get();
        if (!context || context->dying.exchange(true)) return false;
        diagnosticXmaRecord('R',i,storage_+i*ContextBytes); // DIAGNOSTIC v697 (state before zeroing)
        pending_[i]=false; diagnosticKicked()[this].reset(i); // DIAGNOSTIC v696
    }
    // Wait for an in-flight decode of this context, then scrub while no decode can start.
    context->lock.lock();++xmaLocksHeld;
    scrubContext(storage_+i*ContextBytes);
    releaseContext(context);
    std::unique_ptr<Context> doomed;
    {PoolGuard pool;doomed=std::move(contexts_[i]);}
    // A decode that took the pointer before the slot was emptied holds its lock: let it finish.
    {std::lock_guard straggler(doomed->lock);}
    parkDecoder(std::move(doomed->decoder),doomed->decoderFormat); // v168: no decode can reach it now
    return true;
}
bool NativeXmaContextPool::access(std::uint32_t address, bool write, std::uint32_t& value) {
    if constexpr (!KeepFirstFailure) {
        // Cooperative contract: failure()/failureContext() describe the latest access.
        if (!failure_.empty() || failureContext_!=std::array<std::uint32_t,16>{}) {
            PoolGuard pool;failure_.clear();failureContext_={};
        }
    }
    const auto failed=[&](const char* why,const std::uint8_t* context=nullptr) {
        PoolGuard pool;
        if (!KeepFirstFailure || failure_.empty()) {
            failure_=why?why:"XMA failure";
            failureContext_=context?Words(context).w:std::array<std::uint32_t,16>{};
        }
        return false;
    };
    if (!ready() || (address&3)) return failed("XMA register binding/alignment");
    if (!write) {
        if (address==0x7FEA1800) { value=__builtin_bswap32(physicalBase_); return true; }
        if (address==0x7FEA1804) { PoolGuard pool; value=__builtin_bswap32(control_); return true; }
        // Kicks decode synchronously under the context lock; keep the actual last processed ID,
        // without an active (0x200) bit; never rotate IDs to satisfy polling.
        if (address==0x7FEA1818) {
            // DIAGNOSTIC v696: hardware decodes kicked contexts continuously; the
            // guest's progress poll is the synchronization point for that work.
            std::array<std::uint16_t,ContextCount> active{};unsigned count=0;
            {
                PoolGuard pool;
                if (control_==3) {
                    const auto& kicked=diagnosticKicked()[this];
                    for (unsigned i=0;i<ContextCount;++i) if (kicked[i] && contexts_[i] && !pending_[i]) active[count++]=i;
                }
            }
            const bool pumpFast=(recomp_au_switches.load(std::memory_order_relaxed)&4U)!=0; // audio-cost bit 2
            unsigned long long skipped=0;
            for (unsigned k=0;k<count;++k) {
                const unsigned i=active[k];
                // audio-cost bit 2: decode() would return true here without effects (see pumpIdle); xmaLocksHeld is 0.
                if (pumpFast && pumpIdle(Words(storage_+i*ContextBytes),map_)) { ++skipped; continue; }
                if (!decode(i)) {
                    {PoolGuard pool;diagnosticKicked()[this].reset(i);}
                    const auto failures=++diagnosticPumpFailures;
                    if (failures<=8)__android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_XMA_PUMP_FAILED context=%u why=%s",i,callFailure?callFailure:"");
                } else if (callPcmBytes) {
                    const auto pumps=++diagnosticPumps;
                    if (!(pumps&(pumps-1))) {
                        unsigned long long total=0;{PoolGuard pool;total=writtenPcmBytes_;}
                        __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_XMA_PUMP decodes=%llu context=%u pcm_bytes=%llu",pumps,i,total);
                    }
                }
            }
            if (pumpFast) { // audio-cost: counters for RENDER_DIAG_AU (relaxed; one add each per poll)
                if (skipped) recomp_au_pump_skipped.fetch_add(skipped,std::memory_order_relaxed);
                if (count>skipped) recomp_au_pump_decoded.fetch_add(count-skipped,std::memory_order_relaxed);
            }
            PoolGuard pool;value=__builtin_bswap32(lastContext_); return true;
        }
        return failed("Unimplemented XMA register read");
    }
    if (address==0x7FEA1804) {
        const auto control=__builtin_bswap32(value);
        // Original 8365A420 brackets its complete context-copy sweep with 2/3.
        // Native equivalent: quiesce/resume. Kicks while paused are deferred;
        // resume decodes them. No guest context flags or completion counters are invented here.
        if (control!=2 && control!=3) return failed("Unsupported XMA global control mode");
        std::array<std::uint16_t,ContextCount> resume{};unsigned count=0;
        {
            PoolGuard pool;control_=control;
            if (control_==3) for (unsigned i=0;i<ContextCount;++i) if (pending_[i]) resume[count++]=i;
        }
        for (unsigned k=0;k<count;++k) {
            const unsigned i=resume[k];
            if (!decode(i)) return failed(callFailure,storage_+i*ContextBytes);
            PoolGuard pool;pending_[i]=false;lastContext_=i;
        }
        return true;
    }
    unsigned command=0, first=0;
    if (address>=0x7FEA1940 && address<0x7FEA1968) { command=1; first=(address-0x7FEA1940)*8; }
    if (address>=0x7FEA1A40 && address<0x7FEA1A68) { command=2; first=(address-0x7FEA1A40)*8; }
    if (address>=0x7FEA1A80 && address<0x7FEA1AA8) { command=3; first=(address-0x7FEA1A80)*8; }
    if (!command) return failed("Unimplemented XMA register write");
    const auto mask=__builtin_bswap32(value);
    {
        bool allocated=true;
        {PoolGuard pool;for (unsigned b=0;b<32;++b) if ((mask&(1U<<b)) && !contexts_[first+b]) allocated=false;}
        if (!allocated) return failed("XMA command targets unallocated context");
    }
    for (unsigned b=0;b<32;++b) if (mask&(1U<<b)) {
        const auto i=first+b;
        bool decodeNow=false;
        {
            PoolGuard pool;
            diagnosticXmaRecord(command==1?'K':command==2?'L':'C',i,storage_+i*ContextBytes); // DIAGNOSTIC v697 (before)
            if (command==1) diagnosticKicked()[this].set(i); // DIAGNOSTIC v696
            if (command==2 || command==3) diagnosticKicked()[this].reset(i); // DIAGNOSTIC v696
            if (command==1) {
                if (control_==2) { pending_[i]=true; continue; }
                decodeNow=true;
            } else pending_[i]=false;
        }
        if (decodeNow) {
            if (!decode(i)) return failed(callFailure,storage_+i*ContextBytes);
            PoolGuard pool;lastContext_=i;
            continue;
        }
        // Lock waits for an in-flight decode of the context (Xenia Block); clear also resets it.
        auto* context=acquireContext(contexts_,i);
        if (!context) continue; // Released concurrently by its owner.
        if (command==3) {
            auto* stored=storage_+i*ContextBytes;
            keepWordBits(stored,~((3U<<20)|(31U<<27)));
            keepWordBits(stored+4,~(1U<<31));
            keepWordBits(stored+36,~31U);
            auto& c=*context; c.decoder->reset();
            c.assembled=c.length=c.skipPackets=0; c.pcmBlock=c.pcmEndBlock=0;
            c.loopStartSkipPending=c.frameIsLoopEnd=false;
            c.startSkipPending=true; // v72: a cleared context starts afresh
            diagnosticXmaStates[i].flags.store(4,std::memory_order_relaxed);
        }
        releaseContext(context);
    }
    return true;
}

// Returns false with callFailure set on a decode failure. True also covers "nothing to do",
// a context released concurrently, and buffers renamed by the guest since phase 1.
bool NativeXmaContextPool::decode(unsigned index) {
    callFailure=nullptr;callPcmBytes=0;
    const unsigned au=recomp_au_switches.load(std::memory_order_relaxed); // audio-cost: one setting per call
    auto* stored=storage_+index*ContextBytes;
    constexpr unsigned rates[]{24000,32000,44100,48000};
    // Phase 1, no XMA lock held: resolve the buffers this call can use (map_ may take R).
    DecodeMaps maps;
    {
        const Words s(stored);
        const auto loops=s.get(0,12,255),loopStart=s.get(3,0,0x3FFFFFF),loopEnd=s.get(4,0,0x3FFFFFF);
        const bool loopWindow=!loops || !(loopStart<32 || loopStart>loopEnd || s.get(1,17,7)>4 ||
            (loopStart==loopEnd && s.get(1,17,7)>s.get(1,12,3)));
        const unsigned capacity=s.get(0,22,31)*256,read=s.get(9,0,31)*256,write=s.get(0,27,31)*256;
        if (s.get(1,31,1) && !s.get(9,30,3) && loopWindow && capacity && read<capacity && write<capacity) {
            maps.output={s.w[7],capacity,map_(s.w[7],capacity,true),true};
            const unsigned freeBytes=write>=read ? capacity-write+read : read-write;
            const unsigned count=std::max(1U,s.get(1,20,15)),reserve=s.get(1,24,7)*256;
            if (freeBytes>=count*256+reserve)
                for (unsigned b=0;b<2;++b) if (s.valid(b) && s.get(b,0,4095)) {
                    const unsigned inputBytes=s.get(b,0,4095)*2048;
                    maps.input[b]={s.w[5+b],inputBytes,map_(s.w[5+b],inputBytes,false),true};
                }
        }
    }
    // Phase 2: L_xma -> L_xma_ctx hand-over-hand, then decode holding only L_xma_ctx.
    auto* context=acquireContext(contexts_,index);
    if (!context) return true;
    std::uint64_t frames=0,bytes=0;
    const bool ok=[&]()->bool {
        auto& c=*context;
        Words w(stored);
        Words baseline=w;
        const auto store=[&]{publish(stored,baseline,w);baseline=w;};
        if (!w.get(1,31,1)) return true;
        // Hardware interrupts need their own semantics; don't manufacture one.
        if (w.get(9,30,3)) return fail("XMA stop/interrupt mode not implemented");
        const auto loops=w.get(0,12,255);
        const auto loopStart=w.get(3,0,0x3FFFFFF), loopEnd=w.get(4,0,0x3FFFFFF);
        if (loops && (loopStart<32 || loopStart>loopEnd || w.get(1,17,7)>4 ||
            (loopStart==loopEnd && w.get(1,17,7)>w.get(1,12,3))))
            return fail("XMA invalid or empty loop window");
        const unsigned channels=1+w.get(1,29,1), capacity=w.get(0,22,31)*256;
        unsigned read=w.get(9,0,31)*256, write=w.get(0,27,31)*256;
        if (!capacity || read>=capacity || write>=capacity) return fail("XMA output ring bounds");
        if (!maps.output.names(w.w[7],capacity)) return true; // Renamed since phase 1.
        auto* output=maps.output.host;
        if (!output) return fail("XMA output physical allocation unavailable");
        unsigned freeBytes=write>=read ? capacity-write+read : read-write;
        // The SDK count is a consumption quantum in 256-byte blocks, NOT a limit
        // on the whole kick and NOT 128 samples per channel. Stereo has 8 blocks.
        const unsigned count=std::max(1U,w.get(1,20,15));
        const unsigned reserve=w.get(1,24,7)*256;
        if (freeBytes<count*256+reserve) return true;
        if (c.pcmBlock<c.pcmEndBlock && (c.pcm.channels!=channels || c.pcm.sampleRate!=rates[w.get(1,27,3)]))
            return fail("XMA format changed while decoded PCM remains pending");
        // v168: take an already-open decoder of this stream's format instead of reopening the codec.
        const int format=decoderFormat(rates[w.get(1,27,3)],channels);
        if (format!=c.decoderFormat && format>=0) {
            if (auto cached=takeDecoder(format)) {
                parkDecoder(std::move(c.decoder),c.decoderFormat);
                c.decoder=std::move(cached);c.decoderFormat=format;
            }
        }
        if (!c.decoder->configure(rates[w.get(1,27,3)],channels)) return fail("XMA codec configure failed");
        c.decoderFormat=format;
        unsigned steps=0;
        while (freeBytes>=count*256+reserve) {
            if (++steps>32768) return fail("XMA packet traversal exceeded physical buffer bound");
            if (c.pcmBlock<c.pcmEndBlock) {
                const auto blocks=std::min(count,c.pcmEndBlock-c.pcmBlock);
                if (!((au&2U) ? writeXmaPcmBlocksSpan(c.pcm,c.pcmBlock,blocks,output,capacity,write) // audio-cost bit 1
                              : writeXmaPcmBlocks(c.pcm,c.pcmBlock,blocks,output,capacity,write)))
                    return fail("XMA decoded PCM block bounds");
                c.pcmBlock+=blocks; freeBytes-=256*blocks;
                if (c.pcmBlock==c.pcmEndBlock) freeBytes-=reserve;
                bytes+=256*blocks; w.set(0,27,31,write/256);
                if (write==read) w.set(1,31,1,0);
                store();
                continue;
            }
            if (!w.valid(w.current())) {
                // Starvation leaves the output buffer valid. Only a full ring
                // (write==read after writing, above) clears bit31: the title's
                // available-data query (sub_836A96D8) reads a clear bit31 as a
                // FULL ring, and its gather loop then waits for PCM that was
                // never decoded (diagnostic runs 8dstjbv1/7dco19zd).
                if (!w.valid(w.current()^1)) { store(); break; }
                w.swap();
            }
            if (!w.packets()) return fail("XMA valid input has no packets");
            const auto inputBytes=w.packets()*2048;
            const auto buffer=w.current();
            if (!maps.input[buffer].names(w.w[5+buffer],inputBytes)) { store(); return true; } // Renamed.
            const auto* input=maps.input[buffer].host;
            if (!input) return fail("XMA input physical allocation unavailable");
            unsigned offset=w.offset();
            if (offset>=inputBytes*8) return fail("XMA input bit offset outside allocation");
            if (c.skipPackets) {
                const auto packetsLeft=w.packets()-offset/PacketBits;
                const auto skipped=std::min(c.skipPackets,packetsLeft);
                c.skipPackets-=skipped; offset+=skipped*PacketBits;
                if (offset>=inputBytes*8) { w.swap(); store(); continue; }
                w.offset(offset);
            }
            const unsigned packetIndex=offset/PacketBits;
            const auto* packet=input+packetIndex*2048;
            if ((packet[2]&7)>1) return fail("XMA unknown packet metadata");
            w.set(4,26,31,packet[2]&7);
            const bool continuation=c.assembled!=0;
            const auto first=firstFrame(packet);
            const bool newFrames=first<PacketBits &&
                ((packet[2]&7)!=1 || (packet[0]>>2)!=0);
            unsigned within=offset%PacketBits;
            if (!within) within=continuation ? 32 : (newFrames ? first : PacketBits);
            if (within>=PacketBits) {
                // This stream has no new frame starting in this packet.
                c.skipPackets=packet[3];
                const auto next=(packetIndex+1)*PacketBits;
                if (next>=inputBytes*8) w.swap(); else w.offset(next);
                store(); continue;
            }
            if (within<32) return fail("XMA frame offset inside packet header");
            if (!continuation) {
                // Loop end is the START of the final compressed frame, not its
                // end bit. Decode it before jumping, including when it spans packets.
                c.frameIsLoopEnd=w.get(0,12,255) && packetIndex*PacketBits+within==loopEnd;
            }
            if (au&1U) { // audio-cost bit 0: the loop below, one span per stop instead of one bit per iteration
                constexpr unsigned AssemblyBits=sizeof(c.compressed)*8;
                while (within<PacketBits && (!c.length || c.assembled<c.length)) {
                    if (c.assembled>=AssemblyBits) return fail("XMA compressed frame exceeds its assembly buffer");
                    // Up to the next point where the bit loop tests something: the header's 15th bit, the frame end,
                    // the packet end or the assembly bound (each span copies >= 1 bit: every limit is positive here).
                    unsigned n=std::min(PacketBits-within,AssemblyBits-c.assembled);
                    if (c.length) n=std::min(n,c.length-c.assembled);
                    if (c.assembled<15) n=std::min(n,15-c.assembled);
                    copyBitSpan(c.compressed.data(),c.assembled,packet,within,n);
                    within+=n; c.assembled+=n;
                    if (c.assembled==15) {
                        c.length=(unsigned(c.compressed[0])<<7)|(c.compressed[1]>>1);
                        if (c.length<16 || c.length>=0x7FFF) return fail("XMA invalid compressed frame length");
                    }
                }
            } else
            while (within<PacketBits && (!c.length || c.assembled<c.length)) {
                // A kick after a rejected zero length would otherwise keep assembling past the
                // buffer (the length check fires only at bit 15). Formerly a heap overflow.
                if (c.assembled>=c.compressed.size()*8) return fail("XMA compressed frame exceeds its assembly buffer");
                const unsigned v=bit(packet,within++), dst=c.assembled++;
                c.compressed[dst/8]=(c.compressed[dst/8]&~(1U<<(7-dst%8))) | (v<<(7-dst%8));
                if (c.assembled==15) {
                    c.length=(unsigned(c.compressed[0])<<7)|(c.compressed[1]>>1);
                    if (c.length<16 || c.length>=0x7FFF) return fail("XMA invalid compressed frame length");
                }
            }
            if (!c.length || c.assembled<c.length) {
                c.skipPackets=packet[3];
                const auto next=(packetIndex+1)*PacketBits;
                if (next>=inputBytes*8) {
                    if (c.frameIsLoopEnd)
                        return fail("XMA loop-end frame crosses input buffer ownership");
                    w.swap();
                } else w.offset(next);
                store(); continue;
            }
            if (continuation && newFrames && first!=within)
                return fail("XMA split-frame end disagrees with next packet frame offset");
            if (c.decoder->decode(c.compressed.data(),c.compressed.size(),0,c.length,c.pcm)
                    !=NativeXmaFrameDecoder::Result::Decoded) return fail("XMA actual compressed frame rejected by codec");
            ++frames;
            // Each subframe is 128 samples PER CHANNEL. The output ring's blocks
            // contain 128 scalar samples, so stereo skips/limits use twice as many.
            // Skip belongs only to the first frame AFTER a loop jump (including the
            // last finite repeat when the stored count has just become zero).
            c.pcmBlock=c.loopStartSkipPending ? w.get(1,17,7)*channels : 0;
            c.pcmEndBlock=c.frameIsLoopEnd ? (w.get(1,12,3)+1)*channels : 4*channels;
            // v72: the first frame since allocation/clear omits word 1 bits 24-26 subframes (see
            // the header comment). A loop jump cannot precede it; more than 4 skips this frame only.
            if (c.startSkipPending && !c.loopStartSkipPending && startSkipEnabled()) {
                const unsigned skip=std::min(w.get(1,24,7),4U);
                if (skip) {
                    c.pcmBlock=skip*channels;
                    const auto applied=++startSkipApplied;
                    if (applied<=16 || !(applied&(applied-1)))
                        __android_log_print(ANDROID_LOG_INFO,"RecompRenderer",
                            "RENDER_XMA_START_SKIP_APPLIED context=%u subframes=%u channels=%u frame_end_bit=%u end_block=%u applied=%llu",
                            index,skip,channels,packetIndex*PacketBits+within,c.pcmEndBlock,applied);
                }
            }
            c.startSkipPending=false;
            c.loopStartSkipPending=false;
            const bool follows=bit(c.compressed.data(),c.length-1);
            c.length=c.assembled=0;
            unsigned next=packetIndex*PacketBits+within;
            if (c.frameIsLoopEnd) {
                next=loopStart;
                c.skipPackets=0;
                c.loopStartSkipPending=true;
                if (w.get(0,12,255)<255) w.set(0,12,255,w.get(0,12,255)-1);
            } else if (continuation && newFrames) {
                // The trailer describes the packet where this frame STARTED.
                // A continuation packet can also contain newly starting frames,
                // even when the spanning frame's trailer is zero (Xenia frame_idx=-1).
                next=packetIndex*PacketBits+first;
            } else if (continuation || !follows || within==PacketBits) {
                c.skipPackets=packet[3]; next=(packetIndex+1)*PacketBits;
            }
            if (next>=inputBytes*8) w.swap(); else w.offset(next);
            store();
        }
        return true;
    }();
    {   // DIAGNOSTIC v72: publish this context's decoder state for the audio spin probe.
        auto& d=diagnosticXmaStates[index];const auto& c=*context;
        d.assembled.store(c.assembled,std::memory_order_relaxed);d.length.store(c.length,std::memory_order_relaxed);
        d.skipPackets.store(c.skipPackets,std::memory_order_relaxed);d.pcmBlock.store(c.pcmBlock,std::memory_order_relaxed);
        d.pcmEndBlock.store(c.pcmEndBlock,std::memory_order_relaxed);
        d.flags.store(unsigned(c.loopStartSkipPending)|unsigned(c.frameIsLoopEnd)<<1|unsigned(c.startSkipPending)<<2,std::memory_order_relaxed);
        d.decodes.fetch_add(1,std::memory_order_relaxed);
    }
    releaseContext(context);
    callPcmBytes=bytes;
    if (frames || bytes) { PoolGuard pool; decodedFrames_+=frames; writtenPcmBytes_+=bytes; }
    return ok;
}
}
