#pragma once
// Attempt v43, DIAGNOSTIC BUILDS ONLY: pieces of the render thread's per-draw capture path kept free of
// the registry so they can be checked on the host (local-analysis/attempts/v43/tests).
//  - big-endian index conversion straight from the guest bytes into a reused host buffer;
//  - the generic renderer's shader-code collection (v35 genericShaderCode) over in-place readers, with the
//    microcode kept across draws and reused only while the guest bytes are identical (full compare).
// Readers ("views") return a pointer to the guest bytes with the registry copy's validation, or nullptr
// where the copy would have been refused; the bytes stay valid (guest mappings are permanent).
#include "gta5_shader_resource.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <atomic>
#if defined(__aarch64__)
#include <arm_neon.h> // v208
#endif
#include <optional>
#include <unordered_map>
#include <vector>

extern "C" __attribute__((weak)) std::atomic<bool> recomp_v233_baseline; // (generic_renderer.cpp; absent in host tests)
namespace recomp::gpu::hot {
// v233: node pointers of recently used unordered_map entries (64 direct-mapped slots) in front of the per-draw lookups below
// (v227 outdoor profile: their operator[] paths 1.3% of the render thread). An unordered_map keeps references to its
// elements across rehashes; a slot is only trusted for its own key, and every slot is dropped when the map is cleared.
// diag-v233-baseline: the plain lookups.
inline bool v233Baseline(){return &recomp_v233_baseline && recomp_v233_baseline.load(std::memory_order_relaxed);}
template<class Key,class Value>
struct RecentEntries {
    struct Slot {Key key{};Value* entry=nullptr;};
    std::array<Slot,1024> slots{}; // (v290: 1024, v233-v289: 64; v284 outdoor render profile: misses to the map 0.64%)
    template<class Map> Value& get(Map& map,Key key) {
        if(v233Baseline())return map[key];
        auto& slot=slots[(std::uint64_t(key)*0x9E3779B97F4A7C15ull)>>54];
#if defined(RECOMP_V233_MUTANT_NO_KEY)
        if(slot.entry)return *slot.entry; // test mutant: a slot trusted without its key
#else
        if(slot.entry && slot.key==key)return *slot.entry;
#endif
        Value& v=map[key];slot={key,&v};return v;
    }
    void clear(){slots={};}
};
static_assert(std::endian::native==std::endian::little,"the conversions below byte-swap big-endian guest words");

inline std::uint32_t be32(const std::uint8_t* p) {std::uint32_t v;std::memcpy(&v,p,4);return __builtin_bswap32(v);}

// v35 assembled each index byte by byte into a push_back loop; the same values, vectorisable.
// v208: NEON for both widths (the v190/v207 profiles showed the 16-bit loop scalar, ~2% of the render thread): 8 or 4
// indices per step (byte reverse within each index, 16-bit widened to 32), the scalar loop for the rest.
inline void bigEndianIndices(const std::uint8_t* __restrict src,std::size_t count,bool index32,std::uint32_t* __restrict out) {
    std::size_t i=0;
#if defined(__aarch64__) && !defined(RECOMP_HOT_PATH_SCALAR_INDICES)
    if(index32) {
        for(;i+4<=count;i+=4)vst1q_u32(out+i,vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(src+4*i))));
    } else {
        for(;i+8<=count;i+=8) {
            const uint16x8_t v=vreinterpretq_u16_u8(vrev16q_u8(vld1q_u8(src+2*i)));
            vst1q_u32(out+i,vmovl_u16(vget_low_u16(v)));vst1q_u32(out+i+4,vmovl_high_u16(v));
        }
    }
