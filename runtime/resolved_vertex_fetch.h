// Attempt v73: vertex fetches of resolved memory with a computed fetch index. DIAGNOSTIC ONLY.
// GTA V's deferred ambient pass is a pair of DrawVertices rect lists over the 80x45 grid of 16x16 screen tiles
// (vertex shaders 93adf12996c6530f and c145e5abce4f7013, census i=736/737). Each rect's TEXCOORD0 comes from the
// 80x45 k_16_16_16_16_FLOAT tile classification the GPU resolved just before (904:7:80 -> 1f051000, census i=727),
// read as vertex stream slot 94 at the index r1.x = POSITION.w * 64 + POSITION.z: the original microcode's vfetch
// has src_reg 1 (not r0, the vertex index) and the CPU-built POSITION stream holds, in (z, w), the Xenos tiled texel
// index of the rect's tile (checked against 11 census rects: resolved_vertex_fetch_test). The first pass draws a
// tile when TEXCOORD0.z > 0, the second when TEXCOORD0.z == 0.
// Up to v72 the generic renderer fed TEXCOORD0 by vertex index from guest memory, which no resolve writes (resolves
// stay on the GPU): rects of screen tile rows 0..25 read whatever those bytes hold, later rows the zero extension.
// Tiles whose bytes happen to be non-zero got the first pass instead of the second: the tile-row bands in the upper
// part of the screen (v70-play-1.png; census: probe p6 (640,150), inside a band, is written by 736 and not 737).
// Pure functions; the renderer gathers the values on the GPU (generic_vertex_gather.hlsl).
#pragma once
#include "generic_texture_decode.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace recomp::gpu::generic::rvf {

