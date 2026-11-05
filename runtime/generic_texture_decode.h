#pragma once
// Attempt v41 (texture coverage): Xenos texture storage layout and CPU decode for the generic
// renderer. Pure CPU: no Vulkan, no guest memory access (callers pass owned byte copies).
//
// What v35's texture() did and this replaces (see local-analysis/rendering-audit/):
//  - base level only, GPU-generated mips (guest mip levels and packed mip tails ignored);
//  - 2D, cube and stacked only; 1D and 3D refused ("texture-dimension 3d-or-1d", sampled as grey);
//  - stacked/cube layer stride assumed to be the base extent rounded to 4 KB;
//  - sign, gamma, exponent bias and the integer select stripped (k_8 integer refused);
//  - the fetch swizzle applied straight to the host image, so single- and two-component formats
//    read Vulkan's (r,0,0,1) where the Xenos replicates (X,X,X,X) / (X,Y,Y,Y) (a DXT5A texture with
//    the fetch swizzle WWW1 sampled as 1,1,1,1); 5_6_5, 1_5_5_5 and 4_4_4_4 with swapped channels;
//    DXT2_3 decoded as DXT4_5.
//
// Semantics sources (read for semantics only, nothing copied): Xenia's xenos.h fetch layout,
// texture_util.h/.cc layout documentation (row pitch/height/depth alignment, power-of-two mip
// storage, 4 KB subresources, packed mip tails, mip range cleanup, sign swizzling),
// texture_address.h documentation (bit layout of tiled 2D/3D addresses), the Vulkan backend's
// host format table (component order and replication), and the PWL gamma curve of the SPIR-V
// translator. The 2D tiled address here is checked against the repository's existing
// xenos_texture_2d.cpp implementation by local-analysis/attempts/v41/tests.
#include "xenos_texture_description.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace recomp::gpu::generic::tex {
using Desc=XenosTextureDescription;

// Host image formats the decoder can produce (the renderer maps them to VkFormat).
enum class Host : std::uint8_t {
    None,R8,R8G8,RGBA8,RGBA8Srgb,R8S,R8G8S,RGBA8S,R5G6B5,A1R5G5B5,R4G4B4A4,A2B10G10R10,A2B10G10R10S,
    R16,R16G16,RGBA16,R16S,R16G16S,RGBA16S,R16F,R16G16F,RGBA16F,R32F,R32G32F,RGBA32F,
    BC1,BC2,BC3,BC4,BC5, // v132: block-compressed hosts (the device decodes the blocks; see choose())
    Count
};
// v132: 4x4 block formats uploaded as blocks: bytes per block (0: not a block host).
inline unsigned hostBlockBytes(Host h) {
    switch(h) {case Host::BC1: case Host::BC4: return 8;case Host::BC2: case Host::BC3: case Host::BC5: return 16;default: return 0;}
}
inline bool isBlockHost(Host h){return hostBlockBytes(h)!=0;}
inline unsigned hostBytes(Host h) {
    switch(h) {
    case Host::R8: case Host::R8S: return 1;
    case Host::R8G8: case Host::R8G8S: case Host::R5G6B5: case Host::A1R5G5B5: case Host::R4G4B4A4:
    case Host::R16: case Host::R16S: case Host::R16F: return 2;
    case Host::RGBA8: case Host::RGBA8Srgb: case Host::RGBA8S: case Host::A2B10G10R10: case Host::A2B10G10R10S:
    case Host::R16G16: case Host::R16G16S: case Host::R16G16F: case Host::R32F: return 4;
    case Host::RGBA16: case Host::RGBA16S: case Host::RGBA16F: case Host::R32G32F: return 8;
    case Host::RGBA32F: return 16;
    default: return 0;
    }
}

enum class Decode : std::uint8_t {None,Raw,BC1,BC2,BC3,BC4,BC5,DXT3A,Depth24};