#endif
    if(index32)for(;i<count;++i){std::uint32_t v;std::memcpy(&v,src+4*i,4);out[i]=__builtin_bswap32(v);}
    else for(;i<count;++i){std::uint16_t v;std::memcpy(&v,src+2*i,2);out[i]=__builtin_bswap16(v);}
}
// Inline (UP) draws: indices relative to the first vertex, clamped at 0 (v>=min ? v-min : 0).
inline void bigEndianIndicesRebased(const std::uint8_t* __restrict src,std::size_t count,bool index32,std::uint32_t minVertex,
        std::uint32_t* __restrict out) {
    bigEndianIndices(src,count,index32,out);
    for(std::size_t i=0;i<count;++i)out[i]=out[i]>=minVertex?out[i]-minVertex:0;
}

// Microcode by (address, length). A hit needs the full current guest bytes to equal the kept bytes;
// otherwise the entry is refreshed from them. `generation` is unique per content version (never reused,
// also across clears), `known` caches the pack lookup for these exact bytes (-1 not asked yet).
struct CachedCode {std::vector<std::uint8_t> bytes;std::uint64_t generation=0;signed char known=-1;
    std::uint64_t watchStamp=~0ull;}; // v264: the write-watch stamp of the bytes when they were last known equal (~0: none)
// v264: the write watch's stamp of a host range (~0ull: not watched or written recently), set by the generic renderer when
// its watch is installed (guest_write_watch.h). The stamp is taken before the bytes are compared or copied.
inline std::uint64_t (*shaderCodeStamp)(const std::uint8_t*,std::uint32_t)=nullptr;
class ShaderCodeCache {
public:
    template<class View>
    CachedCode* get(std::uint32_t address,std::uint32_t length,const View& view) {
        const std::uint8_t* p=view(address,length);
        if(!p)return nullptr;
        auto& e=recent_.get(entries_,(std::uint64_t{address}<<32)|length);
        const std::uint64_t stamp=shaderCodeStamp?shaderCodeStamp(p,length):~0ull;
        if(e.generation && e.bytes.size()==length) {
            // v264: pages not written since the bytes were last equal hold the same bytes (exact): no comparison
#if !defined(RECOMP_V264_MUTANT_STAMP_ONLY)
            if(stamp!=~0ull && e.watchStamp==stamp){++hits;++stampHits;return &e;}
            if(!std::memcmp(e.bytes.data(),p,length)){++hits;e.watchStamp=stamp;return &e;}
#else // test mutant: a changed stamp keeps the cached bytes without comparing them
            if(stamp!=~0ull){++hits;++stampHits;e.watchStamp=stamp;return &e;}
            if(!std::memcmp(e.bytes.data(),p,length)){++hits;e.watchStamp=stamp;return &e;}
#endif
        }
        e.bytes.assign(p,p+length);e.generation=++generation_;e.known=-1;e.watchStamp=stamp;++misses;
        return &e;
    }
    std::uint64_t stampHits=0; // v264
    // Between draws only: entries handed out by get() stay valid until the next trim.
    void trim(std::size_t limit){if(entries_.size()>limit){entries_.clear();recent_.clear();++clears;}}
    std::size_t size()const noexcept{return entries_.size();}
    std::uint64_t hits=0,misses=0,clears=0;
    ShaderCodeCache()=default;ShaderCodeCache(const ShaderCodeCache&)=delete;ShaderCodeCache& operator=(const ShaderCodeCache&)=delete; // (v233: slots point into entries_)
private:
    std::unordered_map<std::uint64_t,CachedCode> entries_;
    RecentEntries<std::uint64_t,CachedCode> recent_; // v233
    std::uint64_t generation_=0;
};