// A vertex element whose fetch index is POSITION.w * scaleW + POSITION.z (the ALU instruction before the vfetch),
// with the vfetch reading register `indexRegister`.x (the runtime checks the bound microcode agrees).
// v140: indexComponent is the component of indexRegister the vfetch reads (0 = x, every rule before v140).
struct ComputedIndexRule {std::uint64_t vs;unsigned location,positionLocation,indexRegister;float scaleW;unsigned indexComponent=0;};
inline constexpr ComputedIndexRule ComputedIndexRules[]{
    {0x93adf12996c6530full,13,0,1,64.0f}, // ambient grid pass 1 (TEXCOORD0.z > 0)
    {0xc145e5abce4f7013ull,13,0,1,64.0f}, // ambient grid pass 2 (TEXCOORD0.z == 0)
    // v109: the other grid vertex shaders with the same index (tools/grid_rules.py: r1.x = POSITION.w * c + POSITION.z,
    // c a literal of the shader, r0 only the POSITION fetch before it). Outdoors (Ludendorff) none of the two above ran
    // (V73_GATHER gathered 0) and the sky/tree tiles showed the black squares v106 fixed indoors.
    {0x0853db6b758b39afull,13,0,1,64.0f}, // shader-translation-v41full-pack-02/shader-0056: r0.w * c251.y + r0.z
    {0x29ac989d40914575ull,13,0,1,64.0f}, // shader-translation-v41full-pack-02/shader-0055: r0.w * c251.z + r0.z
    {0x86de4d46ec3d9c87ull,13,0,1,64.0f}, // shader-translation-v41full-batch-03/shader-0693: r0.w * c251.y + r0.z
    {0x90453c238d298696ull,13,0,1,64.0f}, // shader-translation-v41full-pack-02/shader-0061: r0.w * c251.y + r0.z
    {0x912ae6bbd1314654ull,13,0,1,64.0f}, // shader-translation-v41full-pack-02/shader-0063: r0.w * c251.z + r0.z
    {0x959f5442ea0173b0ull,13,0,1,64.0f}, // shader-translation-v41full-pack-02/shader-0064: r0.w * c251.y + r0.z
    {0x966bfdb53d2fb95dull,13,0,1,64.0f}, // shader-translation-v41full-pack-02/shader-0058: r0.w * c251.y + r0.z
    {0xdcaade0d59c846c4ull,13,0,1,64.0f}, // shader-translation-v41full-pack-02/shader-0060: r0.w * c251.z + r0.z
    {0xf925a8476d7e600aull,13,0,1,64.0f}, // shader-translation-v41full-pack-01/shader-0115: r0.w * c250.y + r0.z
    {0xff40ffd0a38007caull,13,0,1,64.0f}, // shader-translation-v41full-pack-01/shader-0111: r0.w * c250.y + r0.z
    // v135: the grid shaders v109's text pattern missed (tools/grid_rules2.py, checked against the microcode: vfetch
    // registers and dst swizzles, the MULADD, the literal 64). Their POSITION fetch is swizzled, so the registers differ
    // but the index is still POSITION.w * 64 + POSITION.z in the attribute's components (e.g. r0 = POSITION.xywz:
    // r0.z * 64 + r0.w). cf683c28/eb0e904f/10a689db fetch POSITION into r1 and overwrite r0.x with the index before the
    // TEXCOORD0 vfetch reads it: indexRegister 0 there is the computed r0.x, not the vertex index. The device noted
    // vertex-stream-from-resolve slot=94 res=1f051000 for 415dd91f, cf683c28 and eb0e904f (no rule up to v134).
    {0x415dd91f4cb0e3e1ull,13,0,1,64.0f}, // shader-translation-v41full-pack-02/shader-0057 (lightTile_ambient): r0=P.xywz, r0.z * c251.y + r0.w
    {0xcf683c28a992bf41ull,13,0,0,64.0f}, // shader-translation-v41full-pack-02/shader-0059 (lightTile_directional_spec): r1=P.xywz, r0.x = r1.z * c251.y + r1.w
    {0xeb0e904fec45342aull,13,0,0,64.0f}, // shader-translation-v41full-pack-02/shader-0062 (..._spec_shad): r1=P.xywz, r0.x = r1.z * c251.y + r1.w
    {0x10a689db49db31deull,13,0,0,64.0f}, // shader-translation-v41full-pack-02/shader-0065 (..._spec_shad_sync): r1=P.xywz, r0.x = r1.z * c251.y + r1.w
    {0x2914764a4d77e34eull,13,0,1,64.0f}, // shader-translation-v41full-pack-01/shader-0107 (PassthroughCompositeTiled): r0=P.xzyw, r0.w * c251.y + r0.y
    {0x653d63918ff18cfeull,13,0,1,64.0f}, // shader-translation-v41full-pack-01/shader-0109 (DepthEffects_Tiled): r0=P.zwxy, r0.y * c250.x + r0.x
    {0x792465658dc46a25ull,13,0,1,64.0f}, // shader-translation-v41full-pack-01/shader-0118 (PassthroughCompositeHighDofTiled): r0.w * c250.y + r0.z (v108 counted an r0.y write before the POSITION fetch)
    // v140: VS_shadowTile (shader-translation-v41full-pack-02/shader-0068): r1.z = c249.x(64) * r0.w (scalar unit),
    // r2.xyz = r1.xyz + r0.wzz, so r2.z = POSITION.w * 64 + POSITION.z; the TEXCOORD0 vfetch reads r2.z. Without it the
    // sun-shadow tile pass read TEXCOORD0 by vertex index: every tile row from 24 down was shadowed (v138 play).
    {0x85061dc54f43f83full,13,0,2,64.0f,2},
};
inline const ComputedIndexRule* computedIndexRule(std::uint64_t vs,unsigned location) {
    for(const auto& r:ComputedIndexRules)if(r.vs==vs && r.location==location)return &r;
    return nullptr;
}
inline bool hasComputedIndexRule(std::uint64_t vs){for(const auto& r:ComputedIndexRules)if(r.vs==vs)return true;return false;}
// The vfetch at instruction slot `address`: the index register (src_reg, word 0 bits 5-10) and its component
// (src_sel, bits 30-31). A shader fetching by vertex index reads r0.x.
struct IndexSource {unsigned reg=0,component=0;bool valid=false;};
inline IndexSource vfetchIndexSource(const std::vector<std::uint8_t>& code,unsigned address) {
    const std::size_t at=std::size_t{address}*12;
    if(at+12>code.size())return {};
    const std::uint32_t w0=(std::uint32_t{code[at]}<<24)|(std::uint32_t{code[at+1]}<<16)|(std::uint32_t{code[at+2]}<<8)|code[at+3];
    return {(w0>>5)&63u,(w0>>30)&3u,true};
}
// The index a rule computes from a decoded POSITION (an integer on the hardware: out of range -> nullopt).
inline std::optional<std::uint32_t> ruleIndex(const ComputedIndexRule& r,const std::array<float,4>& position) {
    const double v=double(position[3])*double(r.scaleW)+double(position[2]);
    if(!std::isfinite(v) || v<0 || v>double(1u<<24))return std::nullopt;
    return std::uint32_t(std::lround(v));
}