// One Xenos texture format. `channel` is the host channel (0-3 = R,G,B,A of the host image) that
// holds guest component X,Y,Z,W, replication included (a one-component format replicates X into
// every component before the fetch swizzle). `repl` is the same replication in guest terms, and
// bits/shift describe the decoded texel's integer components for the conversion path.
struct Format {
    std::uint8_t blockW=1,blockH=1,blockBytes=0;Decode decode=Decode::None;
    Host host=Host::None,hostSigned=Host::None,hostSrgb=Host::None;
    std::array<std::uint8_t,4> channel{0,1,2,3},repl{0,1,2,3};
    std::uint8_t components=0,floatBits=0; // floatBits 16/32: float components (sign/integer ignored)
    std::array<std::uint8_t,4> bits{},shift{};
    std::uint8_t decodedBytes=0;           // bytes per texel after Decode (conversion path input)
    Host hostBlock=Host::None;             // v132: the same blocks as a device block format (unsigned direct use only)
};
inline Format formatOf(unsigned f) {
    Format r;
    const auto raw=[&](unsigned bytes,Host u,Host s,std::array<std::uint8_t,4> ch,std::array<std::uint8_t,4> rp,unsigned n,
                       std::array<std::uint8_t,4> b,std::array<std::uint8_t,4> sh) {
        r.blockBytes=std::uint8_t(bytes);r.decode=Decode::Raw;r.host=u;r.hostSigned=s;r.channel=ch;r.repl=rp;
        r.components=std::uint8_t(n);r.bits=b;r.shift=sh;r.decodedBytes=std::uint8_t(bytes);
    };
    const auto flt=[&](unsigned bytes,Host h,unsigned n,unsigned floatBits) {
        raw(bytes,h,Host::None,n==1?std::array<std::uint8_t,4>{0,0,0,0}:n==2?std::array<std::uint8_t,4>{0,1,1,1}:std::array<std::uint8_t,4>{0,1,2,3},
            n==1?std::array<std::uint8_t,4>{0,0,0,0}:n==2?std::array<std::uint8_t,4>{0,1,1,1}:std::array<std::uint8_t,4>{0,1,2,3},n,{},{});
        r.floatBits=std::uint8_t(floatBits);
    };
    const auto block=[&](unsigned bytes,Decode d,Host h,unsigned n,unsigned decodedBytes) {
        r.blockW=r.blockH=4;r.blockBytes=std::uint8_t(bytes);r.decode=d;r.host=h;r.components=std::uint8_t(n);
        r.decodedBytes=std::uint8_t(decodedBytes);
        if(n==1){r.channel=r.repl={0,0,0,0};r.bits={8};r.shift={0};}
        else if(n==2){r.channel=r.repl={0,1,1,1};r.bits={8,8};r.shift={0,8};}
        else {r.bits={8,8,8,8};r.shift={0,8,16,24};}
    };
    switch(f) {
    case 2: case 8: case 9: // k_8, k_8_A, k_8_B
        raw(1,Host::R8,Host::R8S,{0,0,0,0},{0,0,0,0},1,{8},{0});break;
    case 3: // k_1_5_5_5: X bits 0-4 .. W bit 15 = host B,G,R,A of A1R5G5B5
        raw(2,Host::A1R5G5B5,Host::None,{2,1,0,3},{0,1,2,3},4,{5,5,5,1},{0,5,10,15});break;
    case 4: // k_5_6_5: X bits 0-4 = host B of R5G6B5; W replicates Z
        raw(2,Host::R5G6B5,Host::None,{2,1,0,0},{0,1,2,2},3,{5,6,5},{0,5,11});break;
    case 5: // k_6_5_5: no host layout; conversion path only
        raw(2,Host::None,Host::None,{0,1,2,2},{0,1,2,2},3,{5,5,6},{0,5,10});break;
    case 6: case 14: case 50: // k_8_8_8_8, k_8_8_8_8_A, k_8_8_8_8_AS_16_16_16_16
        raw(4,Host::RGBA8,Host::RGBA8S,{0,1,2,3},{0,1,2,3},4,{8,8,8,8},{0,8,16,24});r.hostSrgb=Host::RGBA8Srgb;break;
    case 7: case 54: // k_2_10_10_10(_AS_16_16_16_16): A2B10G10R10 holds X in bits 0-9
        raw(4,Host::A2B10G10R10,Host::A2B10G10R10S,{0,1,2,3},{0,1,2,3},4,{10,10,10,2},{0,10,20,30});break;
    case 10: // k_8_8
        raw(2,Host::R8G8,Host::R8G8S,{0,1,1,1},{0,1,1,1},2,{8,8},{0,8});break;
    case 15: // k_4_4_4_4: X bits 0-3 = host A of R4G4B4A4
        raw(2,Host::R4G4B4A4,Host::None,{3,2,1,0},{0,1,2,3},4,{4,4,4,4},{0,4,8,12});break;
    case 16: case 55: // k_10_11_11: X 11 bits, Y 11 bits, Z 10 bits (conversion path)
        raw(4,Host::None,Host::None,{0,1,2,2},{0,1,2,2},3,{11,11,10},{0,11,22});break;
    case 17: case 56: // k_11_11_10: X 10 bits, Y 11 bits, Z 11 bits (conversion path)
        raw(4,Host::None,Host::None,{0,1,2,2},{0,1,2,2},3,{10,11,11},{0,10,21});break;
    case 18: case 51: block(8,Decode::BC1,Host::RGBA8,4,4);r.hostSrgb=Host::RGBA8Srgb;r.hostBlock=Host::BC1;break;
    case 19: case 52: block(16,Decode::BC2,Host::RGBA8,4,4);r.hostSrgb=Host::RGBA8Srgb;r.hostBlock=Host::BC2;break;
    case 20: case 53: block(16,Decode::BC3,Host::RGBA8,4,4);r.hostSrgb=Host::RGBA8Srgb;r.hostBlock=Host::BC3;break;
    case 22: // k_24_8: 24-bit normalized depth above 8 stencil bits -> R32F
        raw(4,Host::R32F,Host::None,{0,0,0,0},{0,0,0,0},1,{24},{8});r.decode=Decode::Depth24;r.floatBits=32;break;
    case 24: raw(2,Host::R16,Host::R16S,{0,0,0,0},{0,0,0,0},1,{16},{0});break;
    case 25: raw(4,Host::R16G16,Host::R16G16S,{0,1,1,1},{0,1,1,1},2,{16,16},{0,16});break;
    case 26: raw(8,Host::RGBA16,Host::RGBA16S,{0,1,2,3},{0,1,2,3},4,{16,16,16,16},{0,16,32,48});break;
    case 27: case 30: flt(2,Host::R16F,1,16);break;       // k_16_EXPAND, k_16_FLOAT
    case 28: case 31: flt(4,Host::R16G16F,2,16);break;    // k_16_16_EXPAND, k_16_16_FLOAT
    case 29: case 32: flt(8,Host::RGBA16F,4,16);break;    // k_16_16_16_16_EXPAND, _FLOAT
    case 36: flt(4,Host::R32F,1,32);break;
    case 37: flt(8,Host::R32G32F,2,32);break;
    case 38: flt(16,Host::RGBA32F,4,32);break;
    case 49: block(16,Decode::BC5,Host::R8G8,2,2);r.hostBlock=Host::BC5;break;   // k_DXN: BC5, X then Y
    case 58: block(8,Decode::DXT3A,Host::R8,1,1);break;    // k_DXT3A: 4-bit explicit alpha
    case 59: block(8,Decode::BC4,Host::R8,1,1);r.hostBlock=Host::BC4;break;      // k_DXT5A: BC4
    default: break;                                        // no decoder (r.decode == None)
    }
    return r;
}

