// Lane vfetch-index (2026-10-06): vertex fetches whose index the vertex shader COMPUTES from the vertex index.
// On the console a vfetch reads its stream at floor(register value); the translated module reads every input at the
// vertex's own index (its registers start at zero; lane shader-drops, RESULT.md row 30). For the shaders below the
// renderer therefore hands the module, per vertex and per stream, the record the microcode's index selects: the
// stream is laid out anew (record j of the bound buffer = record index(lo + j) of the guest stream), nothing else
// changes (formats, strides, offsets, bindings and the module stay as they are).
//
// The rules are GENERATED from the container microcode (tools/vfi_derive.py: a symbolic run up to every vfetch and on
// to the exports) into vfetch_index_rules.inc. A rule is in this table only when the layout is ALL the module lacks:
// a shader that also computes an export from the vertex index itself (ptxgpu_render#0v: each quad's corner) is not
// here, because the module has no vertex index (its derived rule is in the host tests, "not applicable"; so are the 24
// cloth shaders', whose module declares one input where the console fetches three). v598: the ptx row IS in the table now, applied
// only with v598's override module (r0.x = VertexIndex; generic_renderer.cpp ptxLoad / isPtxKey). Forms (I = the vertex index as a
// 32-bit float, every operation a 32-bit float operation in the microcode's own order):
//   Index   I                                   the vertex's own index (the stream is left alone)
//   Scale   I * k                               im#27v's per-car record (k = c251.w, 0x3ce38e39), the sprites' per-quad
//                                               record (k = 0.25)
//   Mod     I - n * trunc(I * k)                im#27v's template index (k as above, n = 36): mad(-trunc(k*I), n, I)
//   Fmod    n * frc(I * k)                      the sprites' corner index (HLSL fmod(I, 4): n = 4, k = rcp(4))
//   Remap   s * <component of fetch `via`>      im#27v's POSITION / NORMAL: the value TEXCOORD1's fetch left in the
//                                               register (s = 1); the cloth shaders' neighbour indices (s = 255.001953)
// and the fetch index is floor(value) (the vfetch's rounding bit is 0 in every one of them; a bound copy that sets it is
// refused). An index outside the stream reads zeros: the renderer's convention for bytes past a fetch constant's size
// (generic_renderer.cpp, "streams_truncated"); what the console returns there is not known.
// Nothing here touches Vulkan. Guest memory is read in ONE place, take(): everything a kept layout is made from and
// compared with is that copy (host tests: tests/).
#pragma once
#include "generic_renderer_cpu.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace recomp::gpu::generic::vfi {

enum class Form:std::uint8_t {Index,Scale,Mod,Fmod,Remap};
// One vfetch of the shader, in execution order. address: its instruction slot; dstRegister / writtenMask: the register
// it writes and which components (the bound copy must agree, or the rule is refused); srcRegister.srcComponent: where
// it reads its index; via / viaComponent (Remap): the earlier fetch whose result is the index, and the component of
// that fetch's destination register the value comes through (the BOUND copy's destination swizzle then says which
// fetched component that is); location: the element's SPIR-V input location (255: none).
struct FetchRule {std::uint16_t address;std::uint8_t dstRegister,writtenMask,srcRegister,srcComponent;Form form;std::uint32_t kBits,nBits;
    std::uint8_t via,viaComponent,location;};
struct Literal {std::uint8_t reg,component;std::uint32_t bits;}; // a shader literal an index is computed from
struct ShaderRule {std::uint64_t vs;unsigned count;FetchRule fetch[12];unsigned literalCount;Literal literal[8];};
inline constexpr ShaderRule Rules[]{
#include "vfetch_index_rules.inc"
};
// One predictable test for every other shader: a 64-bit filter over the keys' low six bits, then the few keys.
inline constexpr std::uint64_t KeyFilter=[]{std::uint64_t m=0;for(const auto& r:Rules)m|=std::uint64_t{1}<<(r.vs&63);return m;}();
inline const ShaderRule* find(std::uint64_t vs) noexcept {
    if(!((KeyFilter>>(vs&63))&1))return nullptr;
    for(const auto& r:Rules)if(r.vs==vs)return &r;
    return nullptr;
}
inline float bitsFloat(std::uint32_t b) noexcept {float f;std::memcpy(&f,&b,4);return f;}
constexpr std::uint32_t NoRecord=0xFFFFFFFFu; // an index no stream holds (negative, not a number, or 2^31 and above)

