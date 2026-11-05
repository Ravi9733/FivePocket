#pragma once
// Attempt v77: the CPU conversion of vertex attributes (v41 converted streams, v69 SCALED formats, and with
// diag-generic-uint-blend-indices the zyxw BLENDINDICES of peds) without the per-vertex generic decode.
// v71 converted every vertex with
//     swapped = swapCopy(stream bytes, take, n, endian)                        (once per converted attribute)
//     tmp[16] = swapped[v*stride+offset ...] (zero past n); value = decodeAttribute(tmp, f);
//     out[v] = value (float input) or uint32(max(0, value[k])) (uint input)
// convertAttributeFromSource writes the same 16 bytes per vertex:
//  - one loop per format and field mode (the component extraction fixed at compile time); normalized fields of
//    at most 16 bits in single precision (one correctly rounded division equals decodeAttribute's double division
//    rounded to float for every such value; checked exhaustively), 32-bit fields with decodeAttribute's double
//    arithmetic, integer fields exactly; the exponent adjust and the feed as decodeAttribute applies them;
//  - the vertices whose format bytes lie in whole dwords of the source are read from the source and swapped per
//    dword (no copy of the whole stream); the others (a partial final dword, the zero fill past `take`) from a
//    swapCopy of the stream's tail only, through decodeAttribute itself when their 16 bytes cross the end.
// (Host test: vertex_convert_test.cpp, against v71's loop: random data for every format/flag/feed/endian/stride/
// size combination, and every field value exhaustively.)
#include "generic_renderer_cpu.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace recomp::gpu::generic::cpu {