// ---- tiled addresses. Bit layout (per Xenia's texture_address.h documentation): the macro tile
// index and the micro-tile offset form linear bytes L = (outer | inner) << log2(bytes per block),
// and the address is L[3:0] | y[0]<<4 | L[4]<<5 | pipe<<6 | L[7:5]<<8 | bank<<11 | L[>=8]<<12.
inline std::uint64_t tiledScatter(std::uint64_t l,unsigned y0,unsigned pipe,unsigned bank) {
    return (l&15u)|(std::uint64_t{y0}<<4)|(((l>>4)&1u)<<5)|(std::uint64_t{pipe}<<6)|(((l>>5)&7u)<<8)|
           (std::uint64_t{bank}<<11)|((l>>8)<<12);
}
// 2D: 32x32-block macro tiles row-major; micro offset {y[3:1], x[2:0]}; bank y[4]; pipe {x[4]^y[3], x[3]}.
inline std::uint64_t tiled2D(unsigned x,unsigned y,unsigned pitchBlocks,unsigned log2Bytes) {
    const std::uint64_t macro=std::uint64_t{y>>5}*(pitchBlocks>>5)+(x>>5);
    const unsigned micro=(((y>>1)&7u)<<3)|(x&7u);
    return tiledScatter(((macro<<6)|micro)<<log2Bytes,y&1u,((x>>3)&3u)^(((y>>3)&1u)<<1),(y>>4)&1u);
}
// 3D: 32x16x4-block macro tiles (x fastest, then y, then z); micro {z[1:0], y[2:1], x[2:0]};
// bank y[3]^z[2]; pipe {x[4]^bank, x[3]}.
inline std::uint64_t tiled3D(unsigned x,unsigned y,unsigned z,unsigned pitchBlocks,unsigned heightBlocks,unsigned log2Bytes) {
    const std::uint64_t macro=(std::uint64_t{z>>2}*(heightBlocks>>4)+(y>>4))*(pitchBlocks>>5)+(x>>5);
    const unsigned micro=((z&3u)<<5)|(((y>>1)&3u)<<3)|(x&7u);
    const unsigned bank=((y>>3)^(z>>2))&1u;
    return tiledScatter(((macro<<7)|micro)<<log2Bytes,y&1u,((x>>3)&3u)^(bank<<1),bank);
}
// One past the highest address any block at or below (x, y, z) can use: the macro tile of the
// corner (indices grow with every coordinate) with every micro, pipe, bank and byte bit set.
inline std::uint64_t tiledEnd(bool is3D,unsigned x,unsigned y,unsigned z,unsigned pitchBlocks,unsigned heightBlocks,unsigned log2Bytes) {
    const std::uint64_t macro=is3D?(std::uint64_t{z>>2}*(heightBlocks>>4)+(y>>4))*(pitchBlocks>>5)+(x>>5):
                                   std::uint64_t{y>>5}*(pitchBlocks>>5)+(x>>5);
    const std::uint64_t l=((((macro<<(is3D?7:6))|(is3D?127u:63u))<<log2Bytes)|((1u<<log2Bytes)-1u));
    return tiledScatter(l,1,3,1)+1;
}

// ---- storage layout
enum class Dim : std::uint8_t {OneD,TwoD,ThreeD,Cube};
struct Storage {             // one stored surface: the base, one mip level, or a packed mip tail
    bool mips=false;         // under the fetch's mip address (else its base address)
    std::uint64_t offset=0;  // from that address
    unsigned rowPitchBlocks=0,sliceRows=0,slices=1; // storage dimensions in blocks (slices: 3D)
    std::uint64_t layerStride=0; // between array layers / cube faces (3D: the whole volume)
};
struct Level {
    bool absent=false;                      // not stored (a fetch without a base: level 0, never sampled)
    unsigned width=1,height=1,depth=1;      // texels (depth: 3D slices at this level)
    unsigned blocksW=1,blocksH=1;
    unsigned originX=0,originY=0,originZ=0; // blocks inside the storage (packed mip tail position)
    Storage storage;
};
struct Plan {
    Desc desc;Format format;Dim dim=Dim::TwoD;
    unsigned width=1,height=1,depth=1,layers=1; // host image: layers = faces / array layers / 3D slices
    unsigned mipMin=0,mipMax=0;                 // sampler LOD range (host level = guest level)
    unsigned packedLevel=~0u;
    std::uint32_t baseAddress=0,mipAddress=0;
    std::vector<Level> levels;                  // host levels 0..mipMax
    std::uint64_t baseBytes=0,mipBytes=0;       // bytes needed from each address
    bool baseMissing=false;                     // the fetch has no base level (level 0 left empty; mipMin >= 1)
    bool volume=false;                          // (caller's choice) 3D as a 3D image: level l holds its own depth_l slices
};

inline unsigned log2Ceil(unsigned v){return v<=1?0u:unsigned(std::bit_width(v-1));}
inline unsigned alignUp(unsigned v,unsigned a){return (v+a-1)/a*a;}
inline std::uint64_t alignUp64(std::uint64_t v,std::uint64_t a){return (v+a-1)/a*a;}

