#pragma once
// gpu-cost (diag-gc-index-fold): the shared constant block's draw-invariant words as SPIR-V constants.
//
// The pack's modules read a 512-byte "shared" block (descriptor set 4, binding 2: a storage buffer, SPIR-V 1.0 BufferBlock
// form, one runtime array of 32-bit words) through constant-index access chains (OpAccessChain %block %int_0 %int_K). The
// renderer's draw() fills that block the same way for every draw (generic_renderer.cpp, "std::array<std::uint32_t,128>
// shared{}"): words 0-15 / 16-31 / 32-47 / 48-63 are the 2D / 2D-array / cube / sampler descriptor index of fetch slot r, and
// always r (the per-draw descriptor sets put slot r at element r); words 64 (g_Booleans), 70-71 (g_HalfPixelOffset), 77
// (g_AlphaThreshold), 80 (g_Texture3DMask) and 96-98 (the generalized alpha test's S, E, C) change per draw; every other word
// stays 0 (g_Swapped* 65-69, g_ClipPlane 72-75, g_ClipPlaneEnabled 76, ...: never written).
//
// foldSharedWords() turns each OpLoad of a draw-invariant word into an OpConstant of the same result type and result id with
// that word's value, declared just before the module's first OpFunction: every use of the result id now names a constant (a
// texture / sampler heap OpAccessChain gets a constant element index; the vertex shaders' clip-plane test and texcoord /
// normal swaps fold away). Nothing else changes: same ids (the bound stays), same decorations, same interface, the block
// variable and its other loads stay. The loaded bits and the constant's bits are the same 32-bit word for every draw, so
// every value the module computes is unchanged (exact by construction; generic_renderer.cpp audits the block it writes).
// A load is left as it is when: its access chain is not the exact two-constant-index form, the index's word is not
// draw-invariant (or >= 128), the load is volatile or has other memory operands, the result is decorated (OpDecorate /
// OpMemberDecorate / OpDecorateId / OpDecorateString name it), or its result type is not a 32-bit integer. A module with
// OpGroupDecorate / OpGroupMemberDecorate, without a shared block or without a foldable load is returned empty (why set).
#include <cstddef>
#include <cstdint>
#include <vector>

