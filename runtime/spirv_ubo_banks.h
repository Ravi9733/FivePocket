#pragma once
// v294: the constant banks as uniform blocks. The shader pack's modules read the vertex / pixel constant banks (descriptor
// set 4, bindings 0 and 1) from Uniform-class BufferBlock structs (storage buffers, SPIR-V 1.0 form) whose only member is
// a runtime array of 32-bit words with ArrayStride 4: on Adreno a storage-buffer load goes through the load path, while a
// uniform block read at a constant index can be served from the constant file. This rewrites each such bank variable to a
// new Block struct (a uniform block) holding a SIZED uint[1024] with ArrayStride 4 and member offset 0: every word keeps
// its byte offset 4*k, so the renderer's ring contents and dynamic offsets are unchanged and every read returns the same
// bits. A stride-4 word array in a uniform block needs the device's scalarBlockLayout feature. Other variables (binding 2,
// the shared block, keeps its runtime array: a uniform block cannot hold one) and the original types stay as they are.
// Refused (nullopt code, `why` set): anything but that exact shape, or an OpArrayLength of a bank.
#include <cstdint>
#include <cstddef>
#include <vector>

namespace recomp::gpu::generic::spirv_ubo {

struct Result {std::vector<std::uint32_t> code;unsigned banks=0;const char* why=nullptr;};

inline Result uboBanks(const std::uint32_t* w,std::size_t n) {
    Result r;
    if(n<5 || w[0]!=0x07230203u){r.why="header";return r;}
    constexpr unsigned OpTypeInt=21,OpTypeRuntimeArray=29,OpTypeStruct=30,OpTypePointer=32,OpVariable=59,OpArrayLength=68,
        OpDecorate=71,OpMemberDecorate=72,OpConstant=43,OpTypeArray=28;
    constexpr unsigned DecBlock=2,DecBufferBlock=3,DecArrayStride=6,DecBinding=33,DecSet=34,DecOffset=35;
    constexpr unsigned ClassUniform=2;
    const std::uint32_t bound=w[3];
    // ---- pass 1: the declarations this rewrite reads
    std::vector<std::int64_t> set(bound,-1),binding(bound,-1),stride(bound,-1),memberOffset0(bound,-1);
    std::vector<unsigned char> bufferBlock(bound,0);
    struct Ptr {std::uint32_t cls=0,type=0;};std::vector<Ptr> ptr(bound);
    std::vector<std::uint32_t> structMember(bound,0),structCount(bound,0),runtimeElem(bound,0),intWidth(bound,0);
    struct Var {std::size_t at;std::uint32_t id,type;};std::vector<Var> vars;
    std::vector<std::uint32_t> lengthBases;
    std::size_t firstType=0,i=5;
    const auto isTypeOrConst=[](unsigned op){return (op>=19 && op<=39) || (op>=41 && op<=52) || op==322 || op==59;};
    while(i<n) {
        const unsigned op=w[i]&0xFFFFu,len=w[i]>>16;
        if(!len || i+len>n){r.why="truncated";return r;}
        const std::uint32_t* a=w+i+1;
        const auto id=[&](unsigned k)->std::uint32_t{return k+1<len && a[k]<bound?a[k]:0;};
        if(!firstType && isTypeOrConst(op))firstType=i;
        switch(op) {
        case OpDecorate:
            if(len>=3 && id(0)) {
                const auto t=a[0];
                if(a[1]==DecSet && len>=4)set[t]=a[2];
                else if(a[1]==DecBinding && len>=4)binding[t]=a[2];
                else if(a[1]==DecArrayStride && len>=4)stride[t]=a[2];
                else if(a[1]==DecBufferBlock)bufferBlock[t]=1;
            }
            break;
        case OpMemberDecorate:
            if(len>=5 && id(0) && a[1]==0 && a[2]==DecOffset)memberOffset0[a[0]]=a[3];
            break;
        case OpTypeInt: if(len>=4 && id(0))intWidth[a[0]]=a[1]; break;
        case OpTypeRuntimeArray: if(len>=3 && id(0))runtimeElem[a[0]]=a[1]; break;
        case OpTypeStruct: if(len>=2 && id(0)){structCount[a[0]]=len-2;structMember[a[0]]=len>=3?a[1]:0;} break;
        case OpTypePointer: if(len>=4 && id(0))ptr[a[0]]={a[1],a[2]}; break;
        case OpVariable: if(len>=4 && a[1]<bound)vars.push_back({i,a[1],a[0]}); break;
        case OpArrayLength: if(len>=5)lengthBases.push_back(a[2]); break;
        default: break;
        }
        i+=len;
    }
    if(!firstType){r.why="no-types";return r;}
    // ---- the bank variables and their one shared shape
    std::vector<std::size_t> bankAt;std::uint32_t elem=0,oldStruct=0;
    for(const auto& v:vars) {
        if(set[v.id]!=4 || (binding[v.id]!=0 && binding[v.id]!=1))continue;
        const auto& p=ptr[v.type];const auto s=p.type;
        if(p.cls!=ClassUniform || !s || s>=bound || !bufferBlock[s] || structCount[s]!=1 || memberOffset0[s]!=0){r.why="bank-struct";return r;}
        const auto ra=structMember[s];
        if(!ra || ra>=bound || !runtimeElem[ra] || stride[ra]!=4){r.why="bank-array";return r;}
        const auto e=runtimeElem[ra];
        if(e>=bound || intWidth[e]!=32){r.why="bank-element";return r;}
        if((elem && elem!=e) || (oldStruct && oldStruct!=s)){r.why="bank-shapes-differ";return r;}
        elem=e;oldStruct=s;bankAt.push_back(v.at);
        for(const auto b:lengthBases)if(b==v.id){r.why="bank-array-length";return r;}
    }
    if(bankAt.empty()){r.why="no-bank";return r;}
    // ---- pass 2: copy with the new ids (the length constant, the sized array, the Block struct, its Uniform pointer)
    const std::uint32_t cLen=bound,arr=bound+1,blk=bound+2,ptrBlk=bound+3;
    r.code.reserve(n+32);
    r.code.insert(r.code.end(),w,w+5);r.code[3]=bound+4;
    const auto emit=[&](unsigned op,std::initializer_list<std::uint32_t> ops){r.code.push_back(std::uint32_t(ops.size()+1)<<16|op);
        r.code.insert(r.code.end(),ops.begin(),ops.end());};
    i=5;
    while(i<n) {
        const unsigned op=w[i]&0xFFFFu,len=w[i]>>16;
        if(i==firstType) { // the annotations end here: the new types' decorations
            emit(OpDecorate,{arr,DecArrayStride,4u});
            emit(OpDecorate,{blk,DecBlock});
            emit(OpMemberDecorate,{blk,0u,DecOffset,0u});
        }
        const std::size_t out=r.code.size();
        r.code.insert(r.code.end(),w+i,w+i+len);
#if !defined(RECOMP_V294_MUTANT_NO_RETYPE) // test mutant: the banks keep their storage-buffer type
        if(op==OpVariable)for(const auto at:bankAt)if(at==i)r.code[out+1]=ptrBlk; // (result type)
#endif
        if(op==OpTypeInt && w[i+1]==elem) { // right after the element type: everything the banks' new type needs
            emit(OpConstant,{elem,cLen,1024u});
            emit(OpTypeArray,{arr,elem,cLen});
#if !defined(RECOMP_V294_MUTANT_RUNTIME)
            emit(OpTypeStruct,{blk,arr});
            emit(OpTypePointer,{ptrBlk,ClassUniform,blk});
#endif
        }
#if defined(RECOMP_V294_MUTANT_RUNTIME) // test mutant: the Block holds the banks' runtime array (invalid in a uniform block)
        if(op==OpTypeRuntimeArray && w[i+1]==structMember[oldStruct]) {
            emit(OpTypeStruct,{blk,structMember[oldStruct]});
            emit(OpTypePointer,{ptrBlk,ClassUniform,blk});
        }
#endif
        i+=len;
    }
    r.banks=unsigned(bankAt.size());
    return r;
}

} // namespace recomp::gpu::generic::spirv_ubo