// Block offset of mip `level` inside the packed tail (semantics of Xenia's GetPackedMipOffset,
// in texels, then converted to blocks). False when the level is not in the tail.
inline bool packedOffset(unsigned width,unsigned height,unsigned depth,unsigned level,const Format& f,
                         unsigned& x,unsigned& y,unsigned& z) {
    const unsigned lw=log2Ceil(width),lh=log2Ceil(height),size=std::min(lw,lh);
    x=y=z=0;
    if(size>4+level)return false;
    const unsigned base=size>4?size-4:0,p=level-base;
    if(p<3) { // the three largest tail levels sit beside each other at 16, 8 and 4 texels
        if(lw>lh)y=16u>>p;else x=16u>>p;
    } else {  // smaller ones along the other axis; 1x1 3D levels also along Z
        const unsigned offset=(1u<<((lw>lh?lw:lh)-base))>>(p-2);
        if(lw>lh)x=offset;else y=offset;
        if(offset<4) {
            const unsigned ld=log2Ceil(depth);
            z=ld>1+level?(ld-level)*4:4;
        }
    }
    x/=f.blockW;y/=f.blockH;
    return true;
}

// The storage surface of guest level `level` (base when !mips), dimensions per the layout rules:
// the base uses the fetch pitch and its own height/depth; mips use max(pow2(size) >> level, 1);
// row pitch and height align to 32 blocks (linear mip rows also to 256 bytes), 3D depth to 4,
// and every layer/face to 4 KB.
inline Storage storageOf(const Plan& p,bool mips,unsigned level) {
    const auto& f=p.format;Storage s;s.mips=mips;
    const unsigned rowTexels=mips?std::max(std::bit_ceil(p.width)>>level,1u):p.desc.pitchTexels;
    const unsigned rowsTexels=mips?std::max(std::bit_ceil(p.height)>>level,1u):p.height;
    const unsigned slices=p.dim!=Dim::ThreeD?1u:mips?std::max(std::bit_ceil(p.depth)>>level,1u):p.depth;
    const unsigned alignment=(!p.desc.tiled && mips)?std::max(256u/f.blockBytes,32u):32u;
    s.rowPitchBlocks=alignUp((rowTexels+f.blockW-1)/f.blockW,alignment);
    s.sliceRows=p.dim==Dim::OneD?1u:alignUp((rowsTexels+f.blockH-1)/f.blockH,32);
    s.slices=p.dim==Dim::ThreeD?alignUp(slices,4):1u;
    s.layerStride=alignUp64(std::uint64_t{s.rowPitchBlocks}*f.blockBytes*s.sliceRows*s.slices,4096);
    return s;
}

// Byte offset of block (x, y, z) of `layer` from the storage's address.
inline std::uint64_t blockAddress(const Plan& p,const Storage& s,unsigned layer,unsigned x,unsigned y,unsigned z) {
    const unsigned lb=unsigned(std::countr_zero(unsigned(p.format.blockBytes)));
    std::uint64_t inner;
    if(p.desc.tiled)inner=p.dim==Dim::ThreeD?tiled3D(x,y,z,s.rowPitchBlocks,s.sliceRows,lb):tiled2D(x,y,s.rowPitchBlocks,lb);
    else inner=((std::uint64_t{z}*s.sliceRows+y)*s.rowPitchBlocks+x)*p.format.blockBytes;
    return s.offset+std::uint64_t{layer}*s.layerStride+inner;
}
// One past the last byte `level` needs from its storage's address (every layer).
inline std::uint64_t levelEnd(const Plan& p,const Level& l) {
    const auto& s=l.storage;const unsigned lb=unsigned(std::countr_zero(unsigned(p.format.blockBytes)));
    const unsigned x=l.originX+l.blocksW-1,y=l.originY+l.blocksH-1,z=l.originZ+l.depth-1;
    std::uint64_t end;
    if(p.desc.tiled)end=tiledEnd(p.dim==Dim::ThreeD,x,y,z,s.rowPitchBlocks,s.sliceRows,lb);
    else end=alignUp64(((std::uint64_t{z}*s.sliceRows+y)*s.rowPitchBlocks+x+1)*p.format.blockBytes,4);
    const unsigned layers=p.dim==Dim::ThreeD?1u:p.layers;
    return s.offset+std::uint64_t{layers-1}*s.layerStride+end;
}

enum class Refusal : std::uint8_t {None,NotTexture,Format,Size,Tiled1D,Address,Extent};
inline const char* refusalName(Refusal r) {
    switch(r){case Refusal::NotTexture:return "not-texture";case Refusal::Format:return "format";case Refusal::Size:return "size";
    case Refusal::Tiled1D:return "tiled-1d";case Refusal::Address:return "address";case Refusal::Extent:return "extent";default:return "none";}
}