// floor(value) as the fetch index.
inline std::uint32_t fetchIndex(float value) noexcept {
    if(!(value>=0.0f) || value>=2147483648.0f)return NoRecord;
    return std::uint32_t(value); // (truncation = floor for a value >= 0)
}
// The value the microcode leaves in the register an arithmetic-form vfetch reads, for vertex index i. 32-bit float
// operations, one per statement (no contraction into a fused multiply-add: the products below are exact integers below
// 2^24 in every generated rule, so the console's own multiply-add, fused or not, gives the same value).
inline float registerValue(const FetchRule& r,std::uint32_t i) noexcept {
#pragma STDC FP_CONTRACT OFF
    const float I=float(i);
    switch(r.form) {
    case Form::Scale: {const float v=I*bitsFloat(r.kBits);return v;}
    case Form::Mod: {const float t=I*bitsFloat(r.kBits);const float y=std::trunc(t);const float m=-y*bitsFloat(r.nBits);const float v=m+I;return v;}
    case Form::Fmod: {const float t=I*bitsFloat(r.kBits);const float f=t-std::floor(t);const float v=bitsFloat(r.nBits)*f;return v;}
    default: return I;
    }
}
inline std::uint32_t arithmeticIndex(const FetchRule& r,std::uint32_t i) noexcept {
    return r.form==Form::Index?i:fetchIndex(registerValue(r,i));
}

// The bytes an element of a fetch format takes in its record (what the vertex input, or the CPU decode, reads of it).
inline unsigned elementBytes(unsigned format) noexcept {
    switch(format){case 6:case 7:case 16:case 17:case 25:case 31:case 33:case 36:return 4;case 26:case 32:case 34:case 37:return 8;case 57:return 12;default:return 16;}
}

// The records [first, end) a set of fetch indices selects (NoRecord selects none; end == first: no record at all).
struct Reach {std::uint32_t first=0,end=0;
    bool operator==(const Reach& o) const noexcept {return first==o.first && end==o.end;}};
inline Reach reachOf(const std::uint32_t* index,std::size_t count) noexcept {
    std::uint32_t lo=NoRecord,hi=0; // hi: the largest index + 1 (NoRecord + 1 wraps to 0: none)
    for(std::size_t j=0;j<count;++j){const std::uint32_t r=index[j],e=r+1u;lo=r<lo?r:lo;hi=e>hi?e:hi;}
    return hi?Reach{lo,hi}:Reach{};
}
// The bytes [first, second) of a stream of `size` bytes that hold the records of `reach` (cut at the stream's end).
inline std::pair<std::uint64_t,std::uint64_t> reachBytes(std::uint64_t size,unsigned stride,Reach reach) noexcept {
    const std::uint64_t b=std::uint64_t{reach.first}*stride,e=std::min<std::uint64_t>(size,std::uint64_t{reach.end}*stride);
    return {b,e>b?e:b};
}

// A run of a guest vertex stream's bytes: `size` readable bytes at `data`, records of `stride` bytes.
struct StreamView {const std::uint8_t* data=nullptr;std::uint64_t size=0;unsigned stride=0;};

