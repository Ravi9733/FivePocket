#pragma once
// Attempt v79 (post-processing fidelity), pure helpers of the generic renderer; host-tested by
// local-analysis/attempts/v79/tests/post_fidelity_test.cpp.
//
// 1. Display gamma ramp. D3DDevice_SetGammaRamp (836AFAF8) converts the caller's D3DGAMMARAMP for the
//    display's gamma (836B8548 with the 836B83C0 table, VdGetCurrentDisplayGamma type 1 here) and keeps
//    the result at device+15408: three 256-entry arrays of big-endian u16 (red +0, green +512, blue
//    +1024). The next swap (822D41E8 -> 836B5480 -> 836B49F0 -> 836B8668) writes it to the scan-out LUT
//    as DC_LUT_30_COLOR words (R bits 20-29, G 10-19, B 0-9 = the u16 >> 6), and the 360 maps each 8-bit
//    front-buffer channel through that LUT. The generic renderer replaces the swap, so v79 takes the ramp
//    at the SetGammaRamp return and applies it at present. D3D's own default (836B8888 with r4=0,
//    (i*1023/255)<<6) is the identity; GTA's one caller (82989588) sets a static table at 837CB088.
// 2. (v79's resolve writeback: not in v100, which uses v75's generic_resolve_writeback.h.)
#include "generic_texture_decode.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace recomp::gpu::generic::post {

// ---- 1. display gamma ramp

struct DisplayRamp {
    std::array<std::array<std::uint8_t,256>,3> lut{}; // per channel: 8-bit front-buffer value -> 8-bit output
    bool identity=true;
};
// 10-bit LUT sample -> 8 bits, round to nearest (the rule NativeDisplayGammaRamp::pixel uses).
inline std::uint8_t lutTo8(unsigned v10) {return static_cast<std::uint8_t>((std::min(v10,1023u)*255u+511u)/1023u);}
inline DisplayRamp displayRamp(const std::array<std::uint32_t,256>& packed) {
    DisplayRamp r;
    for(unsigned i=0;i<256;++i)for(unsigned c=0;c<3;++c) {
        r.lut[c][i]=lutTo8((packed[i]>>(20-10*c))&1023u);
        r.identity=r.identity && r.lut[c][i]==i;
    }
    return r;
}
// device+15408 (1536 bytes, big-endian u16 red/green/blue arrays) packed as 836B8668 writes it.
inline std::array<std::uint32_t,256> packD3DRamp(const std::uint8_t* bytes) {
    std::array<std::uint32_t,256> packed{};
    const auto u16=[&](unsigned at){return unsigned(bytes[at])<<8|bytes[at+1];};
    for(unsigned i=0;i<256;++i)
        packed[i]=((u16(2*i)>>6)<<20)|((u16(512+2*i)>>6)<<10)|(u16(1024+2*i)>>6);
    return packed;
}
// v24's opaqueRgba (a copy with every alpha 255) with R, G and B mapped through the ramp.
inline std::vector<std::uint8_t> opaqueRgbaRamped(const std::uint8_t* src,std::size_t bytes,const DisplayRamp& ramp) {
    std::vector<std::uint8_t> rgba(src,src+bytes);
    std::uint8_t* p=rgba.data();const std::size_t whole=bytes&~std::size_t{3};
    const auto& r=ramp.lut[0];const auto& g=ramp.lut[1];const auto& b=ramp.lut[2];
    for(std::size_t i=0;i<whole;i+=4){p[i]=r[p[i]];p[i+1]=g[p[i+1]];p[i+2]=b[p[i+2]];p[i+3]=255;}
    return rgba;
}

// ---- 2. (v100: v79's resolve writeback helpers are not used: the writeback is v75's generic_resolve_writeback.h.)

// ---- 3. postfx constants worth logging (register numbers of the translated shaders' cbuffers)

