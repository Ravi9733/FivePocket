#pragma once
// Attempt v86 (HUD/UI): change detection for textures the CPU rewrites in place. Pure CPU, no Vulkan,
// no guest memory access (callers pass byte views they validated).
//
// Evidence (device logs, v70 run u7wwt73g; v72 pause menu local-analysis/rendering-audit/v72-play.png):
// GTA V's Scaleform glyph cache is one 1024x1024 k_8 LINEAR texture (object 10271080,
// RENDER_RESOURCE_LAYOUT size=1024x1024x1 pitch=1024 format=2 tiled=0) that the CPU locks and
// rewrites whenever new glyphs are rasterised (RENDER_API_TEXTURE_LOCK_WAIT object=10271080 at
// 07:15:45, 07:15:58, 07:15:59, 07:19:54; unlock sub_836B0840 called from 8323D30C). The generic
// renderer's change detection (v32-v71) hashes 32 words spread over the texture: 128 of its 1 M
// texels. A new glyph lands on one of them almost never, so the cached image keeps the glyphs present
// at its first upload: the common letters of earlier text ("an t to", "o se ... e e" in the pause
// menu; "o to the guard." without its G) and nothing that was rasterised later.
//
// v86 adds two detectors (either alone suffices for the glyph cache; both are on by default):
//  - unlock invalidation: the texture unlock hook (sub_836B0840) reports the unlocked texture's
//    memory; cached textures overlapping it are re-read at their next use (and the per-frame binding
//    cache is re-verified). diag-ui-no-unlock-invalidate turns it off.
//  - full-content check for textures a Scaleform pixel shader samples (these shaders, below): a hash
//    of every byte, once per frame per binding, instead of the 32-word sample. diag-ui-sampled-
//    fingerprint turns it off.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace recomp::gpu::generic::ui {

// GTA V's scaleform_shaders.fxc pixel shaders (fxl_final_final), keyed by the generic pack's canonical
// microcode hash (tools/iter/generic_shader_pack.py). Names from the containers' debug-info paths
// (local-analysis/shader-translation-v41full-pack-01/shader-0465..0473). Sorted by hash.
struct ScaleformShader {std::uint64_t hash;const char* name;};
inline constexpr std::array<ScaleformShader,9> ScaleformPixelShaders{{
    {0x0c7341b616776c5aull,"PS_GFill_ColorPremult"},
    {0x545eb7f306ebe941ull,"PS_TexturedSprite_Alpha"},   // glyphs: samples .w of the glyph cache
    {0x5f018781e56106e9ull,"PS_GFill_1TextureColor"},
    {0x678a95fcfa95be07ull,"PS_TexturedSprite_Color"},
    {0x761a0b04380cb88full,"PS_BitmapFill"},
    {0x79c5b389f9d0e42bull,"PS_SolidFill"},
    {0x91683f8ba9c9d426ull,"PS_GFill_1Texture"},
    {0xe45a214d177f2350ull,"PS_GFill_Color"},
    {0xe7afab6ce6258f14ull,"PS_GFill_2Texture"},
}};
inline const char* scaleformPixelShader(std::uint64_t hash) {
    for(const auto& s:ScaleformPixelShaders)if(s.hash==hash)return s.name;
    return nullptr;
}

// A hash of every byte (the full-content check): four independent 64-bit lanes over little-endian
// 8-byte words (a word goes to exactly one lane), the tail bytes and the length folded in. Not
// cryptographic. Every step is a bijection of the lane state for a fixed input word (xor, multiply by
// an odd constant, xorshift), and the lanes are combined by a chain of bijections, so a change
// confined to one word (in particular any single changed byte) always changes the result; the host
// test checks that exhaustively for small spans and at random positions of a 1 MB span.
inline std::uint64_t contentHash(const std::uint8_t* p,std::size_t n) {
    constexpr std::uint64_t K0=0x9E3779B97F4A7C15ull,K1=0xC2B2AE3D27D4EB4Full,K2=0x165667B19E3779F9ull,K3=0xD6E8FEB86659FD93ull;
    std::uint64_t a=K0^n,b=K1,c=K2,d=K3;
    const auto step=[](std::uint64_t h,std::uint64_t w,std::uint64_t k){h^=w;h*=k;h^=h>>29;return h;};
    const auto fin=[](std::uint64_t x){x^=x>>30;x*=0xBF58476D1CE4E5B9ull;x^=x>>27;x*=0x94D049BB133111EBull;x^=x>>31;return x;};
    std::size_t i=0;
    for(;i+32<=n;i+=32) {
        std::uint64_t w[4];std::memcpy(w,p+i,32);
        a=step(a,w[0],K1);b=step(b,w[1],K2);c=step(c,w[2],K3);d=step(d,w[3],K0);
    }
    for(;i+8<=n;i+=8){std::uint64_t w;std::memcpy(&w,p+i,8);a=step(a,w,K1);}
    if(i<n){std::uint64_t w=0;std::memcpy(&w,p+i,n-i);b=step(b,w|(std::uint64_t{1}<<63),K2);}
    // h = fin(fin(fin(fin(a) ^ fin(b)) ^ fin(c)) ^ fin(d)): with the other lanes fixed, each lane maps
    // to the result through bijections only.
    std::uint64_t h=fin(a);
    for(const auto x:{b,c,d})h=fin(h^fin(x));
    return h;
}

// Half-open byte ranges [a, a+an) and [b, b+bn) share a byte (64-bit arithmetic: no wrap).
inline bool overlaps(std::uint64_t a,std::uint64_t an,std::uint64_t b,std::uint64_t bn) {
    return an && bn && a<b+bn && b<a+an;
}

// v32-v71 change detection as the renderer did it (for the host test's demonstration of the miss):
// the byte offsets of the 32 sampled words of a span of `bytes`.
inline std::array<std::uint64_t,32> sampledWordOffsets(std::uint64_t bytes) {
    std::array<std::uint64_t,32> o{};
    for(unsigned k=0;k<32;++k)o[k]=bytes>=4?static_cast<std::uint32_t>((bytes-4)*k/31)&~3u:0;
    return o;
}
}
