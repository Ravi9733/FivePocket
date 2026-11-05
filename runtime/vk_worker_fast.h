#pragma once
// vk-worker (package P, 2026-10-03): diag-vkw-convert, an exact fast path for the Vulkan worker's CPU-converted vertex
// attributes (generic_renderer.cpp draw(): the attributes vertexNeedsConversion() sends to the CPU).
//
// cpu::convertAttribute (v78) decodes three kinds without the generic decoder (unorm 8_8_8_8, sint 16_16, sint 16_16_16_16);
// every other kind swaps the whole stream into a scratch copy and calls cpu::decodeAttribute once per vertex (per-field double
// divisions behind an initializer_list and a format switch). While driving that slow path decodes 25-99 K vertices per frame,
// mostly signed-normalized 11_11_10 (format 16; RENDER_GENERIC_VERTEX_ADAPT cpu_vertices / convert_ms, v407 milestone drive),
// 6.5% of the worker (~1.3 ms per frame) in the v407 drive profile.
//
// convertPacked() writes the same 16 bytes per vertex for formats 6 / 7 / 16 / 17 (8_8_8_8, 2_10_10_10, 11_11_10, 10_11_11) in
// every field mode:
//  - the component values come from per-field tables (one per field width and fetch mode) whose entry for each raw value is
//    cpu::decodeAttribute's own result for a word holding that value in that field (the reference function computes the
//    table, so every entry is its bit pattern: same divisions, same rounding, same clamp);
//  - the exponent adjust (2^expAdjust built as decodeAttribute builds it, on the format's fetched components only), the feed
//    (cpu::applyFeed) and the store (cpu::storeConverted: uint inputs max(0, x) truncated) are decodeAttribute's / v78's own;
//  - the 32-bit word of a vertex is read as convertAttribute's fast kinds read theirs: vertices whose word lies inside the
//    whole source dwords from the source with the endian swap, the others (the source's end, zero extension past take or n)
//    through cpu::swappedWord, which returns the 4 bytes at that offset of what swapCopy writes.
// Only dword-aligned strides and offsets are taken (as the fast kinds); anything else returns false and v78's path runs.
// Host test: tools/vk_worker/tests/vkw_convert_test.cpp (randomized differential against cpu::convertAttribute over every format,
// mode, exponent, feed, endian, stride, offset and stream end; every raw field value exhaustively; mutants).
#include "generic_renderer_cpu.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>