struct PostfxShader {std::uint64_t hash;const char* name;std::array<std::uint8_t,16> registers;unsigned count;};
inline constexpr PostfxShader PostfxShaders[]={
    // 08dfab6c: the tonemap (HDRSampler 7e3 bits, BloomParams, Filmic0/1, ColorCorrectHighLum,
    // ColorShiftLowLum, Desaturate, Gamma, Vignetting, dofProj/dofDist, lightrayParams, sslrParams,
    // globalScalars3 = c18).
    {0x08dfab6c16ec854cull,"tonemap",{18,130,132,135,136,137,164,175,176,183,184,185,186,191},14},
    {0x0065a421a6716d61ull,"adaptation",{193,194,195,196},4},       // ExposureParams0-3
    {0xd63dad630d8cd420ull,"bright-pass",{18,135,136},3},           // globalScalars3, BloomParams, Filmic0
    {0x2bf099324b87521dull,"hdr-downsample",{18},1},
    {0xb880c5f0ea1a8f1eull,"ambient",{18,37,38,39,40,41,42,43},8},  // natural/artificial ambient colours
    {0xa63b24cf5f70e11cull,"light",{18},1},
};
// fixer2 (2026-10-22): the tonemap is not one pixel shader but a family of 48 (postfx#20p .. #67p: the same final pass with
// motion blur, high-quality depth of field, player mask, ... compiled in). Every one of them declares dofProj at c130 and
// Filmic0 at c136 (the pack's translations; attempts/v577/tests/tonemap_family_test.cpp derives the family from them and
// checks this table against it); the camera the frame generator takes (fg_gen.h takeCamera: c8-c15, c130) is the frame's
// in every one of them. postfxShader() answers the tonemap entry for each, so a frame drawn with a variant is counted and
// logged as the tonemap frame it is. Until v576 only postfx#20p was: every frame drawn with another variant (motion blur
// while the camera moves fast, deaths, fades) was "no-camera" for the frame generator, and those refusals tripped its
// slot keeper (why=inputs: no made pictures for 20 s). diag-fg-tonemap-one (read at init) restores v576's one shader.
struct TonemapVariant {std::uint64_t hash;unsigned postfx;};
inline constexpr TonemapVariant TonemapVariants[]={
    {0x05e4f597fe786d1full,23},
    {0x08d6cf0775378078ull,42},
    {0x08dfab6c16ec854cull,20},
    {0x0b86735657a541eaull,50},
    {0x0ce70535f29e499bull,56},
    {0x0df2e8f53ec851b7ull,37},
    {0x0ef6dd02d28cfebcull,51},
    {0x0f9da5d36c24a5cfull,61},
    {0x1235b57963b2ea95ull,27},
    {0x12e9d0938c91ebc1ull,28},
    {0x13f47ccbb12d8234ull,44},
    {0x1d307c953200483cull,64},
    {0x22be2ceb2af119f9ull,55},
    {0x247584040b1007f1ull,33},
    {0x38f9a61a70b5a6dcull,21},
    {0x3a7bd75ec69e7ed5ull,32},
    {0x3b68d42a5b0ac8e8ull,54},
    {0x3facbfe07b3d2d70ull,34},
    {0x42006c2d62dd4a95ull,47},
    {0x4cd424c0291726e9ull,36},
    {0x4da5ef58a22749bdull,31},
    {0x4f42515de5ee66b2ull,24},
    {0x5bec06a9cee94e90ull,63},
    {0x66a2d8d986e97cdbull,67},
    {0x70b7fda8cde311e2ull,40},
    {0x795edf1e85045e76ull,48},
    {0x83a0eb95c234ec2full,49},
    {0x8aef3d235fba6816ull,22},
    {0x8ea67d5d618118a2ull,59},
    {0x917957706941a9d6ull,41},
    {0x9239fc0809382d27ull,66},
    {0x9f4319ee67bc2d99ull,38},
    {0xa172b6eec6497bc0ull,26},
    {0xa6c41b2a91429dbfull,58},
    {0xa8e70f84ac848700ull,45},
    {0xab0a620af91d98a2ull,30},
    {0xbf6a9d311e18a7b0ull,52},
    {0xc5a78ce3de40f55aull,39},
    {0xd0880e923b7c55abull,29},
    {0xde23db3e95bbf041ull,25},
    {0xde267a29f623ec65ull,46},
    {0xe5c6bcd27fa7c3e2ull,60},
    {0xf0aec08b0a4598b0ull,57},
    {0xf575509ca9c2646full,65},
    {0xf6c718c65a467f3bull,62},
    {0xf72b684654687bbeull,53},
    {0xfdaeff1bfe6f51d4ull,35},
    {0xfe25081e340b57d7ull,43}
};
static_assert(std::size(TonemapVariants)==48,"the tonemap family");
inline std::atomic<bool> tonemapOneOnly{false}; // diag-fg-tonemap-one: v576's single tonemap shader
// The family member of `hash` (its postfx number; 0: not a tonemap). The table is sorted by hash.
inline unsigned tonemapVariant(std::uint64_t hash) {
    std::size_t lo=0,hi=std::size(TonemapVariants);
    while(lo<hi){const std::size_t m=(lo+hi)/2;if(TonemapVariants[m].hash<hash)lo=m+1;else hi=m;}
    return lo<std::size(TonemapVariants) && TonemapVariants[lo].hash==hash?TonemapVariants[lo].postfx:0u;
}
inline const PostfxShader* postfxShader(std::uint64_t hash) {
    for(const auto& s:PostfxShaders)if(s.hash==hash)return &s;
    if(!tonemapOneOnly.load(std::memory_order_relaxed) && tonemapVariant(hash))return &PostfxShaders[0]; // fixer2: a tonemap variant
    return nullptr;
}
// " c18=x,y,z,w c130=..." for the registers of `s` (or c18 alone) from a draw's pixel constant words.
inline std::string postfxConstants(const PostfxShader* s,const std::array<std::uint32_t,1024>& words) {
    std::string out;
    const auto one=[&](unsigned reg) {
        float v[4];for(unsigned c=0;c<4;++c)std::memcpy(&v[c],&words[reg*4+c],4);
        char b[96];std::snprintf(b,sizeof b," c%u=%.6g,%.6g,%.6g,%.6g",reg,v[0],v[1],v[2],v[3]);out+=b;
    };
    if(!s){one(18);return out;}
    for(unsigned i=0;i<s->count;++i)one(s->registers[i]);
    return out;
}
}