// Every host level of the texture described by the submitted fetch words.
inline std::optional<Plan> plan(const std::array<std::uint32_t,6>& fetch,Refusal& why) {
    why=Refusal::None;
    const auto decoded=Desc::decode(fetch);
    if(!decoded){why=Refusal::NotTexture;return std::nullopt;}
    Plan p;p.desc=*decoded;p.format=formatOf(p.desc.format);
    const auto& d=p.desc;const auto& f=p.format;
    if(f.decode==Decode::None){why=Refusal::Format;return std::nullopt;}
    switch(d.dimension) {
    case Desc::Dimension::OneD: p.dim=Dim::OneD;break;
    case Desc::Dimension::ThreeD: p.dim=Dim::ThreeD;break;
    case Desc::Dimension::Cube: p.dim=Dim::Cube;break;
    default: p.dim=Dim::TwoD;break;
    }
    p.width=d.width;p.height=p.dim==Dim::OneD?1u:d.height;
    p.depth=p.dim==Dim::ThreeD?d.depthOrLayers:1u;
    p.layers=p.dim==Dim::Cube?6u:p.dim==Dim::ThreeD?p.depth:p.dim==Dim::TwoD?d.depthOrLayers:1u;
    // Host limits kept by the caller's device (16384 2D, 2048 layers): refuse what cannot be bound.
    if(!p.width || !p.height || p.width>16384 || p.height>8192 || p.layers>1024 || (!d.pitchTexels && p.dim!=Dim::OneD))
        {why=Refusal::Size;return std::nullopt;}
    if(p.dim==Dim::OneD && d.tiled){why=Refusal::Tiled1D;return std::nullopt;}
    // Mip range cleanup (Xenia GetSubresourcesFromFetchConstant semantics): no mip address means
    // base only; a base equal to the mip address with mip_min > 0, or no base, starts at level 1.
    const unsigned longest=std::max({p.width,p.height,p.dim==Dim::ThreeD?p.depth:1u});
    const unsigned sizeMax=unsigned(std::bit_width(longest))-1;
    // Pages compare in the 512 MB physical space (a submitted fetch may hold a virtual address);
    // the addresses themselves stay in the caller's domain for its reader.
    std::uint32_t basePage=(d.baseAddress>>12)&0x1FFFFu,mipPage=(d.mipAddress>>12)&0x1FFFFu;
    unsigned mipMin=0,mipMax=0;
    if(mipPage) {mipMin=std::min(d.mipMin,sizeMax);mipMax=std::max(std::min(d.mipMax,sizeMax),mipMin);}
    if(mipMax) {
        if(mipMin && basePage==mipPage)basePage=0;
        if(!basePage)mipMin=std::max(mipMin,1u);
    } else mipPage=0;
    if(!basePage && !mipPage){why=Refusal::Address;return std::nullopt;}
    p.mipMin=mipMin;p.mipMax=mipMax;p.baseMissing=!basePage;
    p.baseAddress=basePage?d.baseAddress:0;p.mipAddress=mipPage?d.mipAddress:0;
    p.packedLevel=d.packedMips?(std::min(log2Ceil(p.width),log2Ceil(p.height))>4?std::min(log2Ceil(p.width),log2Ceil(p.height))-4:0u):~0u;
    // Mip storage offsets: level 1 at the mip address, each stored level after the previous one
    // (non-base 2D arrays count their layers rounded up to 4).
    const unsigned mipLayers=p.dim==Dim::TwoD && p.layers>1?alignUp(p.layers,4):p.dim==Dim::ThreeD?1u:p.layers;
    std::vector<Storage> mipStorage(16);
    {
        std::uint64_t offset=0;
        const unsigned last=std::min(mipMax,p.packedLevel);
        if(p.packedLevel==0) { // the mips' tail is stored like level 0 with mip strides
            mipStorage[0]=storageOf(p,true,0);mipStorage[0].offset=0;
        } else for(unsigned l=1;l<=last && l<16;++l) {
            mipStorage[l]=storageOf(p,true,l);mipStorage[l].offset=offset;
            offset+=mipStorage[l].layerStride*mipLayers;
        }
    }
    const auto base=storageOf(p,false,0);
    for(unsigned l=0;l<=mipMax;++l) {
        Level v;v.width=std::max(p.width>>l,1u);v.height=std::max(p.height>>l,1u);
        v.depth=p.dim==Dim::ThreeD?std::max(p.depth>>l,1u):1u;
        v.blocksW=(v.width+f.blockW-1)/f.blockW;v.blocksH=(v.height+f.blockH-1)/f.blockH;
        const bool inTail=p.packedLevel!=~0u && l>=p.packedLevel;
        if(l==0 && p.baseMissing){v.absent=true;p.levels.push_back(v);continue;}
        if(l==0)v.storage=base;
        else v.storage=mipStorage[std::min(inTail?p.packedLevel:l,15u)];
        if(inTail) {
            unsigned x,y,z;packedOffset(p.width,p.height,p.depth,l,f,x,y,z);
            v.originX=x;v.originY=y;v.originZ=p.dim==Dim::ThreeD?z:0;
        }
        const auto end=levelEnd(p,v);
        auto& total=v.storage.mips?p.mipBytes:p.baseBytes;total=std::max(total,end);
        p.levels.push_back(v);
    }
    constexpr std::uint64_t Limit=96ull*1024*1024;
    if(p.baseBytes>Limit || p.mipBytes>Limit || std::uint64_t{p.baseAddress}+p.baseBytes>0x100000000ull ||
       std::uint64_t{p.mipAddress}+p.mipBytes>0x100000000ull){why=Refusal::Extent;return std::nullopt;}
    return p;
}