namespace recomp::gpu::generic::vkw {

// The field tables of one fetch mode (isInteger, isSigned, noZero): widths 2, 8, 10 and 11.
struct PackedTables {
    std::array<float,4> t2{};std::array<float,256> t8{};std::array<float,1024> t10{};std::array<float,2048> t11{};
};
inline unsigned modeIndex(const cpu::VFetch& f){return (f.isInteger?1u:0u)|(f.isSigned?2u:0u)|(f.noZero?4u:0u);}
// Built from cpu::decodeAttribute itself (no exponent, identity feed): a word with the raw value in a field of that width.
inline std::unique_ptr<PackedTables> buildPackedTables(const cpu::VFetch& mode) {
    auto t=std::make_unique<PackedTables>();
    cpu::VFetch f;f.isInteger=mode.isInteger;f.isSigned=mode.isSigned;f.noZero=mode.noZero;f.expAdjust=0;f.feed=0x688;
    const auto decode=[&](unsigned format,std::uint32_t word){std::uint8_t b[16]{};std::memcpy(b,&word,4);f.format=format;return cpu::decodeAttribute(b,f);};
    for(std::uint32_t r=0;r<2048;++r)t->t11[r]=decode(16,r)[0];         // 11_11_10: x = bits 0-10
    for(std::uint32_t r=0;r<1024;++r)t->t10[r]=decode(16,r<<22)[2];     //           z = bits 22-31 (10 bits)
    for(std::uint32_t r=0;r<256;++r)t->t8[r]=decode(6,r)[0];            // 8_8_8_8:  x = bits 0-7
    for(std::uint32_t r=0;r<4;++r)t->t2[r]=decode(7,r<<30)[3];          // 2_10_10_10: w = bits 30-31
    return t;
}
// Tables per mode, made on first use (the renderer thread only).
inline const PackedTables& packedTables(const cpu::VFetch& f) {
    static std::array<std::unique_ptr<PackedTables>,8> tables;
    auto& p=tables[modeIndex(f)];
    if(!p)p=buildPackedTables(f);
    return *p;
}
inline bool packedFormat(unsigned format){return format==6 || format==7 || format==16 || format==17;}
// Whether convertPacked() takes this attribute (else cpu::convertAttribute, unchanged, does).
inline bool convertPackedHandles(const cpu::VFetch& f,unsigned stride,unsigned offset) {
    return packedFormat(f.format) && cpu::fastDecodeKind(f)==cpu::FastDecode::None && !(stride&3) && !(offset&3);
}
// decodeAttribute's value of one swapped word (formats 6 / 7 / 16 / 17), before the exponent adjust and the feed.
inline std::array<float,4> decodePacked(const PackedTables& t,unsigned format,std::uint32_t w) {
    std::array<float,4> v{0,0,0,1};
    switch(format) {
#if defined(RECOMP_VKW_MUTANT_CONVERT_ORDER) // test mutant: 11_11_10 read as 10_11_11 (field widths in the other order)
    case 16: v[0]=t.t10[w&1023];v[1]=t.t11[(w>>10)&2047];v[2]=t.t11[(w>>21)&2047];break;
#else
    case 16: v[0]=t.t11[w&2047];v[1]=t.t11[(w>>11)&2047];v[2]=t.t10[(w>>22)&1023];break;
#endif
    case 17: v[0]=t.t10[w&1023];v[1]=t.t11[(w>>10)&2047];v[2]=t.t11[(w>>21)&2047];break;
    case 7: v[0]=t.t10[w&1023];v[1]=t.t10[(w>>10)&1023];v[2]=t.t10[(w>>20)&1023];v[3]=t.t2[w>>30];break;
    default: v[0]=t.t8[w&255];v[1]=t.t8[(w>>8)&255];v[2]=t.t8[(w>>16)&255];v[3]=t.t8[w>>24];break; // 6
    }
    return v;
}
// convertAttribute's output (out[16*count]) for an attribute convertPackedHandles() accepts: `count` vertices of a stream
// whose source bytes are src[0, take) of the n it spans, swapped per `endian`, at v*stride + offset.
inline void convertPacked(std::uint8_t* __restrict out,unsigned count,const std::uint8_t* src,std::size_t take,std::size_t n,unsigned endian,
                          unsigned stride,unsigned offset,const cpu::VFetch& f,bool uintInput) {
    const auto& t=packedTables(f);
    const unsigned format=f.format;
    // decodeAttribute's exponent adjust: 2^expAdjust on the fetched components (3 for 16 / 17, 4 for 6 / 7)
    const unsigned scaled=f.expAdjust?(format==16 || format==17?3u:4u):0u;
    float scale=1.0f;
    if(scaled){const std::uint32_t bits=std::uint32_t(127+f.expAdjust)<<23;std::memcpy(&scale,&bits,4);}
#if defined(RECOMP_VKW_MUTANT_CONVERT_NO_EXP) // test mutant: the exponent adjust dropped
    scale=1.0f;
#endif
    const auto one=[&](std::uint32_t w,std::uint8_t* o) {
        auto v=decodePacked(t,format,w);
        for(unsigned k=0;k<scaled;++k)v[k]*=scale;
        cpu::storeConverted(o,f.feed==0x688?v:cpu::applyFeed(v,f.feed),uintInput);
    };
    // Vertices [0, direct): their word lies inside the whole swapped dwords of the source (as convertAttribute's fast kinds).
    const std::size_t whole=std::min(take,n)&~std::size_t{3};
    unsigned direct=0;
#if defined(RECOMP_VKW_MUTANT_CONVERT_DIRECT_PAST) // test mutant: a word straddling the end of the whole dwords read directly too
    if(std::min(take,n)>=std::size_t{offset}+1)direct=unsigned(std::min<std::size_t>(count,(std::min(take,n)-offset-1)/std::max(1u,stride)+1));
#else
    if(whole>=std::size_t{offset}+4)direct=unsigned(std::min<std::size_t>(count,(whole-offset-4)/std::max(1u,stride)+1));
#endif
    const auto run=[&](auto swap) {
        for(unsigned v=0;v<direct;++v) {
            std::uint32_t w;std::memcpy(&w,src+std::size_t{v}*stride+offset,4);
            one(swap(w),out+16*std::size_t{v});
        }
    };
    switch(endian) {
    case 1: run([](std::uint32_t v){return ((v>>8)&0x00FF00FFu)|((v<<8)&0xFF00FF00u);});break;
    case 2: run([](std::uint32_t v){return __builtin_bswap32(v);});break;
    case 3: run([](std::uint32_t v){return (v>>16)|(v<<16);});break;
    default: run([](std::uint32_t v){return v;});break;
    }
    for(unsigned v=direct;v<count;++v)one(cpu::swappedWord(src,take,n,endian,std::size_t{v}*stride+offset),out+16*std::size_t{v});
}

} // namespace recomp::gpu::generic::vkw