// v589 (F5, the black band on the marina water): the memory a colour resolve leaves under the destination texture format
// k_16_16_16_16_EXPAND (29) holds the same four 16-bit float components as k_16_16_16_16_FLOAT (32): the texture decoder plans
// both alike (tex::formatOf), and GTA V resolves its 80x45 water tile classification (1440:7:80 -> 04892000) as format 29 and
// fetches it as format 32 in watertex#3v. Up to v588 only 32 was accepted, so that gather was refused ("not-resolved") and the
// tile rects took TEXCOORD0 from guest memory no resolve writes: tiles collapsed and the refraction they write stayed black.
#if defined(RECOMP_V589_MUTANT_FLOAT_ONLY) // test mutant: v588's rule
inline bool float16x4Memory(unsigned textureFormat){return textureFormat==32;}
#else
inline bool float16x4Memory(unsigned textureFormat){return textureFormat==32 || textureFormat==29;}
#endif
// The memory layout of a resolve destination (its texture fetch): 2D, tiled or linear, pitch in texels (tiled: a
// multiple of 32), log2 bytes per texel.
struct Layout {bool tiled=false;unsigned pitch=0,height=0,log2Bytes=0;};
inline unsigned alignedHeight(const Layout& l){return l.tiled?(l.height+31u)&~31u:l.height;}
inline std::uint64_t footprintBytes(const Layout& l){return std::uint64_t{l.pitch}*alignedHeight(l)<<l.log2Bytes;}
// Texel coordinates (x | y << 16) of every texel slot of a layout's footprint, by memory order (texel index =
// byte offset >> log2Bytes); ~0u where the footprint has no texel of the image (padding).
inline std::vector<std::uint32_t> texelTable(const Layout& l,unsigned width) {
    std::vector<std::uint32_t> table(std::size_t(footprintBytes(l)>>l.log2Bytes),~0u);
    const unsigned rows=alignedHeight(l);
    for(unsigned y=0;y<rows;++y)for(unsigned x=0;x<l.pitch;++x) {
        const std::uint64_t byte=l.tiled?tex::tiled2D(x,y,l.pitch,l.log2Bytes):(std::uint64_t{y}*l.pitch+x)<<l.log2Bytes;
        const std::uint64_t slot=byte>>l.log2Bytes;
        if(slot<table.size() && x<width && y<l.height)table[std::size_t(slot)]=x|(y<<16);
    }
    return table;
}
// How the fetch's endian mode sees the words the resolve wrote in its own: identity (0), 16-bit halves exchanged in
// every dword (1: 16-in-32), or a byte permutation that splits 16-bit components (-1, not representable).
inline int endianComposition(unsigned resolveEndian,unsigned fetchEndian) {
    static constexpr unsigned perm[4][4]{{0,1,2,3},{1,0,3,2},{3,2,1,0},{2,3,0,1}}; // none, 8in16, 8in32, 16in32
    std::array<unsigned,4> p{};for(unsigned i=0;i<4;++i)p[i]=perm[resolveEndian&3][perm[fetchEndian&3][i]];
    if(p==std::array<unsigned,4>{0,1,2,3})return 0;
    if(p==std::array<unsigned,4>{2,3,0,1})return 1;
    return -1;
}
}