// The v29/v32 word source of genericShaderCode: each object's header block copied once (VS 1152 bytes,
// PS 72), adjacent descriptor word pairs read as 8 bytes (at most 16 per call), anything else one word.
// Copies go to fixed buffers (the same bytes the v35 vectors held), so later words of the same call see
// one snapshot of each block exactly as before.
class ShaderWords {
public:
    template<class HeapView>
    void begin(std::uint32_t vs,std::uint32_t ps,const HeapView& heap) {
        pairCount_=0;load(blocks_[0],vs,1152,heap);load(blocks_[1],ps,72,heap);
    }
    template<class HeapView>
    std::optional<std::uint32_t> read(std::uint32_t a,const HeapView& heap) {
        for(const auto& b:blocks_)
            if(b.valid && a>=b.base && std::uint64_t{a}+4<=std::uint64_t{b.base}+b.size)return be32(b.bytes.data()+(a-b.base));
        for(unsigned i=0;i<pairCount_;++i) {
            const auto& p=pairs_[i];
            if(a>=p.base && std::uint64_t{a}+4<=std::uint64_t{p.base}+8)return be32(p.bytes+(a-p.base));
        }
        if(!(a&3) && a<=0xFFFFFFF4u && pairCount_<16) {
            if(const auto* p=heap(a,8)) {
                auto& pair=pairs_[pairCount_++];pair.base=a;std::memcpy(pair.bytes,p,8);
                return be32(pair.bytes);
            }
        }
        const auto* p=heap(a,4);
        if(!p)return std::nullopt;
        return be32(p);
    }
private:
    struct Block {std::uint32_t base=0,size=0;bool valid=false;std::array<std::uint8_t,1152> bytes{};};
    struct Pair {std::uint32_t base=0;std::uint8_t bytes[8]{};};
    template<class HeapView>
    static void load(Block& b,std::uint32_t object,std::uint32_t size,const HeapView& heap) {
        b.valid=false;
        if(!object || (object&3))return;
        const auto* p=heap(object,size);
        if(!p)return;
        b.base=object;b.size=size;std::memcpy(b.bytes.data(),p,size);b.valid=true;
    }
    std::array<Block,2> blocks_{};
    std::array<Pair,16> pairs_{};
    unsigned pairCount_=0;
};

