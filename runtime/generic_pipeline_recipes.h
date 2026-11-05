// v139: pipeline recipes. When the generic renderer creates a graphics pipeline it records the exact fixed-function
// state it passed to Vulkan, plus which pack shader modules it used (by pack hash and kind, never code). The next
// process compiles those recipes in the background during the loading screen into the in-memory VkPipelineCache, so
// the renderer's later identical vkCreateGraphicsPipelines is a cache hit instead of a ~26 ms compile mid-game (v134
// play: 863 pipelines, 22.8 s of hitches). Only this process's own pack SPIR-V is ever compiled; the cache is not
// seeded from disk (device.cpp's policy): a recipe only decides WHICH pipelines are compiled early.
// Pure encode/decode here; the renderer builds the create info (generic_renderer.cpp, "v139").
#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace recomp::gpu::generic::recipes {

enum PsKind : std::uint32_t {PsPack=0,PsGradientFixed=1,PsGradientOriginal=2,PsNone=3};

struct Recipe {
    std::uint64_t vs=0,ps=0;
    std::uint32_t psKind=PsPack,psVariant=0,spec=0;
    std::uint32_t topology=0,restart=0,cull=0,frontFace=0,depthBias=0;
    std::uint32_t depthTest=0,depthWrite=0,depthCompare=0,stencilTest=0;
    VkStencilOpState front{},back{};
    std::uint32_t colorCount=0;
    std::array<VkPipelineColorBlendAttachmentState,4> blend{};
    std::array<std::int32_t,5> formats{-1,-1,-1,-1,-1}; // colour 0-3, depth; -1 = no attachment
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    // v149: the v77 vertex-input adaptations of a variant vertex module (location, convert, feed); empty = the pack module.
    std::vector<std::array<std::uint32_t,3>> adapts;
};

constexpr std::uint32_t Magic=0x52435031u; // "RCP1"
constexpr std::uint32_t Magic2=0x52435032u; // "RCP2" (v149): RCP1 + adaptations
constexpr std::size_t MaximumListLength=32;

namespace detail {
inline void put(std::vector<std::uint32_t>& w,std::uint32_t v){w.push_back(v);}
inline void put64(std::vector<std::uint32_t>& w,std::uint64_t v){w.push_back(std::uint32_t(v));w.push_back(std::uint32_t(v>>32));}
template<class T> void putRaw(std::vector<std::uint32_t>& w,const T& v) {
    static_assert(sizeof(T)%4==0);std::uint32_t tmp[sizeof(T)/4];std::memcpy(tmp,&v,sizeof(T));
    for(auto x:tmp)w.push_back(x);
}
struct Reader {
    const std::uint32_t* p;std::size_t n,at=0;bool ok=true;
    std::uint32_t get(){if(at>=n){ok=false;return 0;}return p[at++];}
    std::uint64_t get64(){const std::uint64_t lo=get();return lo|(std::uint64_t{get()}<<32);}
    template<class T> T getRaw(){T v{};static_assert(sizeof(T)%4==0);if(at+sizeof(T)/4>n){ok=false;return v;}
        std::memcpy(&v,p+at,sizeof(T));at+=sizeof(T)/4;return v;}
};
}

// One record: Magic, word count (including these two), then the fields in declaration order.
inline std::vector<std::uint32_t> encode(const Recipe& r) {
    using namespace detail;
    std::vector<std::uint32_t> w;w.reserve(96);
    put(w,r.adapts.empty()?Magic:Magic2);put(w,0);
    put64(w,r.vs);put64(w,r.ps);
    for(auto v:{r.psKind,r.psVariant,r.spec,r.topology,r.restart,r.cull,r.frontFace,r.depthBias,
                r.depthTest,r.depthWrite,r.depthCompare,r.stencilTest})put(w,v);
    putRaw(w,r.front);putRaw(w,r.back);
    put(w,r.colorCount);
    for(const auto& a:r.blend)putRaw(w,a);
    for(auto f:r.formats)put(w,std::uint32_t(f));
    put(w,std::uint32_t(r.bindings.size()));for(const auto& b:r.bindings)putRaw(w,b);
    put(w,std::uint32_t(r.attributes.size()));for(const auto& a:r.attributes)putRaw(w,a);
    if(!r.adapts.empty()){put(w,std::uint32_t(r.adapts.size()));for(const auto& a:r.adapts)for(auto v:a)put(w,v);}
    w[1]=std::uint32_t(w.size());
    return w;
}

// Decodes every whole, well-formed record of a file's words; stops at the first malformed one (a torn tail).
inline std::vector<Recipe> decodeAll(const std::uint32_t* words,std::size_t count) {
    std::vector<Recipe> out;std::size_t at=0;
    while(at+2<=count) {
        if(words[at]!=Magic && words[at]!=Magic2)break;
        const bool v2=words[at]==Magic2;
        const std::size_t size=words[at+1];
        if(size<2 || at+size>count)break;
        detail::Reader rd{words+at+2,size-2};
        Recipe r;
        r.vs=rd.get64();r.ps=rd.get64();
        std::uint32_t* fields[]{&r.psKind,&r.psVariant,&r.spec,&r.topology,&r.restart,&r.cull,&r.frontFace,&r.depthBias,
                                &r.depthTest,&r.depthWrite,&r.depthCompare,&r.stencilTest};
        for(auto* f:fields)*f=rd.get();
        r.front=rd.getRaw<VkStencilOpState>();r.back=rd.getRaw<VkStencilOpState>();
        r.colorCount=rd.get();
        for(auto& a:r.blend)a=rd.getRaw<VkPipelineColorBlendAttachmentState>();
        for(auto& f:r.formats)f=std::int32_t(rd.get());
        const auto nb=rd.get();
        if(nb>MaximumListLength)break;
        for(std::uint32_t i=0;i<nb;++i)r.bindings.push_back(rd.getRaw<VkVertexInputBindingDescription>());
        const auto na=rd.get();
        if(na>MaximumListLength)break;
        for(std::uint32_t i=0;i<na;++i)r.attributes.push_back(rd.getRaw<VkVertexInputAttributeDescription>());
        if(v2){
            const auto nd=rd.get();
            if(nd==0 || nd>MaximumListLength)break;
            for(std::uint32_t i=0;i<nd;++i){std::array<std::uint32_t,3> a{};for(auto& v:a)v=rd.get();r.adapts.push_back(a);}
        }
        if(!rd.ok || rd.at!=rd.n || r.colorCount>4 || r.psKind>PsNone)break;
        out.push_back(std::move(r));at+=size;
    }
    return out;
}

inline std::uint64_t digest(const std::vector<std::uint32_t>& w) {
    std::uint64_t h=0xcbf29ce484222325ull;
    for(auto x:w)for(unsigned i=0;i<4;++i){h^=(x>>(8*i))&0xFF;h*=0x100000001b3ull;}
    return h;
}

} // namespace recomp::gpu::generic::recipes