// ---- component conversion (the path for sign, gamma, exponent bias, integer and formats with
// no host layout): every texel becomes RGBA16F with the fetch swizzle already applied.
// Round-to-nearest-even float -> half.
inline std::uint16_t toHalf(float v) {
    std::uint32_t x;std::memcpy(&x,&v,4);
    const std::uint32_t sign=(x>>16)&0x8000u;x&=0x7FFFFFFFu;
    if(x>=0x7F800000u)return std::uint16_t(sign|(x>0x7F800000u?0x7E00u:0x7C00u)); // inf / nan
    if(x>=0x477FF000u)return std::uint16_t(sign|0x7C00u);  // rounds to 65520 or more: inf
    if(x>=0x38800000u) {                                   // normal half (2^-14 and up)
        std::uint32_t r=(x-0x38000000u)>>13;const std::uint32_t rem=x&0x1FFFu;
        if(rem>0x1000u || (rem==0x1000u && (r&1u)))++r;
        return std::uint16_t(sign|r);
    }
    if(x<0x33000000u)return std::uint16_t(sign);           // below 2^-25: zero
    const unsigned shift=126u-(x>>23);                     // subnormal half: units of 2^-24
    const std::uint32_t m=(x&0x7FFFFFu)|0x800000u;
    std::uint32_t r=m>>shift;const std::uint32_t rem=m&((1u<<shift)-1u),half=1u<<(shift-1);
    if(rem>half || (rem==half && (r&1u)))++r;
    return std::uint16_t(sign|r);
}
inline float fromHalf(std::uint16_t h) {
    const unsigned e=(h>>10)&31u,m=h&1023u;
    float r=e==31?(m?NAN:INFINITY):e?std::ldexp(1.0f+m/1024.0f,int(e)-15):std::ldexp(m/1024.0f,-14);
    return (h&0x8000u)?-r:r;
}
// Xbox 360 piecewise-linear degamma (the PWL curve of Xenia's translator): four segments on
// the 8-bit scale with slopes 1/1023, 2/1023, 4/1023, 8/1023 and breakpoints 64, 96, 192.
inline float pwlGammaToLinear(float g) {
    g=std::isnan(g)?0.0f:std::clamp(g,0.0f,1.0f);
    const float scaled=g*255.0f;
    float scale,offset;
    if(scaled>=192.0f){scale=8.0f;offset=-1024.0f;}
    else if(scaled>=96.0f){scale=4.0f;offset=-256.0f;}
    else if(scaled>=64.0f){scale=2.0f;offset=-64.0f;}
    else {scale=1.0f;offset=0.0f;}
    float linear=scaled*scale+offset;
    linear+=std::trunc(linear*scale/1024.0f);
    return linear/1023.0f;
}

// How a planned texture is uploaded: the host format, the view swizzle (host channel 0-3, or
// 4 = zero, 5 = one) and whether texels go through the conversion path.
struct Choice {Host host=Host::None;std::array<std::uint8_t,4> swizzle{0,1,2,3};bool convert=false;const char* why="";};
// `supported(h)`: the device samples and linearly filters h. `srgb`: gamma components may use an
// sRGB host format (the sRGB curve, not the 360's PWL degamma: faster, approximate); else the
// conversion path applies the PWL curve exactly.
// v132: `blocks`: an unsigned direct use of a BC1-BC5 texture takes the device block format (f.hostBlock) when supported:
// the same channels and swizzle as the decoded RGBA8/R8/R8G8 host, the texels decoded by the sampler instead of here.
template<class Supported> Choice choose(const Plan& p,const Supported& supported,bool srgb=false,bool blocks=false) {
    const auto& d=p.desc;const auto& f=p.format;Choice c;
    bool anyVariable=false,unsignedAll=true,signedAll=true,gammaRgb=true;
    for(unsigned i=0;i<4;++i) {
        const unsigned s=d.swizzle[i];
        if(s>=4)continue;
        anyVariable=true;
        const unsigned sign=d.sign[s];
        unsignedAll=unsignedAll && sign==0;signedAll=signedAll && sign==1;
        // sRGB host formats linearize R, G, B (guest X, Y, Z of an 8888-type layout), never A
        gammaRgb=gammaRgb && (s<3?sign==3:sign==0);
    }
    const bool isFloat=f.floatBits!=0;
    const bool integer=d.integerComponents && !isFloat;
    Host direct=Host::None;
    if(!anyVariable)direct=f.host;                         // only constant components: data unused
    else if(d.exponentBias){c.why="exponent-bias";}
    else if(integer){c.why="integer";}
    else if(isFloat || unsignedAll)direct=blocks && f.hostBlock!=Host::None && supported(f.hostBlock)?f.hostBlock:f.host;
    else if(signedAll){direct=f.hostSigned;if(direct==Host::None)c.why="signed";}
    else if(srgb && gammaRgb && f.hostSrgb!=Host::None)direct=f.hostSrgb;
    else c.why="sign-mix";
    if(direct!=Host::None && !supported(direct)){c.why="host-format";direct=Host::None;}
    if(direct==Host::None && !*c.why)c.why="no-host-layout";
    if(direct!=Host::None) {
        c.host=direct;
        for(unsigned i=0;i<4;++i){const unsigned s=d.swizzle[i];c.swizzle[i]=std::uint8_t(s<4?f.channel[s]:s==4?4:5);}
        return c;
    }
    c.host=Host::RGBA16F;c.convert=true;c.swizzle={0,1,2,3};
    return c;
}

