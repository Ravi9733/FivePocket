#pragma once
// v231: rect-list attributes without decodeAttribute's per-call format switch. The v227 outdoor profile put decodeAttribute
// at 7.7% of the renderer thread, all of it from the rect-list corner reads (generic_renderer.cpp draw(), `read`); v210 had
// already taken the 32-bit floats with the identity feed. A plan per attribute, made once per draw, picks:
//  - 32-bit float formats (36/37/57/38) without an exponent adjust and with a feed: their raw words over the (0, 0, 0, 1)
//    defaults, then the feed (decodeAttribute memcpys the same words and applies the same applyFeed);
//  - the fastDecode kinds (unsigned-normalized 8_8_8_8, signed-integer 16_16 and 16_16_16_16, no exponent adjust):
//    fastDecode of the attribute's first dwords as the swapped stream holds them (decodeAttribute reads the same dwords
//    with the same field arithmetic and feed; convertAttribute relies on this equality since v78).
// Everything else (and the identity-feed floats, which v210's branch takes first) is not planned: decodeAttribute as
// before. Exactness: tests/rect_decode_test.cpp compares rectDecode with decodeAttribute bit for bit.
#include "generic_renderer_cpu.h"
#include <array>
#include <cstdint>
#include <cstring>

namespace recomp::gpu::generic::cpu {
struct RectDecodePlan {unsigned floats=0;FastDecode kind=FastDecode::None;bool active()const{return floats || kind!=FastDecode::None;}};
inline unsigned floatComponents(unsigned format){return format==36?1:format==37?2:format==57?3:format==38?4:0;}
inline RectDecodePlan rectDecodePlan(const VFetch& f) {
    RectDecodePlan p;
    if(f.expAdjust)return p;
    if(const unsigned n=floatComponents(f.format)){if(f.feed!=0x688)p.floats=n;return p;} // (identity feed: v210's branch)
    p.kind=fastDecodeKind(f);
    return p;
}
// `at`: the attribute's bytes in the swapped stream, at least 16 readable (the caller's off+16 <= size test).
inline std::array<float,4> rectDecode(const RectDecodePlan& p,const std::uint8_t* at,unsigned feed) {
    if(p.floats) {
        std::array<float,4> v{0,0,0,1};std::memcpy(v.data(),at,4*p.floats);
#if defined(RECOMP_RECT_DECODE_MUTANT_NO_FEED)
        return v; // test mutant: the feed is dropped
#else
        return applyFeed(v,feed);
#endif
    }
    std::uint32_t w0,w1=0;std::memcpy(&w0,at,4);
#if !defined(RECOMP_RECT_DECODE_MUTANT_NO_W1) // test mutant: the second dword of 16_16_16_16 is not read
    if(p.kind==FastDecode::Sint16x4)std::memcpy(&w1,at+4,4);
#endif
    return fastDecode(p.kind,w0,w1,feed);
}
}