namespace convert_detail {
// decodeAttribute's `packed` for one field of `bits` (<= 32) bits, as a function of the fetch's mode.
enum class Mode : unsigned {SignedInt,UnsignedInt,Unorm,SnormClamp,SnormNoZero};
inline Mode modeOf(const VFetch& f) {
    if(f.isInteger)return f.isSigned?Mode::SignedInt:Mode::UnsignedInt;
    if(!f.isSigned)return Mode::Unorm;
    return f.noZero?Mode::SnormNoZero:Mode::SnormClamp;
}
template<Mode M,unsigned Bits> inline float field(std::uint64_t raw) {
    constexpr std::uint64_t mask=(std::uint64_t{1}<<Bits)-1;
    raw&=mask;
    if constexpr(Bits<=16 && M!=Mode::UnsignedInt && M!=Mode::SignedInt) {
        const float sext=(raw>>(Bits-1))&1?float(std::int32_t(raw)-std::int32_t(mask)-1):float(raw);
        if constexpr(M==Mode::Unorm)return float(raw)/float(mask);
        else if constexpr(M==Mode::SnormNoZero)return (2.0f*sext+1.0f)/float(mask);
        else return std::max(-1.0f,sext/float(mask>>1));
    } else if constexpr(M==Mode::UnsignedInt) {
        return float(double(raw)); // the same double -> float rounding as decodeAttribute (exact below 2^24)
    } else if constexpr(M==Mode::SignedInt) {
        const double sext=(raw>>(Bits-1))&1?double(raw)-double(mask)-1.0:double(raw);
        return float(sext);
    } else if constexpr(M==Mode::Unorm) {
        return float(double(raw)/double(mask));
    } else {
        const double sext=(raw>>(Bits-1))&1?double(raw)-double(mask)-1.0:double(raw);
        if constexpr(M==Mode::SnormNoZero)return float((2.0*sext+1.0)/double(mask));
        else return float(std::max(-1.0,sext/double(mask>>1)));
    }
}
inline std::uint32_t load32(const std::uint8_t* p){std::uint32_t x;std::memcpy(&x,p,4);return x;}
inline float halfToFloat(std::uint16_t h) { // decodeAttribute's half(): no infinities/NaNs, e=31 is a normal exponent
    const unsigned e=(h>>10)&31,m=h&1023;const float r=e?std::ldexp(1.0f+m/1024.0f,int(e)-15):std::ldexp(m/1024.0f,-14);
    return (h&0x8000)?-r:r;
}
inline std::uint32_t swapWord(std::uint32_t v,unsigned endian) { // swapCopy's per-dword swap
    switch(endian){case 1:return ((v>>8)&0x00FF00FFu)|((v<<8)&0xFF00FF00u);case 2:return __builtin_bswap32(v);case 3:return (v>>16)|(v<<16);default:return v;}
}
// Components fetched by decodeAttribute's exponent adjust (the defaults (0, 0, 0, 1) are not scaled).
inline unsigned adjustedComponents(unsigned format) {
    return format==36 || format==33?1:format==37 || format==31 || format==25 || format==34?2:format==57 || format==16 || format==17?3:4;
}
// Bytes decodeAttribute reads for the format (0: none, the format decodes to (0, 0, 0, 1)).
constexpr unsigned formatBytes(unsigned format) {
    switch(format){case 6:case 7:case 16:case 17:case 25:case 31:case 33:case 36:return 4;case 26:case 32:case 34:case 37:return 8;
        case 57:return 12;case 35:case 38:return 16;default:return 0;}
}
// Decode of one vertex's components (no exponent adjust, no feed) from its formatBytes readable bytes.
template<unsigned Format,Mode M> inline void decode(const std::uint8_t* p,float* v) {
    if constexpr(Format==6){const auto w=load32(p);v[0]=field<M,8>(w);v[1]=field<M,8>(w>>8);v[2]=field<M,8>(w>>16);v[3]=field<M,8>(w>>24);}
    else if constexpr(Format==7){const auto w=load32(p);v[0]=field<M,10>(w);v[1]=field<M,10>(w>>10);v[2]=field<M,10>(w>>20);v[3]=field<M,2>(w>>30);}
    else if constexpr(Format==16){const auto w=load32(p);v[0]=field<M,11>(w);v[1]=field<M,11>(w>>11);v[2]=field<M,10>(w>>22);}
    else if constexpr(Format==17){const auto w=load32(p);v[0]=field<M,10>(w);v[1]=field<M,11>(w>>10);v[2]=field<M,11>(w>>21);}
    else if constexpr(Format==25){const auto w=load32(p);v[0]=field<M,16>(w);v[1]=field<M,16>(w>>16);}
    else if constexpr(Format==26){const auto w=load32(p),h=load32(p+4);v[0]=field<M,16>(w);v[1]=field<M,16>(w>>16);v[2]=field<M,16>(h);v[3]=field<M,16>(h>>16);}
    else if constexpr(Format==33){v[0]=field<M,32>(load32(p));}
    else if constexpr(Format==34){v[0]=field<M,32>(load32(p));v[1]=field<M,32>(load32(p+4));}
    else if constexpr(Format==35){for(unsigned k=0;k<4;++k)v[k]=field<M,32>(load32(p+4*k));}
    else if constexpr(Format==31 || Format==32){for(unsigned k=0;k<(Format==31?2u:4u);++k){std::uint16_t h;std::memcpy(&h,p+2*k,2);v[k]=halfToFloat(h);}}
    else if constexpr(Format==36 || Format==37 || Format==38 || Format==57){std::memcpy(v,p,formatBytes(Format));}
}
struct Params {unsigned count,stride,offset;bool uintInput;float scale;unsigned scaled,feed;};
inline void store(const float* r,bool uintInput,std::uint8_t* out) {
    if(uintInput){std::uint32_t u[4];for(unsigned k=0;k<4;++k)u[k]=std::uint32_t(std::max(0.0f,r[k]));std::memcpy(out,u,16);}
    else std::memcpy(out,r,16);
}
inline void finish(float* v,const Params& q,std::uint8_t* out) {
    if(q.scaled)for(unsigned k=0;k<q.scaled;++k)v[k]*=q.scale;
    if(q.feed==0x688){store(v,q.uintInput,out);return;}
    float r[4];
    for(unsigned c=0;c<4;++c){const unsigned s=swizzleSelector(q.feed,c);r[c]=s<4?v[s]:s==4?0.0f:1.0f;}
    store(r,q.uintInput,out);
}
// The v71 per-vertex path for one vertex of swapped bytes [0, size) at byte offset `off`.
inline void slowVertex(const std::uint8_t* swapped,std::size_t size,std::size_t off,const VFetch& f,bool uintInput,std::uint8_t* out) {
    std::array<std::uint8_t,16> tmp{};
    if(off<size)std::memcpy(tmp.data(),swapped+off,std::min<std::size_t>(16,size-off));
    const auto value=decodeAttribute(tmp.data(),f);
    float r[4]{value[0],value[1],value[2],value[3]};store(r,uintInput,out);
}
template<unsigned Format,Mode M> struct Kernel {
    static constexpr unsigned bytes=formatBytes(Format);
    // Vertices [from, to) of already swapped bytes [0, size): offsets (v - from) * stride + offset.
    static void swapped(const std::uint8_t* s,std::size_t size,unsigned from,unsigned to,const VFetch& f,const Params& q,std::uint8_t* out) {
        for(unsigned v=from;v<to;++v,out+=16) {
            const std::size_t off=std::size_t{v-from}*q.stride+q.offset;
            if(!bytes || off+bytes>size){slowVertex(s,size,off,f,q.uintInput,out);continue;}
            float c[4]{0,0,0,1};decode<Format,M>(s+off,c);finish(c,q,out);
        }
    }
    // Vertices [0, to) read from the unswapped source, every one inside whole dwords (dword-aligned offsets).
    static void source(const std::uint8_t* src,unsigned endian,unsigned to,const Params& q,std::uint8_t* out) {
        for(unsigned v=0;v<to;++v,out+=16) {
            const std::uint8_t* p=src+std::size_t{v}*q.stride+q.offset;
            std::uint32_t w[4];for(unsigned k=0;k<bytes/4;++k)w[k]=swapWord(load32(p+4*k),endian);
            float c[4]{0,0,0,1};decode<Format,M>(reinterpret_cast<const std::uint8_t*>(w),c);finish(c,q,out);
        }
    }
};
// Dispatch of `Run<Format, Mode>::go(args...)` over the fetch's format and field mode.
template<template<unsigned,Mode> class Run,class... A> inline void dispatch(const VFetch& f,A&&... a) {
    const auto modes=[&](auto format) {
        constexpr unsigned F=decltype(format)::value;
        switch(modeOf(f)) {
        case Mode::SignedInt: Run<F,Mode::SignedInt>::go(a...);break;
        case Mode::UnsignedInt: Run<F,Mode::UnsignedInt>::go(a...);break;
        case Mode::Unorm: Run<F,Mode::Unorm>::go(a...);break;
        case Mode::SnormClamp: Run<F,Mode::SnormClamp>::go(a...);break;
        case Mode::SnormNoZero: Run<F,Mode::SnormNoZero>::go(a...);break;
        }
    };
    const auto one=[&](auto format){Run<decltype(format)::value,Mode::Unorm>::go(a...);}; // formats without field modes
    switch(f.format) {
    case 6: modes(std::integral_constant<unsigned,6>{});return;
    case 7: modes(std::integral_constant<unsigned,7>{});return;
    case 16: modes(std::integral_constant<unsigned,16>{});return;
    case 17: modes(std::integral_constant<unsigned,17>{});return;
    case 25: modes(std::integral_constant<unsigned,25>{});return;
    case 26: modes(std::integral_constant<unsigned,26>{});return;
    case 33: modes(std::integral_constant<unsigned,33>{});return;
    case 34: modes(std::integral_constant<unsigned,34>{});return;
    case 35: modes(std::integral_constant<unsigned,35>{});return;
    case 31: one(std::integral_constant<unsigned,31>{});return;
    case 32: one(std::integral_constant<unsigned,32>{});return;
    case 36: one(std::integral_constant<unsigned,36>{});return;
    case 37: one(std::integral_constant<unsigned,37>{});return;
    case 38: one(std::integral_constant<unsigned,38>{});return;
    case 57: one(std::integral_constant<unsigned,57>{});return;
    default: one(std::integral_constant<unsigned,0>{});return; // decodeAttribute's (0, 0, 0, 1): every vertex slow
    }
}
template<unsigned F,Mode M> struct SwappedRun {
    static void go(const std::uint8_t* s,std::size_t size,unsigned from,unsigned to,const VFetch& f,const Params& q,std::uint8_t* out){Kernel<F,M>::swapped(s,size,from,to,f,q,out);}
};
template<unsigned F,Mode M> struct SourceRun {
    static void go(const std::uint8_t* src,unsigned endian,unsigned to,const Params& q,std::uint8_t* out){Kernel<F,M>::source(src,endian,to,q,out);}
};
inline Params paramsOf(const VFetch& f,unsigned count,unsigned stride,unsigned offset,bool uintInput) {
    return {count,stride,offset,uintInput,f.expAdjust?std::ldexp(1.0f,f.expAdjust):1.0f,f.expAdjust?adjustedComponents(f.format):0u,f.feed};
}
} // namespace convert_detail

