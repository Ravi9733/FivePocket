#pragma once
// artifact A (black tree canopies): boolean-conditional exec blocks. files/private-trace/diag-cexec-bools, read ONCE at initialize().
//
// The v41 XenosRecomp emitted the Xenos control-flow instructions "conditional exec on a boolean constant" (opcodes 3, 4, 13, 14:
// run the block only while boolean constant boolAddress equals `condition`) without their test, so the block always ran. In the
// pack that is 713 blocks (all opcode 13) in 93 vertex shaders: the tree shaders' player / vehicle collision (bPlayerCollEnabled
// b0, bVehTreeColl1-3Enabled b2-b4), grass (bVehColl0-3Enabled b11-b14) and 84 vehicle shaders (switchOn b8). The game sets
// those booleans per draw and uploads the constants the blocks read (the tree's _globalEntityScale, vertex c232) only when it
// sets the boolean; with the boolean clear the console skips the block and never reads the stale constant. Running the tree
// block with a stale c232 = (0.4, 0.4, 0, 0) puts every leaf vertex on gWorldViewProj's translation row: the leaf cards have no
// area and only the canopy's dark opaque core is drawn.
//
// The fix is in the translation (shader_recompiler.cpp: `if ((g_Booleans & <bool>) !=/== 0) { block }`); the 93 shaders
// re-translated with it are a second GSP2 pack, files/private-trace/generic-shaders-cexec.bin (tools/iter/generic_shader_pack.py
// over the re-translated records: same keys, same vertex elements, new SPIR-V). With the switch file present, applyOverlay()
// appends each of its modules after the pack's bytes and points the pack's own entry at it, BEFORE initialize() takes any entry
// pointer or runs any module pass: every later reader (the blend-index pass, constant masks, v294's bank rewrite, the gc fold,
// the vertex variants, the prewarm) sees one module per key, the re-translated one. Nothing else changes: g_Booleans (shared
// word 64) already carries the draw's vertex b0-b15 / pixel b0-b15 (registers 0x4900 / 0x4904) and stays per draw.
// All or nothing: one refused overlay entry leaves the pack exactly as it was read (why names the first refusal).
// Absent switch = the pack's modules (today's behaviour). Out of line and noexcept: initialize() keeps its control flow.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>