namespace recomp::gpu::generic::spirv_gc {

constexpr unsigned SharedWords=128; // the 512 bytes draw() writes
// True when word k of the shared block has the same value v for every draw (see above).
constexpr bool sharedWordConstant(std::uint32_t k,std::uint32_t& v) {
    if(k>=SharedWords)return false;
#if defined(RECOMP_GC_MUTANT_TABLE) // test mutant: the descriptor-index words taken as k (the array / cube / sampler rows are k & 15)
    if(k<64){v=k;return true;}
#else
    if(k<64){v=k&15u;return true;}
#endif
    switch(k) {
#if !defined(RECOMP_GC_MUTANT_BOOLEANS) // test mutant: g_Booleans (per draw) folded as 0
    case 64:
#endif
    case 70:case 71:case 77:case 80:case 96:case 97:case 98: return false;
    default: v=0;return true;
    }
}

struct FoldResult {std::vector<std::uint32_t> code;unsigned folded=0,kept=0;const char* why=nullptr;};

inline FoldResult foldSharedWords(const std::uint32_t* w,std::size_t n) {
    FoldResult r;
    if(n<5 || w[0]!=0x07230203u){r.why="header";return r;}
    constexpr unsigned OpTypeInt=21,OpConstant=43,OpVariable=59,OpLoad=61,OpAccessChain=65,OpInBoundsAccessChain=66,OpFunction=54,
        OpDecorate=71,OpMemberDecorate=72,OpGroupDecorate=74,OpGroupMemberDecorate=75,OpDecorateId=332,OpDecorateString=5632,
        OpMemberDecorateString=5633;
    constexpr unsigned DecBinding=33,DecSet=34;
    const std::uint32_t bound=w[3];
    std::vector<std::int64_t> set(bound,-1),binding(bound,-1),constant(bound,-1);
    std::vector<unsigned char> int32(bound,0),decorated(bound,0),sharedVar(bound,0);
    std::vector<std::int32_t> chainWord(bound,-1); // access chain id -> constant word index into the shared block
    struct Load {std::size_t at;std::uint32_t type,id,value;};
    std::vector<Load> loads;
    std::size_t firstFunction=0,i=5;
    while(i<n) {
        const unsigned op=w[i]&0xFFFFu,len=w[i]>>16;
        if(!len || i+len>n){r.why="truncated";return r;}
        const std::uint32_t* a=w+i+1;
        const auto ok=[&](unsigned k){return k+1<len && a[k]<bound;};
        switch(op) {
        case OpDecorate:
            if(len>=3 && ok(0)) {
                decorated[a[0]]=1;
                if(len>=4 && a[1]==DecSet)set[a[0]]=a[2];
                else if(len>=4 && a[1]==DecBinding)binding[a[0]]=a[2];
            }
            break;
        case OpMemberDecorate: case OpDecorateId: case OpDecorateString: case OpMemberDecorateString:
            if(len>=2 && ok(0))decorated[a[0]]=1;
            break;
        case OpGroupDecorate: case OpGroupMemberDecorate: r.why="group-decorate";return r;
        case OpTypeInt: if(len>=4 && ok(0) && a[1]==32)int32[a[0]]=1; break;
        case OpConstant: if(len==4 && ok(1) && a[0]<bound && int32[a[0]])constant[a[1]]=a[2]; break;
        case OpVariable: if(len>=4 && ok(1) && set[a[1]]==4 && binding[a[1]]==2)sharedVar[a[1]]=1; break;
        case OpFunction: if(!firstFunction)firstFunction=i; break;
        case OpAccessChain: case OpInBoundsAccessChain:
            // result type, result, base, index 0 (the struct member: 0), index 1 (the word)
            if(len==6 && ok(1) && ok(2) && sharedVar[a[2]] && a[3]<bound && a[4]<bound && constant[a[3]]==0 && constant[a[4]]>=0)
                chainWord[a[1]]=std::int32_t(constant[a[4]]>=std::int64_t(SharedWords)?-2:constant[a[4]]); // (-2: out of the block)
            break;
        case OpLoad:
            if(len>=4 && ok(1) && ok(2) && chainWord[a[2]]>=0) {
                std::uint32_t v=0;
#if defined(RECOMP_GC_MUTANT_MEMORY_OPERANDS) // test mutant: a load with memory operands (e.g. Volatile) folded too
                const bool plain=true;
#else
                const bool plain=len==4;
#endif
                if(plain && a[0]<bound && int32[a[0]] && !decorated[a[1]] && sharedWordConstant(std::uint32_t(chainWord[a[2]]),v))
                    loads.push_back({i,a[0],a[1],v});
                else ++r.kept;
            } else if(len>=4 && ok(2) && chainWord[a[2]]==-2)++r.kept;
            break;
        default: break;
        }
        i+=len;
    }
    if(!firstFunction){r.why="no-function";return r;}
    bool anyShared=false;for(std::uint32_t k=0;k<bound;++k)anyShared=anyShared || sharedVar[k];
    if(!anyShared){r.why="no-shared-block";return r;}
    if(loads.empty()){r.why="nothing-to-fold";return r;}
    // decorations come before every function: a load result decorated later than its load cannot exist (checked anyway)
    for(const auto& l:loads)if(decorated[l.id]){r.why="decorated-load";return r;}
    r.code.reserve(n+loads.size());
    r.code.insert(r.code.end(),w,w+5);
    std::size_t next=0; // the next folded load (in stream order)
    i=5;
    while(i<n) {
        const unsigned len=w[i]>>16;
        if(i==firstFunction) {
#if !defined(RECOMP_GC_MUTANT_LATE_CONSTANTS) // test mutant: the constants emitted inside the first function (invalid SPIR-V)
            for(const auto& l:loads){r.code.push_back((4u<<16)|OpConstant);r.code.push_back(l.type);r.code.push_back(l.id);r.code.push_back(l.value);}
#endif
        }
        if(next<loads.size() && loads[next].at==i){++next;i+=len;continue;} // the folded load
        r.code.insert(r.code.end(),w+i,w+i+len);
#if defined(RECOMP_GC_MUTANT_LATE_CONSTANTS)
        if(i==firstFunction)for(const auto& l:loads){r.code.push_back((4u<<16)|OpConstant);r.code.push_back(l.type);r.code.push_back(l.id);r.code.push_back(l.value);}
#endif
        i+=len;
    }
    r.folded=unsigned(loads.size());
    return r;
}

} // namespace recomp::gpu::generic::spirv_gc