// The bytes in the order the vertex input reads them: every whole dword swapped per `endian`, a dword the stream's end
// cuts zero-extended and swapped (cpu::swapCopy's rule for a truncated stream), `size` rounded up to a dword.
inline void swappedStream(std::vector<std::uint8_t>& out,const StreamView& s,unsigned endian) {
    out.resize(std::size_t((s.size+3)&~std::uint64_t{3}));
    if(!out.empty())cpu::swapCopy(out.data(),s.data,std::size_t(s.size),out.size(),endian);
}
// out[j] = record index[j] of the (already swapped) bytes [0, bytes) that begin at record `first` of the stream; bytes
// past their end read 0, and so does a record before `first` or NoRecord. Returns how many of the `count` records did
// not lie wholly inside. N: the stride when known at compile time (0: any).
template<unsigned N>
inline std::uint64_t expandLoop(std::uint8_t* out,const std::uint8_t* stream,std::uint64_t bytes,unsigned strideAny,
                                const std::uint32_t* index,std::size_t count,std::uint32_t first) noexcept {
    const std::size_t stride=N?N:strideAny;
    std::uint64_t outside=0;
    const std::uint64_t whole=stride?bytes/stride:0;
    for(std::size_t j=0;j<count;++j) {
        const std::uint32_t r=index[j]-first;std::uint8_t* o=out+j*stride; // (an index below `first` wraps to a record no window holds)
        if(r<whole && index[j]>=first){std::memcpy(o,stream+std::size_t{r}*stride,stride);continue;}
        ++outside;std::memset(o,0,stride);
        if(index[j]!=NoRecord && index[j]>=first && std::uint64_t{r}*stride<bytes)std::memcpy(o,stream+std::size_t{r}*stride,std::size_t(bytes-std::uint64_t{r}*stride));
    }
    return outside;
}
inline std::uint64_t expandRecords(std::uint8_t* out,const std::uint8_t* stream,std::uint64_t bytes,unsigned stride,
                                   const std::uint32_t* index,std::size_t count,std::uint32_t first=0) noexcept {
    switch(stride) { // (fixed-size moves for the strides the census shows: 4, 8, 16, 24; any other stride by memcpy)
    case 4: return expandLoop<4>(out,stream,bytes,stride,index,count,first);
    case 8: return expandLoop<8>(out,stream,bytes,stride,index,count,first);
    case 12: return expandLoop<12>(out,stream,bytes,stride,index,count,first);
    case 16: return expandLoop<16>(out,stream,bytes,stride,index,count,first);
    case 20: return expandLoop<20>(out,stream,bytes,stride,index,count,first);
    case 24: return expandLoop<24>(out,stream,bytes,stride,index,count,first);
    case 32: return expandLoop<32>(out,stream,bytes,stride,index,count,first);
    default: return expandLoop<0>(out,stream,bytes,stride,index,count,first);
    }
}

// Remap: the fetch `via` as the hardware reads it for one of its records. stream: via's raw guest bytes from record
// `first` on; endian: the fetch constant's own endian mode; offset: the element's byte offset in a record; fetch: its
// bound format fields; selector: the BOUND destination swizzle's selector at the rule's viaComponent (0-3 a fetched
// component, 4 zero, 5 one); scale: the literal the microcode multiplies it by (1 when none).
struct Via {StreamView stream;std::uint32_t first=0;unsigned endian=2,offset=0,selector=0;cpu::VFetch fetch;float scale=1.0f;};
inline float viaValue(const Via& v,std::uint32_t record) noexcept {
#pragma STDC FP_CONTRACT OFF
    std::array<std::uint8_t,16> raw{},swapped{};std::size_t take=0;
    if(record!=NoRecord && record>=v.first) {
        const std::uint64_t at=std::uint64_t{record-v.first}*v.stream.stride+v.offset;
        if(at<v.stream.size){take=std::size_t(std::min<std::uint64_t>(16,v.stream.size-at));std::memcpy(raw.data(),v.stream.data+at,take);}
    }
    cpu::swapCopy(swapped.data(),raw.data(),take,16,v.endian);
    cpu::VFetch f=v.fetch;f.feed=0x688;f.bgra=false; // the components as fetched: the bound swizzle is applied here
    const auto c=cpu::decodeAttribute(swapped.data(),f);
    const float x=v.selector<4?c[v.selector]:v.selector==4?0.0f:1.0f;
    if(v.scale==1.0f)return x;
    const float y=x*v.scale;return y;
}

// The record index of every vertex lo .. lo + count - 1 for one fetch of a rule. `viaIndex` (Remap): the records the
// fetch `via` reads for the same vertices (its own arithmetic index). table: scratch for the remap of via records
// (one decode per distinct via record the bytes hold, and one for every other record, which reads zeros).
inline void fetchIndices(const FetchRule& r,std::uint32_t lo,std::size_t count,std::uint32_t* out,const std::uint32_t* viaIndex=nullptr,
                         const Via* via=nullptr,std::vector<std::uint32_t>* table=nullptr) {
    if(r.form!=Form::Remap){for(std::size_t j=0;j<count;++j)out[j]=arithmeticIndex(r,lo+std::uint32_t(j));return;}
    constexpr std::uint32_t Unset=0xFFFFFFFEu;
    // records first .. first + limit - 1 can hold stream bytes; every other record (and NoRecord) decodes sixteen zero bytes
    const std::uint64_t limit=via->stream.stride?(via->stream.size+via->stream.stride-1)/via->stream.stride:(via->stream.size?1:0);
    const std::uint32_t slots=std::uint32_t(std::min<std::uint64_t>(limit,65536));
    if(table)table->assign(std::size_t{slots}+1,Unset);
    for(std::size_t j=0;j<count;++j) {
        const std::uint32_t v=viaIndex[j];const std::uint64_t local=v>=via->first && v!=NoRecord?std::uint64_t{v}-via->first:limit;
        if(table && (local<slots || local>=limit)){auto& t=(*table)[local<slots?std::size_t(local):slots];if(t==Unset)t=fetchIndex(viaValue(*via,v));out[j]=t;}
        else out[j]=fetchIndex(viaValue(*via,v));
    }
}