namespace recomp::gpu::generic::cexec {

// A GSP2 entry (generic_renderer.cpp's PackEntry; written by tools/iter/generic_shader_pack.py).
struct Entry {
    std::uint64_t hash;std::uint32_t stage,spirvOffset,spirvWords,elementCount;
    std::uint32_t elements[24];std::uint32_t outputs,samplers;
    std::uint8_t elementDst[24];
};
static_assert(sizeof(Entry)==152);
constexpr std::size_t MaxOverlayEntries=512;
constexpr std::size_t MaxOverlayBytes=std::size_t{64}<<20;

struct Result {bool enabled=false;unsigned entries=0,applied=0;const char* why="-";};

// True when [p, p+n) starts with a GSP2 version 2 header whose entry table lies inside it; count = its entries.
inline bool header(const std::uint8_t* p,std::size_t n,std::uint32_t& count) noexcept {
    if(n<16 || std::memcmp(p,"GSP2",4))return false;
    std::uint32_t version;std::memcpy(&version,p+4,4);std::memcpy(&count,p+8,4);
    return version==2 && count<=(n-16)/sizeof(Entry);
}

// Points every pack entry the overlay names at the overlay's module (appended to `pack`). Vertex entries only: the pixel
// entries have their own module overrides (denorm, gradients), which an overlay would silently lose to.
[[gnu::noinline]] inline Result applyOverlay(std::vector<std::uint8_t>& pack,const std::uint8_t* overlay,std::size_t overlayBytes) noexcept {
    Result r;r.enabled=true;
    std::uint32_t packCount=0,count=0;
    if(!header(pack.data(),pack.size(),packCount)){r.why="pack-header";return r;}
    if(!overlay || !header(overlay,overlayBytes,count)){r.why="overlay-header";return r;}
    r.entries=count;
    if(count==0 || count>MaxOverlayEntries){r.why="overlay-count";return r;}
    std::uint32_t target[MaxOverlayEntries]; // overlay entry -> pack entry index
    std::size_t total=0;std::uint64_t previous=0;
    for(std::uint32_t i=0;i<count;++i) {
        Entry o;std::memcpy(&o,overlay+16+std::size_t{i}*sizeof(Entry),sizeof o);
        if(i && o.hash<=previous){r.why="overlay-order";return r;} // (the pack tool writes its keys ascending: no key twice)
        previous=o.hash;
        if(o.stage!=0){r.why="overlay-stage";return r;}
        const std::size_t bytes=std::size_t{o.spirvWords}*4;
        if(o.spirvWords<5 || o.spirvOffset>overlayBytes || bytes>overlayBytes-o.spirvOffset){r.why="overlay-extent";return r;}
        std::uint32_t magic;std::memcpy(&magic,overlay+o.spirvOffset,4);
        if(magic!=0x07230203u){r.why="overlay-spirv";return r;}
        std::uint32_t found=packCount;
        for(std::uint32_t j=0;j<packCount;++j) {
            std::uint64_t hash;std::memcpy(&hash,pack.data()+16+std::size_t{j}*sizeof(Entry),8);
            if(hash==o.hash){found=j;break;}
        }
        if(found==packCount){r.why="overlay-key-missing";return r;}
        Entry e;std::memcpy(&e,pack.data()+16+std::size_t{found}*sizeof(Entry),sizeof e);
        // The same container gives the same stage, vertex elements (address | usage | index | location, vfetch destination
        // registers), outputs and sampler mask; anything else is not this pack's shader.
        if(e.stage!=o.stage || e.elementCount!=o.elementCount || std::memcmp(e.elements,o.elements,sizeof e.elements) ||
           e.outputs!=o.outputs || e.samplers!=o.samplers || std::memcmp(e.elementDst,o.elementDst,sizeof e.elementDst)) {
            r.why="overlay-entry-differs";return r;
        }
        target[i]=found;total+=bytes+3;
    }
    if(pack.size()+total>std::size_t{0xFFFFFFFFu}){r.why="pack-size";return r;}
    try {pack.reserve(pack.size()+total);} catch(...) {r.why="alloc";return r;} // (no reallocation, so no throw, after this)
    for(std::uint32_t i=0;i<count;++i) {
        Entry o;std::memcpy(&o,overlay+16+std::size_t{i}*sizeof(Entry),sizeof o);
        while(pack.size()%4)pack.push_back(0);
        const std::uint32_t offset=std::uint32_t(pack.size());
        pack.insert(pack.end(),overlay+o.spirvOffset,overlay+o.spirvOffset+std::size_t{o.spirvWords}*4);
        std::uint8_t* entry=pack.data()+16+std::size_t{target[i]}*sizeof(Entry);
        std::memcpy(entry+offsetof(Entry,spirvOffset),&offset,4);
        std::memcpy(entry+offsetof(Entry,spirvWords),&o.spirvWords,4);
        ++r.applied;
    }
    return r;
}

// The switch file and the overlay file: absent switch = nothing read, nothing changed.
[[gnu::noinline]] inline Result applyOverlayFile(std::vector<std::uint8_t>& pack,const char* switchPath,const char* overlayPath) noexcept {
    if(access(switchPath,F_OK)!=0)return {};
    Result r;r.enabled=true;r.why="overlay-file";
    std::FILE* f=std::fopen(overlayPath,"rb");
    if(!f)return r;
    std::fseek(f,0,SEEK_END);const long n=std::ftell(f);std::fseek(f,0,SEEK_SET);
    if(n>16 && std::size_t(n)<=MaxOverlayBytes) {
        if(auto* bytes=static_cast<std::uint8_t*>(std::malloc(std::size_t(n)))) {
            if(std::fread(bytes,1,std::size_t(n),f)==std::size_t(n))r=applyOverlay(pack,bytes,std::size_t(n));
            std::free(bytes);
        }
    }
    std::fclose(f);
    return r;
}

} // namespace recomp::gpu::generic::cexec
