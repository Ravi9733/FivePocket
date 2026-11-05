#pragma once
// Attempt v85: pixel shader SPIR-V edits for the Xenos alpha test and for discards.
//
// XenosRecomp (the translator of the pack) ends every pixel shader return with
//     BRANCH if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST) { clip(output.oC0.w - g_AlphaThreshold); }
// which DXC compiles to (spot.fxc #50, v41 pack):
//     %p = OpAccessChain %_ptr_Uniform_uint %g_SharedConstantsBuffer %uint_0 %uint_77   (byte 308)
//     %u = OpLoad %uint %p ; %t = OpBitcast %float %u
//     %d = OpFSub %float %alpha %t
//     %k = OpFOrdLessThan %bool %d %float_0
//     OpSelectionMerge %m None ; OpBranchConditional %k %kill %m ; %kill = OpLabel ; OpKill
// i.e. only the GREATER_EQUAL alpha function. The Xenos RB_COLORCONTROL alpha_func has eight (NEVER,
// LESS, EQUAL, LEQUAL, GREATER, NOTEQUAL, GEQUAL, ALWAYS); GTA V uses LEQUAL (im.fxc #73: the stencil
// mask of postfx.fxc #80), GREATER and NOTEQUAL (v67 census).
//
// generalizeAlphaTest() rewrites every such site to kill when
//     x = S*d + E*|d| + C < 0        (d = alpha - threshold, as before)
// with S, E, C read from three more shared-constant words (the renderer's alphaTestState() picks them per
// function; S=1, E=0, C=0 is the translator's own test). |d| is OpSelect(d < 0, -d, d): no extended
// instruction import is needed. Only instructions are inserted and the compare's first operand replaced.
//
// XenosRecomp also gives every pixel shader that does not export depth [earlydepthstencil]
// (OpExecutionMode EarlyFragmentTests), including the ones that discard (the alpha test above, and the
// microcode kill instructions it translates to clip()). Under that mode Vulkan performs the depth and
// stencil tests AND WRITES before the shader runs, so a discarded fragment still writes depth and
// stencil. The Xenos (and Xenia, which only hints early Z for shaders that cannot kill and draws whose
// coverage does not depend on alpha) tests and writes after the kill. analyzePixelDiscards() reports the
// mode and the kills; withoutEarlyFragmentTests() drops the mode (nothing else changes).
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace recomp::gpu::generic::spirv {

namespace alpha_detail {
enum : unsigned {
    OpExecutionMode=16,OpTypeBool=20,OpTypeInt=21,OpTypeFloat=22,OpConstant=43,OpFunction=54,OpVariable=59,OpLoad=61,
    OpAccessChain=65,OpInBoundsAccessChain=66,OpDecorate=71,OpBitcast=124,OpFNegate=127,OpFAdd=129,OpFSub=131,OpFMul=133,
    OpSelect=169,OpFOrdLessThan=184,OpSelectionMerge=247,OpLabel=248,OpBranch=249,OpBranchConditional=250,OpKill=252,
    OpTerminateInvocation=4416,OpDemoteToHelperInvocation=5380,
    ModeEarlyFragmentTests=9,DecorationBinding=33,DecorationDescriptorSet=34
};
struct Instruction {std::size_t at;unsigned op,count;};
struct Module {
    const std::vector<std::uint32_t>& w;
    std::vector<Instruction> list;
    std::unordered_map<std::uint32_t,std::size_t> def; // result id -> list index
    std::map<std::uint32_t,unsigned> set,binding;     // decorations
    bool ok=false;
    explicit Module(const std::vector<std::uint32_t>& words):w(words) {
        if(w.size()<5 || w[0]!=0x07230203u)return;
        for(std::size_t i=5;i<w.size();) {
            const unsigned count=w[i]>>16,op=w[i]&0xFFFF;
            if(!count || i+count>w.size())return;
            list.push_back({i,op,count});i+=count;
        }
        def.reserve(list.size());
        for(std::size_t k=0;k<list.size();++k) {
            const auto& x=list[k];const auto* p=w.data()+x.at;
            if(x.op==OpDecorate && x.count>=4 && p[2]==DecorationDescriptorSet)set[p[1]]=p[3];
            else if(x.op==OpDecorate && x.count>=4 && p[2]==DecorationBinding)binding[p[1]]=p[3];
            const auto r=resultOf(x);if(r)def[r]=k;
        }
        ok=true;
    }
    // The result id of the opcodes this file reads (0: none or not tracked).
    std::uint32_t resultOf(const Instruction& x) const {
        const auto* p=w.data()+x.at;
        switch(x.op) {
        case OpLabel: case OpTypeBool: case OpTypeInt: case OpTypeFloat: return x.count>=2?p[1]:0;
        case OpConstant: case OpVariable: case OpLoad: case OpAccessChain: case OpInBoundsAccessChain: case OpBitcast:
        case OpFNegate: case OpFAdd: case OpFSub: case OpFMul: case OpSelect: case OpFOrdLessThan: return x.count>=3?p[2]:0;
        default: return 0;
        }
    }
    const std::uint32_t* at(std::uint32_t id,unsigned op) const {
        const auto it=def.find(id);if(it==def.end() || list[it->second].op!=op)return nullptr;
        return w.data()+list[it->second].at;
    }
    std::optional<std::uint32_t> constantValue(std::uint32_t id) const {
        const auto it=def.find(id);if(it==def.end())return std::nullopt;
        const auto& x=list[it->second];if(x.op!=OpConstant || x.count!=4)return std::nullopt;
        return w[x.at+3];
    }
    bool isKill(unsigned op) const {return op==OpKill || op==OpTerminateInvocation || op==OpDemoteToHelperInvocation;}
};
// One translator alpha-test site: the compare (list index), its operands and the pieces to reuse.
// d is OpFSub(alpha, threshold), or OpFNegate(threshold) where the shader's oC0.w is the constant 0 (DXC
// folds 0 - t to -t; zeroAlpha).
struct AlphaSite {std::size_t compare;std::uint32_t boolType,floatType,d,zero,pointerType,uintType,shared,index0,thresholdIndex;std::size_t killAt;
    bool zeroAlpha;};
inline std::vector<AlphaSite> alphaSites(const Module& m,unsigned thresholdWord) {
    std::vector<AlphaSite> out;
    for(std::size_t k=0;k+2<m.list.size();++k) {
        const auto& x=m.list[k];if(x.op!=OpFOrdLessThan || x.count!=5)continue;
        const auto* c=m.w.data()+x.at;
        const auto zero=m.constantValue(c[4]);if(!zero || *zero!=0)continue;               // "< 0.0"
        const auto* sub=m.at(c[3],OpFSub);const auto* negate=sub?nullptr:m.at(c[3],OpFNegate);
        if(sub?(sub[0]>>16)!=5:!negate || (negate[0]>>16)!=4)continue;
        const auto* cast=m.at(sub?sub[4]:negate[3],OpBitcast);if(!cast)continue;          // alpha - threshold, or -threshold
        const auto* load=m.at(cast[3],OpLoad);if(!load)continue;
        const std::uint32_t* chain=m.at(load[3],OpAccessChain);if(!chain)chain=m.at(load[3],OpInBoundsAccessChain);
        if(!chain || (chain[0]>>16)!=6)continue;
        const auto s=m.set.find(chain[3]),b=m.binding.find(chain[3]);
        if(s==m.set.end() || b==m.binding.end() || s->second!=4 || b->second!=2)continue;  // g_SharedConstantsBuffer
        const auto i0=m.constantValue(chain[4]),i1=m.constantValue(chain[5]);
        if(!i0 || !i1 || *i0!=0 || *i1!=thresholdWord)continue;
        // OpSelectionMerge; OpBranchConditional %k %kill %merge; %kill = OpLabel; OpKill
        const auto& merge=m.list[k+1];const auto& branch=m.list[k+2];
        if(merge.op!=OpSelectionMerge || branch.op!=OpBranchConditional || branch.count<4 || m.w[branch.at+1]!=c[2])continue;
        const auto label=m.def.find(m.w[branch.at+2]);
        if(label==m.def.end() || m.list[label->second].op!=OpLabel || label->second+1>=m.list.size() ||
           !m.isKill(m.list[label->second+1].op))continue;
        out.push_back({k,c[1],sub?sub[1]:negate[1],c[3],c[4],chain[1],load[1],chain[3],chain[4],chain[5],label->second+1,!sub});
    }
    return out;
}
} // namespace alpha_detail

struct PixelDiscardInfo {
    bool valid=false;       // parsed as SPIR-V
    bool earlyTests=false;  // declares OpExecutionMode EarlyFragmentTests
    unsigned kills=0;       // OpKill / OpTerminateInvocation / OpDemoteToHelperInvocation instructions
    unsigned alphaKills=0;  // of those, the translator's alpha-test kills (alphaSites)
    bool microcodeKill()const{return kills>alphaKills;} // a kill the shader's own code reaches (clip())
};
inline PixelDiscardInfo analyzePixelDiscards(const std::vector<std::uint32_t>& words,unsigned thresholdWord=77) {
    using namespace alpha_detail;
    PixelDiscardInfo info;const Module m(words);if(!m.ok)return info;
    info.valid=true;
    for(const auto& x:m.list) {
        if(x.op==OpExecutionMode && x.count>=3 && m.w[x.at+2]==ModeEarlyFragmentTests)info.earlyTests=true;
        if(m.isKill(x.op))++info.kills;
    }
    std::set<std::size_t> alphaKills;for(const auto& s:alphaSites(m,thresholdWord))alphaKills.insert(s.killAt);
    info.alphaKills=unsigned(alphaKills.size());
    return info;
}
// The module without OpExecutionMode ... EarlyFragmentTests (depth/stencil tests and writes after the shader).
inline std::vector<std::uint32_t> withoutEarlyFragmentTests(const std::vector<std::uint32_t>& words) {
    using namespace alpha_detail;
    const Module m(words);if(!m.ok)return words;
    std::vector<std::uint32_t> out(words.begin(),words.begin()+5);out.reserve(words.size());
    for(const auto& x:m.list) {
        if(x.op==OpExecutionMode && x.count==3 && m.w[x.at+2]==ModeEarlyFragmentTests)continue;
        out.insert(out.end(),words.begin()+std::ptrdiff_t(x.at),words.begin()+std::ptrdiff_t(x.at+x.count));
    }
    return out;
}

struct AlphaPatchResult {std::optional<std::vector<std::uint32_t>> code;unsigned sites=0;const char* why="";};
// Every translator alpha-test site kills on S*d + E*|d| + C < 0, S/E/C the shared-constant words
// sWord/eWord/cWord (float bits). nullopt when the module has no such site (why = "no-alpha-test").
inline AlphaPatchResult generalizeAlphaTest(const std::vector<std::uint32_t>& in,unsigned sWord,unsigned eWord,unsigned cWord,
                                            unsigned thresholdWord=77) {
    using namespace alpha_detail;
    AlphaPatchResult r;
    const Module m(in);if(!m.ok){r.why="not-spirv";return r;}
    const auto sites=alphaSites(m,thresholdWord);
    if(sites.empty()){r.why="no-alpha-test";return r;}
    std::uint32_t bound=in[3];
    // Index constants of the threshold's type (uint or int): reuse, else declare after the threshold index.
    std::map<std::size_t,std::vector<std::uint32_t>> insertBefore; // list index -> words
    std::map<std::pair<std::uint32_t,unsigned>,std::uint32_t> constants;
    for(const auto& x:m.list)if(x.op==OpConstant && x.count==4)constants.emplace(std::pair{m.w[x.at+1],m.w[x.at+3]},m.w[x.at+2]);
    const auto indexConstant=[&](const AlphaSite& s,unsigned value)->std::uint32_t {
        const auto type=m.w[m.list[m.def.at(s.thresholdIndex)].at+1];
        const auto it=constants.find({type,value});if(it!=constants.end())return it->second;
        const std::uint32_t id=bound++;constants[{type,value}]=id;
        auto& words=insertBefore[m.def.at(s.thresholdIndex)+1];
        words.insert(words.end(),{(4u<<16)|OpConstant,type,id,value});
        return id;
    };
    std::vector<std::uint32_t> out(in);
    for(const auto& s:sites) {
        const std::uint32_t idx[3]{indexConstant(s,sWord),indexConstant(s,eWord),indexConstant(s,cWord)};
        std::uint32_t value[3];std::vector<std::uint32_t> code;
        for(unsigned k=0;k<3;++k) {
            const std::uint32_t pointer=bound++,loaded=bound++;value[k]=bound++;
            code.insert(code.end(),{(6u<<16)|OpAccessChain,s.pointerType,pointer,s.shared,s.index0,idx[k]});
            code.insert(code.end(),{(4u<<16)|OpLoad,s.uintType,loaded,pointer});
            code.insert(code.end(),{(4u<<16)|OpBitcast,s.floatType,value[k],loaded});
        }
        const std::uint32_t negated=bound++,negative=bound++,magnitude=bound++,m1=bound++,m2=bound++,sum=bound++,x=bound++;
        code.insert(code.end(),{(4u<<16)|OpFNegate,s.floatType,negated,s.d});
        code.insert(code.end(),{(5u<<16)|OpFOrdLessThan,s.boolType,negative,s.d,s.zero});
        code.insert(code.end(),{(6u<<16)|OpSelect,s.floatType,magnitude,negative,negated,s.d});
        code.insert(code.end(),{(5u<<16)|OpFMul,s.floatType,m1,value[0],s.d});
        code.insert(code.end(),{(5u<<16)|OpFMul,s.floatType,m2,value[1],magnitude});
        code.insert(code.end(),{(5u<<16)|OpFAdd,s.floatType,sum,m1,m2});
        code.insert(code.end(),{(5u<<16)|OpFAdd,s.floatType,x,sum,value[2]});
        auto& words=insertBefore[s.compare];words.insert(words.end(),code.begin(),code.end());
        out[m.list[s.compare].at+3]=x; // the kill compare now reads x
    }
    std::vector<std::uint32_t> result(out.begin(),out.begin()+5);result[3]=bound;result.reserve(out.size()+insertBefore.size()*64);
    for(std::size_t k=0;k<m.list.size();++k) {
        if(const auto it=insertBefore.find(k);it!=insertBefore.end())result.insert(result.end(),it->second.begin(),it->second.end());
        result.insert(result.end(),out.begin()+std::ptrdiff_t(m.list[k].at),out.begin()+std::ptrdiff_t(m.list[k].at+m.list[k].count));
    }
    r.code=std::move(result);r.sites=unsigned(sites.size());r.why="patched";
    return r;
}

} // namespace recomp::gpu::generic::spirv