// The records of the vertices 0 .. n - 1 of an arithmetic-form fetch (a function of the rule alone: kept and grown as
// draws need them), and end[i] = the largest of index[0 .. i] + 1: the reach of a draw that starts at vertex 0.
constexpr std::size_t TableLimit=std::size_t{1}<<18; // (vertices; a longer draw computes its indices each time)
struct IndexTable {std::vector<std::uint32_t> index,end;};
inline void growTable(IndexTable& t,const FetchRule& r,std::size_t n) {
    const std::size_t old=t.index.size();
    if(old>=n)return;
    t.index.resize(n);t.end.resize(n);
    fetchIndices(r,std::uint32_t(old),n-old,t.index.data()+old);
    std::uint32_t e=old?t.end[old-1]:0;
    for(std::size_t i=old;i<n;++i){const std::uint32_t x=t.index[i]+1u;e=x>e?x:e;t.end[i]=e;}
}

// ---- guest memory is read HERE and nowhere else: the bytes of the records `reach` of one stream, copied once.
// at: where the stream's byte reach.first * stride is readable; size: the fetch constant's size. A layout and the key
// it is kept under are both made from this copy, so a guest write during the draw cannot part them.
struct Taken {std::vector<std::uint8_t> bytes;Reach reach;};
inline void take(Taken& t,const std::uint8_t* at,std::uint64_t size,unsigned stride,Reach reach) {
    const auto b=reachBytes(size,stride,reach);
    t.reach=reach;t.bytes.resize(std::size_t(b.second-b.first));
    if(!t.bytes.empty())std::memcpy(t.bytes.data(),at,t.bytes.size());
}

// ---- the kept layouts. One per stream of a rule draw, while the bytes it was made from are equal: the distant cars'
// template and remap layouts are reused every frame, the per-car layout is made anew when a car moves.
// Bounded: at most Entries layouts and `budget` bytes (vector capacities, checked after every layout); a layout that
// does not fit is made in the caller's scratch for the one draw. A stream whose content changed takes the place of
// its own older layout first (the old content is gone), a layout used in the current present is never replaced or
// released (an earlier stream of the same draw may still point at it), and a layout no draw used for a whole
// 60-present window is dropped (dropUnused).
struct Request {
    const FetchRule* rule=nullptr;std::uint32_t lo=0,count=0;unsigned stride=0,endian=0; // endian: how the vertex input's bytes are swapped
    const Taken* own=nullptr;              // the stream's bytes (records own->reach)
    const std::uint32_t* index=nullptr;    // arithmetic forms: the record of every vertex (count entries)
    const Via* via=nullptr;const Taken* viaTaken=nullptr;const std::uint32_t* viaIndex=nullptr; // Remap: via's fields but its bytes; those; via's records
    std::uint64_t frame=0,window=0;        // the present and the 60-present window of this draw
};
struct Kept {
    bool valid=false,remap=false;const FetchRule* rule=nullptr;std::uint32_t lo=0,count=0;unsigned stride=0,endian=0;
    Reach reach,viaReach;Via via; // (via: every field but the bytes, which viaSource holds)
    std::vector<std::uint8_t> source,viaSource,out;std::uint64_t outside=0,lastUse=0,frame=0,window=0;
    std::size_t bytes() const noexcept {return source.capacity()+viaSource.capacity()+out.capacity();}
    void release() noexcept {valid=false;std::vector<std::uint8_t>().swap(source);std::vector<std::uint8_t>().swap(viaSource);std::vector<std::uint8_t>().swap(out);}
};
struct Layout {const std::uint8_t* data=nullptr;std::uint64_t outside=0;enum How:std::uint8_t {Hit,Built,Uncached} how=Hit;};
struct LayoutCache {
    static constexpr std::size_t Entries=24;
    std::array<Kept,Entries> kept;
    std::size_t budget=std::size_t{8}<<20; // bytes of kept layouts and their sources
    std::uint64_t useSerial=0,dropped=0;
    std::vector<std::uint8_t> swapped;std::vector<std::uint32_t> remapIndex,remapTable;
    bool remapReady=false; // remapIndex holds the indices of the request remapReach() was just called for