// ---- block decoders (after the endian fix; standard BC1-BC5 and Xenos DXT3A)
inline void bc1Colors(const std::uint8_t* b,bool fourAlways,std::array<std::array<std::uint8_t,4>,4>& c) {
    const unsigned c0=b[0]|(unsigned(b[1])<<8),c1=b[2]|(unsigned(b[3])<<8);
    const auto unpack=[](unsigned v){const unsigned r=v>>11,g=(v>>5)&63u,bl=v&31u;
        return std::array<std::uint8_t,4>{std::uint8_t((r<<3)|(r>>2)),std::uint8_t((g<<2)|(g>>4)),std::uint8_t((bl<<3)|(bl>>2)),255};};
    c[0]=unpack(c0);c[1]=unpack(c1);
    const bool four=fourAlways || c0>c1;
    for(unsigned k=0;k<3;++k) {
        c[2][k]=std::uint8_t(four?(2*c[0][k]+c[1][k])/3:(c[0][k]+c[1][k])/2);
        c[3][k]=std::uint8_t(four?(c[0][k]+2*c[1][k])/3:0);
    }
    c[2][3]=255;c[3][3]=four?255:0;
}
inline void bc4Values(const std::uint8_t* b,std::array<std::uint8_t,16>& out) {
    std::array<std::uint8_t,8> a{b[0],b[1]};
    const unsigned divisor=a[0]>a[1]?7:5;
    for(unsigned i=1;i<divisor;++i)a[i+1]=std::uint8_t(((divisor-i)*a[0]+i*a[1])/divisor);
    if(divisor==5){a[6]=0;a[7]=255;}
    std::uint64_t sel=0;for(unsigned i=0;i<6;++i)sel|=std::uint64_t{b[2+i]}<<(8*i);
    for(unsigned t=0;t<16;++t)out[t]=a[(sel>>(3*t))&7u];
}
// Decodes one block into `texels` (blockW*blockH texels of f.decodedBytes each, row-major).
inline void decodeBlock(const Format& f,const std::uint8_t* b,std::uint8_t* texels) {
    switch(f.decode) {
    case Decode::BC1: case Decode::BC2: case Decode::BC3: {
        const std::uint8_t* color=f.decode==Decode::BC1?b:b+8;
        std::array<std::array<std::uint8_t,4>,4> c{};bc1Colors(color,f.decode!=Decode::BC1,c);
        const std::uint32_t sel=std::uint32_t{color[4]}|(std::uint32_t{color[5]}<<8)|(std::uint32_t{color[6]}<<16)|(std::uint32_t{color[7]}<<24);
        std::array<std::uint8_t,16> alpha{};
        if(f.decode==Decode::BC3)bc4Values(b,alpha);
        for(unsigned t=0;t<16;++t) {
            auto v=c[(sel>>(2*t))&3u];
            if(f.decode==Decode::BC3)v[3]=alpha[t];
            else if(f.decode==Decode::BC2){const unsigned n=(b[t/2]>>(4*(t&1)))&15u;v[3]=std::uint8_t(n*17);}
            std::memcpy(texels+4*t,v.data(),4);
        }
        break;
    }
    case Decode::BC4: {std::array<std::uint8_t,16> a{};bc4Values(b,a);std::memcpy(texels,a.data(),16);break;}
    case Decode::BC5: {
        std::array<std::uint8_t,16> x{},y{};bc4Values(b,x);bc4Values(b+8,y);
        for(unsigned t=0;t<16;++t){texels[2*t]=x[t];texels[2*t+1]=y[t];}
        break;
    }
    case Decode::DXT3A: for(unsigned t=0;t<16;++t)texels[t]=std::uint8_t(((b[t/2]>>(4*(t&1)))&15u)*17);break;
    default: break;
    }
}

// Converts one decoded texel (f.decodedBytes, little-endian) to the conversion path's four
// output components, with the fetch swizzle, signs, integer select and exponent bias applied.
inline void convertTexel(const Plan& p,const std::uint8_t* texel,std::array<float,4>& out) {
    const auto& d=p.desc;const auto& f=p.format;
    std::uint64_t word=0;std::memcpy(&word,texel,std::min<unsigned>(f.decodedBytes,8));
    const float scale=std::ldexp(1.0f,d.exponentBias);
    for(unsigned i=0;i<4;++i) {
        const unsigned s=d.swizzle[i];
        if(s>=4){out[i]=s==4?0.0f:1.0f;continue;}
        const unsigned c=f.repl[s];float v;
        if(f.floatBits==16){std::uint16_t h;std::memcpy(&h,texel+2*c,2);v=fromHalf(h);}
        else if(f.floatBits==32 && f.decode!=Decode::Depth24){std::memcpy(&v,texel+4*c,4);}
        else {
            const unsigned bits=f.bits[c];const std::uint64_t mask=(std::uint64_t{1}<<bits)-1;
            const std::uint64_t raw=(word>>f.shift[c])&mask;const float maxv=float(mask);
            const unsigned sign=bits<2?0u:d.sign[s]; // one-bit components are never signed
            const std::int64_t sext=(raw&(std::uint64_t{1}<<(bits-1)))?std::int64_t(raw)-std::int64_t(mask)-1:std::int64_t(raw);
            if(f.decode==Decode::Depth24)v=float(raw)/maxv;
            else if(d.integerComponents)v=sign==1?float(sext):float(raw);
            else if(sign==1)v=std::max(float(sext)/float((mask>>1)),-1.0f);
            else if(sign==2)v=2.0f*float(raw)/maxv-1.0f;
            else if(sign==3)v=pwlGammaToLinear(float(raw)/maxv);
            else v=float(raw)/maxv;
        }
        out[i]=v*scale;
    }
}