// The vertex code of the created variant (the pack key), the patched copy selected for this draw, the
// other stored variants draw() may fall back to, and the pixel code. Pointers stay valid until the next
// collectShaderCode call; `alternates` is never null.
struct ShaderCodeResult {
    const std::vector<std::uint8_t>* identity=nullptr;
    const std::vector<std::uint8_t>* effective=nullptr;
    const std::vector<std::uint8_t>* pixel=nullptr;
    const std::vector<std::vector<std::uint8_t>>* alternates=nullptr;
    const char* pixelWhy=nullptr; // v48 (from v41): why `pixel` is null: a null pixel shader, header or code unreadable
    // v78: the cache generations of `identity` and `pixel` (0 when null): unique per content version, never reused,
    // so a generation always names the same bytes (generic::DrawInput::vertexIdentityVersion / pixelVersion).
    std::uint64_t identityGeneration=0,pixelGeneration=0;
    std::uint64_t effectiveVersion=0; // v78: set by the caller (ContentVersions) for `effective`, 0 unknown
};
struct ShaderCodeState {
    ShaderCodeCache cache;
    ShaderWords words;
    // v35 rebuilt the alternates (the selected variant's code when it differs from variant 0) per draw;
    // kept per vertex object for the exact code versions it was built from.
    struct Memo {std::uint64_t identity=0,second=0;std::vector<std::vector<std::uint8_t>> alternates;};
    std::unordered_map<std::uint32_t,Memo> memo;
    RecentEntries<std::uint32_t,Memo> recentMemo; // v233
    std::vector<std::vector<std::uint8_t>> scratch;
    std::uint64_t memoHits=0,memoMisses=0,unknownWalks=0;
    void trim(std::size_t limit){cache.trim(limit);if(memo.size()>limit){memo.clear();recentMemo.clear();}}
};
// v78: a content version for bytes the caller keeps elsewhere and may rewrite in place (the selected vertex binding's
// patched code): the same version while the bytes equal the last ones seen for `key`, a new one when they differ.
// Versions carry bit 62, so they never equal a ShaderCodeCache generation (the renderer keeps both in one map).
class ContentVersions {
public:
    std::uint64_t version(std::uint64_t key,const std::vector<std::uint8_t>& bytes) {
        auto& e=recent_.get(entries_,key);
        if(e.version && e.bytes==bytes){++hits;return e.version;}
        e.bytes=bytes;e.version=(std::uint64_t{1}<<62)|++next_;++changes;
        return e.version;
    }
    void trim(std::size_t limit){if(entries_.size()>limit){entries_.clear();recent_.clear();++trims;}}
    std::uint64_t hits=0,changes=0;
    std::uint64_t trims=0; // v366d: clears so far (a caller's memo of an answer is void once the entries were cleared)
private:
    struct Entry {std::vector<std::uint8_t> bytes;std::uint64_t version=0;};
    std::unordered_map<std::uint64_t,Entry> entries_;
    RecentEntries<std::uint64_t,Entry> recent_; // v233
    std::uint64_t next_=0;
};
// Same selection as v35 genericShaderCode: variant 0 and the selected variant (the first readable one is
// the identity, a different second one an alternate); the other variants only when neither the identity
// nor the patched code is known to the pack; then the pixel code. v35 read variant 0 twice when the
// selected variant was 0 and dropped the second copy when it was equal; that second read is skipped.
template<class HeapView,class DataView,class Known>
ShaderCodeResult collectShaderCode(std::uint32_t vs,std::uint32_t ps,const std::vector<std::uint8_t>* effective,unsigned variant,
        const HeapView& heap,const DataView& data,const Known& vertexKnown,ShaderCodeState& st) {
    ShaderCodeResult r;r.effective=effective;
    st.words.begin(vs,ps,heap);
    const auto word=[&](std::uint32_t a){return st.words.read(a,heap);};
    CachedCode* identity=nullptr;CachedCode* second=nullptr;
    if(const auto range=describeGta5ShaderCode(vs,0,0,word))identity=st.cache.get(range->virtualAddress,range->byteCount,data);
    if(variant)if(const auto range=describeGta5ShaderCode(vs,0,variant,word))second=st.cache.get(range->virtualAddress,range->byteCount,data);
    if(!identity){identity=second;second=nullptr;}
    auto& memo=st.recentMemo.get(st.memo,vs);
    const std::uint64_t idGen=identity?identity->generation:0,secondGen=second?second->generation:0;
    if(memo.identity!=idGen || memo.second!=secondGen) {
        memo.alternates.clear();
        if(identity && second && second!=identity && second->bytes!=identity->bytes)memo.alternates.push_back(second->bytes);
        memo.identity=idGen;memo.second=secondGen;++st.memoMisses;
    } else ++st.memoHits;
    r.identity=identity?&identity->bytes:nullptr;r.identityGeneration=identity?identity->generation:0;
    r.alternates=&memo.alternates;
    bool known=false;
    if(identity) {
        if(identity->known<0)identity->known=vertexKnown(identity->bytes)?1:0;
        known=identity->known>0;
    }
    if(!known && effective)known=vertexKnown(*effective);
    if(!known) {
        ++st.unknownWalks;
        st.scratch=memo.alternates;
        for(unsigned v=1;v<32 && st.scratch.size()<8;++v)if(v!=variant)
            if(const auto range=describeGta5ShaderCode(vs,0,v,word))
                if(const auto* e=st.cache.get(range->virtualAddress,range->byteCount,data))
                    if(identity && e->bytes!=identity->bytes && std::find(st.scratch.begin(),st.scratch.end(),e->bytes)==st.scratch.end())
                        st.scratch.push_back(e->bytes);
        r.alternates=&st.scratch;
    }
    if(!ps)r.pixelWhy="ps-null";
    else if(const auto range=describeGta5ShaderCode(ps,1,0,word)) {
        if(const auto* e=st.cache.get(range->virtualAddress,range->byteCount,data)){r.pixel=&e->bytes;r.pixelGeneration=e->generation;}
        else r.pixelWhy="ps-code-unreadable";
    } else r.pixelWhy="ps-header-unreadable";
    return r;
}
}