// `swapped`/`size`: the stream's bytes after swapCopy (v71's convertSwapped); out: count*16 bytes.
inline void convertAttributeStream(const std::uint8_t* swapped,std::size_t size,unsigned count,unsigned stride,unsigned offset,
                                   const VFetch& f,bool uintInput,std::uint8_t* out) {
    using namespace convert_detail;
    const auto q=paramsOf(f,count,stride,offset,uintInput);
    dispatch<SwappedRun>(f,swapped,size,0u,count,f,q,out);
}
// The same from the stream's unswapped bytes: `take` source bytes of the `n` the stream spans (zero past take),
// swapped per `endian` as swapCopy does. `scratch` holds the swapped tail (or the whole stream for offsets that
// are not dword multiples).
inline void convertAttributeFromSource(const std::uint8_t* src,std::size_t take,std::size_t n,unsigned endian,unsigned count,unsigned stride,unsigned offset,
                                       const VFetch& f,bool uintInput,std::uint8_t* out,std::vector<std::uint8_t>& scratch) {
    using namespace convert_detail;
    take=std::min(take,n);
    const unsigned bytes=formatBytes(f.format);
    if((stride&3) || (offset&3) || !bytes) {
        scratch.resize(n);swapCopy(scratch.data(),src,take,n,endian);
        convertAttributeStream(scratch.data(),n,count,stride,offset,f,uintInput,out);return;
    }
    const std::size_t whole=take&~std::size_t{3};
    // Vertices [0, direct): their format bytes lie in whole source dwords.
    unsigned direct=0;
    if(std::size_t{offset}+bytes<=whole)direct=stride?unsigned(std::min<std::size_t>(count,(whole-offset-bytes)/stride+1)):count;
    const auto q=paramsOf(f,count,stride,offset,uintInput);
    dispatch<SourceRun>(f,src,endian,direct,q,out);
    if(direct==count)return;
    // The rest: the swapped bytes from the first such vertex on (w0 is a dword multiple, so swapCopy of the
    // window swaps, pads and zero-fills exactly as the whole stream's swapCopy does there).
    const std::size_t w0=std::size_t{direct}*stride+offset;
    const std::size_t size=w0<n?n-w0:0;
    scratch.resize(size);
    if(size)swapCopy(scratch.data(),src+w0,take>w0?take-w0:0,size,endian);
    auto tail=q;tail.offset=0;
    dispatch<SwappedRun>(f,scratch.data(),size,direct,count,f,tail,out+std::size_t{direct}*16);
}

} // namespace recomp::gpu::generic::cpu