    std::size_t bytes() const noexcept {std::size_t n=0;for(const auto& k:kept)n+=k.bytes();return n;}
    unsigned count() const noexcept {unsigned n=0;for(const auto& k:kept)n+=k.valid;return n;}
    // A layout no draw used during the window that just ended is dropped (its stream is gone).
    void dropUnused(std::uint64_t endedWindow) noexcept {
        for(auto& k:kept)if((k.valid || k.bytes()) && k.window<endedWindow){dropped+=k.valid;k.release();}
    }
    static bool sameVia(const Kept& k,const Request& q) noexcept {
        const auto& a=k.via;const auto& b=*q.via;const auto& f=a.fetch;const auto& h=b.fetch;
        return a.stream.stride==b.stream.stride && a.endian==b.endian && a.offset==b.offset && a.selector==b.selector && a.scale==b.scale &&
               f.format==h.format && f.isSigned==h.isSigned && f.isInteger==h.isInteger && f.noZero==h.noZero && f.expAdjust==h.expAdjust &&
               k.viaReach==q.viaTaken->reach && k.viaSource.size()==q.viaTaken->bytes.size() &&
               !std::memcmp(k.viaSource.data(),q.viaTaken->bytes.data(),k.viaSource.size());
    }
    // Whether a kept layout was made for this request's index definition: the same fetch, first vertex, stride, swap and
    // (Remap) the same via bytes, for at least as many vertices (a longer one serves when none of its records lay outside).
    static bool sameDefinition(const Kept& k,const Request& q) noexcept {
        return k.valid && k.rule==q.rule && k.lo==q.lo && k.count>=q.count && (k.count==q.count || !k.outside) && k.stride==q.stride &&
               k.endian==q.endian && k.remap==(q.via!=nullptr) && (!q.via || sameVia(k,q));
    }
    void remapIndices(const Request& q) {
        Via v=*q.via;v.stream.data=q.viaTaken->bytes.data();v.stream.size=q.viaTaken->bytes.size();v.first=q.viaTaken->reach.first;
        remapIndex.resize(q.count);fetchIndices(*q.rule,q.lo,q.count,remapIndex.data(),q.viaIndex,&v,&remapTable);
    }
    // Remap: the records of the request's own stream its vertices select: a function of via's bytes, so a kept layout
    // of the same definition gives it (for a shorter draw: a range that holds it); else the indices are made now.
    Reach remapReach(const Request& q) {
        remapReady=false;
        for(const auto& k:kept)if(sameDefinition(k,q))return k.reach;
        remapIndices(q);remapReady=true;
        return reachOf(remapIndex.data(),q.count);
    }
    // The layout made from the request's taken bytes alone. Returns the records that did not lie wholly inside.
    std::uint64_t build(std::vector<std::uint8_t>& out,const Request& q) {
        out.resize(std::size_t{q.count}*q.stride);
        const std::uint32_t* index=q.index;
        if(q.via){if(!remapReady)remapIndices(q);remapReady=false;index=remapIndex.data();}
        swappedStream(swapped,StreamView{q.own->bytes.data(),q.own->bytes.size(),q.stride},q.endian);
        return expandRecords(out.data(),swapped.data(),swapped.size(),q.stride,index,q.count,q.own->reach.first);
    }
    // Room for n bytes without a later reallocation: kept when it fits and is not more than twice the need, else new
    // (an eighth of headroom, so a car count that creeps up does not allocate every frame).
    static void fit(std::vector<std::uint8_t>& v,std::size_t n) {
        if(v.capacity()>=n && v.capacity()<=2*n+64)return;
        std::vector<std::uint8_t>().swap(v);
        if(n)v.reserve(n+n/8);
    }
    // The request's layout: a kept one whose bytes equal the taken ones, else made now and kept when it fits, else made
    // in `scratch` (which then holds it until the caller's next request for that scratch).
    Layout get(const Request& q,std::vector<std::uint8_t>& scratch) {
        Kept* victim=nullptr;int victimClass=3;
        for(auto& k:kept) {
            if(sameDefinition(k,q) && k.reach==q.own->reach && k.source.size()==q.own->bytes.size() &&
               !std::memcmp(k.source.data(),q.own->bytes.data(),k.source.size())) {
                k.lastUse=++useSerial;k.frame=q.frame;k.window=q.window;remapReady=false;
                return {k.out.data(),k.count==q.count?k.outside:0,Layout::Hit};
            }
            // 0: this fetch's layout of an earlier present (its stream changed: that content is gone); 1: unused;
            // 2: another fetch's layout of an earlier present; a layout used in this present is never replaced
            if(k.valid && k.frame==q.frame)continue;
            const int c=!k.valid?1:k.rule==q.rule?0:2;
            if(c<victimClass || (c==victimClass && k.lastUse<victim->lastUse)){victim=&k;victimClass=c;}
        }
        if(victim) {
            auto& k=*victim;k.valid=false;
            fit(k.out,std::size_t{q.count}*q.stride);fit(k.source,q.own->bytes.size());fit(k.viaSource,q.via?q.viaTaken->bytes.size():0);
            // within the budget once this layout is in: layouts of earlier presents go, oldest first, until it is
            std::size_t total=bytes();
            while(total>budget) {
                Kept* old=nullptr;
                for(auto& o:kept)if(&o!=victim && o.bytes() && o.frame!=q.frame && (!old || o.lastUse<old->lastUse))old=&o;
                if(!old)break;
                total-=old->bytes();dropped+=old->valid;old->release();
            }
            if(total<=budget) {
                k.source.assign(q.own->bytes.begin(),q.own->bytes.end());
                if(q.via)k.viaSource.assign(q.viaTaken->bytes.begin(),q.viaTaken->bytes.end());else k.viaSource.clear();
                k.outside=build(k.out,q);
                k.rule=q.rule;k.lo=q.lo;k.count=q.count;k.stride=q.stride;k.endian=q.endian;k.remap=q.via!=nullptr;k.reach=q.own->reach;
                if(q.via){k.via=*q.via;k.via.stream.data=nullptr;k.via.stream.size=0;k.viaReach=q.viaTaken->reach;}
                k.lastUse=++useSerial;k.frame=q.frame;k.window=q.window;k.valid=true;
                return {k.out.data(),k.outside,Layout::Built};
            }
            k.release();
        }
        const std::uint64_t outside=build(scratch,q);
        return {scratch.data(),outside,Layout::Uncached};
    }
};