// Decodes every host level of the plan. `base`/`mips` are owned copies starting at the plan's
// base and mip addresses. Output: level after level, each `layers` host layers of width x height
// texels of hostBytes(choice.host). 3D: with p.volume, level l holds its depth_l slices (a 3D
// image); else `layers` = depth at every level and layer k of level l holds guest slice
// floor(k*depth_l/depth) (a 2D array).
struct Decoded {std::vector<std::uint8_t> bytes;std::vector<std::size_t> levelOffset;};
// v86: the row copy below is on by default; the host test turns it off to compare against the per-block loop.
inline bool decodeRowCopyEnabled=true;
inline std::optional<Decoded> decode(const Plan& p,const Choice& c,const std::uint8_t* base,std::size_t baseSize,
                                     const std::uint8_t* mips,std::size_t mipSize) {
    const auto& f=p.format;
    // v132: a block host keeps the blocks after the endian fix, row-major blocks per level and layer (tightly packed rows
    // of ceil(width/4) blocks, what vkCmdCopyBufferToImage expects for a block format with bufferRowLength 0).
    const unsigned blockOut=hostBlockBytes(c.host);
    if(blockOut && (c.convert || blockOut!=f.blockBytes || f.blockW!=4 || f.blockH!=4))return std::nullopt;
    const unsigned outBytes=blockOut?blockOut:hostBytes(c.host);
    if(!outBytes || !f.blockBytes)return std::nullopt;
    constexpr unsigned EndianXor[]{0,1,3,2};const unsigned x4=EndianXor[p.desc.endian&3];
    Decoded r;std::size_t total=0;
    const bool volume=p.dim==Dim::ThreeD && p.volume;
    // v86: linear textures of one-texel blocks copied as the host stores them are read a row at a time: the
    // same bytes, and the same refusal when one is out of range, as the per-block loop below (consecutive x
    // of a linear row are consecutive blocks; the endian XOR stays per byte). The Scaleform glyph cache
    // (1024x1024 k_8, linear) is re-read on every unlock; per block it cost ~1M blockAddress calls.
    const bool rowCopy=decodeRowCopyEnabled && f.decode==Decode::Raw && !c.convert && f.blockW==1 && f.blockH==1 && !p.desc.tiled && outBytes==f.blockBytes;
    const auto levelBytes=[&](const Level& l)->std::size_t{return blockOut?std::size_t{(l.width+3)/4}*((l.height+3)/4)*blockOut:std::size_t{l.width}*l.height*outBytes;};
    for(const auto& l:p.levels){r.levelOffset.push_back(total);total+=levelBytes(l)*(volume?l.depth:p.layers);total=(total+15)&~std::size_t{15};}
    if(total>256u*1024*1024)return std::nullopt;
    try {r.bytes.assign(total,0);} catch(const std::bad_alloc&) {return std::nullopt;}
    std::array<std::uint8_t,16> blockBytes{};std::array<std::uint8_t,16*4> texels{};
    for(std::size_t li=0;li<p.levels.size();++li) {
        const auto& l=p.levels[li];
        if(l.absent)continue; // zeros; the sampler's minimum LOD never reads it
        const std::uint8_t* src=l.storage.mips?mips:base;const std::size_t size=l.storage.mips?mipSize:baseSize;
        if(!src)return std::nullopt;
        std::uint8_t* level=r.bytes.data()+r.levelOffset[li];
        for(unsigned layer=0;layer<(volume?l.depth:p.layers);++layer) {
            const unsigned guestLayer=p.dim==Dim::ThreeD?0u:layer;
            const unsigned slice=volume?layer:p.dim==Dim::ThreeD?unsigned(std::uint64_t{layer}*l.depth/p.depth):0u;
            std::uint8_t* out=level+std::size_t{layer}*levelBytes(l);
            if(rowCopy) {
                const std::size_t rowBytes=std::size_t{l.width}*f.blockBytes;
                for(unsigned y=0;y<l.height;++y) {
                    const auto at=blockAddress(p,l.storage,guestLayer,l.originX,l.originY+y,l.originZ+slice);
                    std::uint8_t* o=out+std::size_t{y}*rowBytes;
                    if(!x4) {
                        if(at+rowBytes>size)return std::nullopt;
                        std::memcpy(o,src+at,rowBytes);
                    } else for(std::size_t k=0;k<rowBytes;++k) {
                        const auto s=(at+k)^x4;
                        if(s>=size)return std::nullopt;
                        o[k]=src[s];
                    }
                }
                continue;
            }
            for(unsigned by=0;by<l.blocksH;++by)for(unsigned bx=0;bx<l.blocksW;++bx) {
                const auto at=blockAddress(p,l.storage,guestLayer,l.originX+bx,l.originY+by,l.originZ+slice);
                for(unsigned k=0;k<f.blockBytes;++k) {
                    const auto o=(at+k)^x4;
                    if(o>=size)return std::nullopt;
                    blockBytes[k]=src[o];
                }
                if(blockOut) { // v132: the fixed block as is (blocks past the level's block extent are not stored)
                    const unsigned rowBlocks=(l.width+3)/4;
                    if(bx<rowBlocks && by<(l.height+3)/4)std::memcpy(out+(std::size_t{by}*rowBlocks+bx)*blockOut,blockBytes.data(),blockOut);
                    continue;
                }
                const std::uint8_t* texel=blockBytes.data();
                if(f.decode!=Decode::Raw && f.decode!=Decode::Depth24){decodeBlock(f,blockBytes.data(),texels.data());texel=texels.data();}
                for(unsigned ty=0;ty<f.blockH;++ty)for(unsigned tx=0;tx<f.blockW;++tx) {
                    const unsigned px=bx*f.blockW+tx,py=by*f.blockH+ty;
                    if(px>=l.width || py>=l.height)continue;
                    const std::uint8_t* t=texel+(std::size_t{ty}*f.blockW+tx)*f.decodedBytes;
                    std::uint8_t* o=out+(std::size_t{py}*l.width+px)*outBytes;
                    if(c.convert) {
                        std::array<float,4> v{};convertTexel(p,t,v);
                        for(unsigned i=0;i<4;++i){const auto h=toHalf(v[i]);std::memcpy(o+2*i,&h,2);}
                    } else if(f.decode==Decode::Depth24) {
                        std::uint32_t w;std::memcpy(&w,t,4);const float v=float(w>>8)/16777215.0f;std::memcpy(o,&v,4);
                    } else std::memcpy(o,t,outBytes);
                }
            }
        }
    }
    return r;
}

// Words of the fetch that decide the decoded bytes and the view (the key of a cached image):
// everything except the sampler's filters, clamps, anisotropy, LOD bias and border colour.
inline std::array<std::uint32_t,6> contentWords(const std::array<std::uint32_t,6>& w) {
    return {w[0]&~(0x1FFu<<10),w[1],w[2],w[3]&((1u<<19)-1u),w[4]&(0xFFu<<2),w[5]&~0x1FFu};
}
}