// ---- the bound microcode against the rule: the fields the container hash leaves out (D3D patches them at bind time)
struct BoundFetch {bool vertexFetch=false;unsigned dstRegister=0,srcRegister=0,srcComponent=0,dstSwizzle=0,writtenMask=0;bool rounded=false,mini=false,predicated=false;};
inline BoundFetch boundFetch(const std::vector<std::uint8_t>& code,unsigned address) noexcept {
    BoundFetch b;const std::size_t at=std::size_t{address}*12;
    if(at+12>code.size())return b;
    const std::uint32_t w0=cpu::be32(code.data()+at),w1=cpu::be32(code.data()+at+4);
    b.vertexFetch=(w0&0x1Fu)==0;b.srcRegister=(w0>>5)&63u;b.dstRegister=(w0>>12)&63u;b.srcComponent=(w0>>30)&3u;
    b.dstSwizzle=w1&0xFFFu;b.rounded=(w1>>15)&1u;b.mini=(w1>>30)&1u;b.predicated=(w1>>31)&1u;
    for(unsigned k=0;k<4;++k)if(cpu::swizzleSelector(b.dstSwizzle,k)<=5)b.writtenMask|=1u<<k;
    return b;
}
inline bool sameIndex(const FetchRule& a,const FetchRule& b) noexcept {
    return a.form==b.form && a.kBits==b.kBits && a.nBits==b.nBits && a.via==b.via && a.viaComponent==b.viaComponent &&
           (a.form!=Form::Remap || (a.srcRegister==b.srcRegister && a.srcComponent==b.srcComponent));
}
// v621 (thor-fixer2, G14): the instruction slot rule fetch j occupies in the BOUND microcode. D3D's bind-time patch may reorder a
// shader's vfetches within its fetch run (by stream offset) and make some of them mini fetches: on the Thor, ped_cloth#3v's eight
// element fetches (slots 14-21) arrive as POSITION, BLENDWEIGHT, BLENDINDICES, NORMAL, COLOR0, TEXCOORD, TANGENT, COLOR1 where the
// container has ..., TEXCOORD, NORMAL, TANGENT, COLOR0, COLOR1. A vfetch is identified by the register it writes: the rule's own
// slot when the bound fetch there writes the rule's register (every bound copy v474-v620 accepted), else the one slot among the
// rule's vfetch slots (all of the shader's, permuted) whose bound fetch writes it. ~0u: none, or several.
inline unsigned boundSlot(const ShaderRule& rule,const std::vector<std::uint8_t>& code,unsigned j) noexcept {
    const auto& r=rule.fetch[j];
    {const auto b=boundFetch(code,r.address);if(b.vertexFetch && b.dstRegister==r.dstRegister)return r.address;}
#if defined(RECOMP_V621_MUTANT_ADDRESS_ONLY) // host-test mutant: v570-v620's lookup (the container's slot only)
    return ~0u;
#endif
    unsigned found=~0u;
    for(unsigned k=0;k<rule.count;++k) {
        const auto b=boundFetch(code,rule.fetch[k].address);
        if(b.vertexFetch && b.dstRegister==r.dstRegister){if(found!=~0u)return ~0u;found=rule.fetch[k].address;}
    }
    return found;
}
// The bound fetch of rule fetch j (at its located slot; at its own address when it cannot be located, which boundMismatch refuses).
inline BoundFetch boundFetchOf(const ShaderRule& rule,const std::vector<std::uint8_t>& code,unsigned j) noexcept {
    const unsigned slot=boundSlot(rule,code,j);
    return boundFetch(code,slot!=~0u?slot:rule.fetch[j].address);
}
// Why the rule does not describe this bound copy (nullptr: it does). Every vfetch of the rule must be found in the bound code
// (v621: by its destination register, see boundSlot), write the same components, read the same register component, not round
// its index and not be predicated; a mini fetch (it takes the previous full fetch's address on the hardware) must have the same
// index definition as the full fetch before it in the BOUND order (v621; v570-v620: the rule's previous fetch, the container order).
inline const char* boundMismatch(const ShaderRule& rule,const std::vector<std::uint8_t>& code,bool predicatedOk=false) noexcept {
    std::array<unsigned,12> slotOf{};
    for(unsigned j=0;j<rule.count;++j) {
        const auto& r=rule.fetch[j];
        slotOf[j]=boundSlot(rule,code,j);
        if(slotOf[j]==~0u)return boundFetch(code,r.address).vertexFetch?"bound-order":"bound-not-vfetch";
        const auto b=boundFetch(code,slotOf[j]);
        if(b.srcRegister!=r.srcRegister || b.srcComponent!=r.srcComponent)return "bound-source";
        if(b.writtenMask!=r.writtenMask)return "bound-mask";
        if(b.rounded)return "bound-rounded";
        if(b.predicated && !predicatedOk)return "bound-predicated";
    }
    for(unsigned j=0;j<rule.count;++j) {
        if(!boundFetch(code,slotOf[j]).mini)continue;
        unsigned prev=~0u; // the rule fetch at the nearest earlier slot whose bound fetch is a full fetch
        for(unsigned k=0;k<rule.count;++k)
            if(slotOf[k]<slotOf[j] && !boundFetch(code,slotOf[k]).mini && (prev==~0u || slotOf[k]>slotOf[prev]))prev=k;
        if(prev==~0u || !sameIndex(rule.fetch[j],rule.fetch[prev]))return "bound-mini";
    }
    return nullptr;
}
// Whether the constant bank (256 float4 as host words) holds the literals the rule's indices were derived with
// (information only: the translated module carries the container's literals itself, and so does the rule).
inline bool literalsHold(const ShaderRule& rule,const std::array<std::uint32_t,1024>& constants) noexcept {
    for(unsigned k=0;k<rule.literalCount;++k)
        if(constants[std::size_t{rule.literal[k].reg}*4+rule.literal[k].component]!=rule.literal[k].bits)return false;
    return true;
}
}
