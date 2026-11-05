#pragma once
// Attempt v277: which 16-word (64-byte) blocks of the two float-constant banks a pack module can read, so the renderer copies
// only those blocks of a bank into the draw's ring slot (v275 outdoor worker profile: the two whole-bank 4 KB copies were
// 4.4% of the renderer thread; the pack's modules with constant indices read a median of 3 (vertex) / 7 (pixel) of the 64
// blocks, local-analysis offline scan 2026-09-29).
//
// The banks are the pack ABI's set 4, binding 0 (vertex) and binding 1 (pixel): a Uniform (BufferBlock) or StorageBuffer
// variable whose type is a struct with member 0 at offset 0 an array (runtime or sized) of 32-bit scalars with ArrayStride 4,
// i.e. one bank word per element. A read is an OpLoad through an access chain [member 0, element k] from the variable (or
// the same path split over two chains); k must be an OpConstant of a 32-bit integer type below 1024, and the block k/16 is
// marked. Everything else that touches the variable or a pointer derived from it (a non-constant or out-of-range element
// index, another member, OpPtrAccessChain, a load of the whole struct or array, a store, a copy, a function argument, any
// other opcode naming the id) marks the whole bank: the mask is conservative, never smaller than what the module reads.
// A module this scan cannot parse, or a bank variable of any other layout, gives the whole bank too.
//
// The other module transforms the renderer applies (spirv_input_adapt.h, spirv_screen_space.h: inputs / outputs only;
// spirv_alpha_test.h: reads set 4 binding 2, the shared block, which is always copied whole) add no reads of either bank,
// so a pack entry's masks hold for all its variants.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace recomp::gpu::generic::spirv {

struct ConstantBlockMasks {
    std::array<std::uint64_t,2> bank{~std::uint64_t{0},~std::uint64_t{0}}; // [0] vertex (binding 0), [1] pixel (binding 1)
    bool parsed=false;const char* why="unparsed";
};

inline ConstantBlockMasks constantBlockMasks(const std::uint32_t* w,std::size_t n) {
    enum : unsigned {OpSourceContinued=2,OpSource=3,OpSourceExtension=4,OpName=5,OpMemberName=6,OpString=7,OpLine=8,
        OpExtension=10,OpExtInstImport=11,OpExtInst=12,OpMemoryModel=14,OpEntryPoint=15,OpExecutionMode=16,OpCapability=17,
        OpTypeInt=21,OpTypeFloat=22,OpTypeArray=28,OpTypeRuntimeArray=29,OpTypeStruct=30,OpTypePointer=32,OpConstant=43,
        OpVariable=59,OpLoad=61,OpAccessChain=65,OpInBoundsAccessChain=66,OpPtrAccessChain=67,OpDecorate=71,OpMemberDecorate=72,
        OpVectorShuffle=79,OpCompositeExtract=81,OpCompositeInsert=82,OpBranchConditional=250,OpSwitch=251,OpNoLine=317,OpModuleProcessed=330,OpDecorateId=332,OpDecorateString=5632,OpMemberDecorateString=5633};
    enum : unsigned {DecorationArrayStride=6,DecorationBinding=33,DecorationDescriptorSet=34,DecorationOffset=35};
    enum : unsigned {StorageUniform=2,StorageStorageBuffer=12};
    ConstantBlockMasks out;
    if(!w || n<5 || w[0]!=0x07230203u){out.why="header";return out;}
    std::unordered_map<std::uint32_t,unsigned> set,binding,stride,scalarWidth,memberOffset0;
    std::unordered_map<std::uint32_t,std::uint32_t> pointee,arrayElement,constant;
    std::unordered_map<std::uint32_t,std::vector<std::uint32_t>> members;
    std::vector<std::pair<std::uint32_t,std::uint32_t>> variables; // (result id, pointer type id)
    std::unordered_map<std::uint32_t,std::uint32_t> variableClass;
    // pass 1: decorations, types, constants, variables
    for(std::size_t i=5;i<n;) {
        const unsigned count=w[i]>>16,op=w[i]&0xFFFFu;
        if(!count || i+count>n){out.why="word-count";return out;}
        const std::uint32_t* a=w+i+1;const unsigned k=count-1;
        if(op==OpDecorate && k>=3) {
            if(a[1]==DecorationDescriptorSet)set[a[0]]=a[2];else if(a[1]==DecorationBinding)binding[a[0]]=a[2];
            else if(a[1]==DecorationArrayStride)stride[a[0]]=a[2];
        } else if(op==OpMemberDecorate && k>=4 && a[1]==0 && a[2]==DecorationOffset)memberOffset0[a[0]]=a[3];
        else if(op==OpTypeInt && k>=2)scalarWidth[a[0]]=a[1];
        else if(op==OpTypeFloat && k>=2)scalarWidth[a[0]]=a[1];
        else if((op==OpTypeRuntimeArray && k>=2) || (op==OpTypeArray && k>=3))arrayElement[a[0]]=a[1];
        else if(op==OpTypeStruct && k>=1)members[a[0]].assign(a+1,a+k);
        else if(op==OpTypePointer && k>=3)pointee[a[0]]=a[2];
        else if(op==OpConstant && k==3) {if(const auto t=scalarWidth.find(a[0]);t!=scalarWidth.end() && t->second==32)constant[a[1]]=a[2];}
        else if(op==OpVariable && k>=3){variables.push_back({a[1],a[0]});variableClass[a[1]]=a[2];}
        i+=count;
    }
    // bank variables: bank index, and whether their layout is the one word per element this scan understands
    struct Derived {unsigned bank;unsigned level;}; // level 0: the variable (struct), 1: the word array, 2: one word
    std::unordered_map<std::uint32_t,Derived> derived;
    std::array<std::uint64_t,2> mask{0,0};
    for(const auto& [id,type]:variables) {
        const auto s=set.find(id),b=binding.find(id);
        if(s==set.end() || b==binding.end() || s->second!=4 || b->second>1)continue;
        const unsigned bank=b->second;
        const auto sc=variableClass[id];
        bool ok=sc==StorageUniform || sc==StorageStorageBuffer;
        const auto p=pointee.find(type);ok=ok && p!=pointee.end();
        const auto m=ok?members.find(p->second):members.end();ok=ok && m!=members.end() && m->second.size()==1;
        if(ok){const auto o=memberOffset0.find(p->second);ok=o!=memberOffset0.end() && o->second==0;}
        if(ok) {
            const auto arr=m->second[0];const auto e=arrayElement.find(arr);const auto st=stride.find(arr);
            ok=e!=arrayElement.end() && st!=stride.end() && st->second==4;
            if(ok){const auto sw=scalarWidth.find(e->second);ok=sw!=scalarWidth.end() && sw->second==32;}
        }
        if(!ok){mask[bank]=~std::uint64_t{0};continue;}
        derived[id]={bank,0};
    }
    // pass 2: the uses of the bank variables and of every pointer derived from them
    const auto full=[&](unsigned bank){mask[bank]=~std::uint64_t{0};};
    for(std::size_t i=5;i<n;) {
        const unsigned count=w[i]>>16,op=w[i]&0xFFFFu;
        const std::uint32_t* a=w+i+1;const unsigned k=count-1;
        i+=count;
        if(derived.empty())break;
        switch(op) { // (these read no bank word: names, decorations, literals, debug info, the entry point's interface list)
        case OpExtension:case OpExtInstImport:case OpMemoryModel:case OpEntryPoint:case OpExecutionMode:case OpCapability:
        case OpSourceContinued:case OpSource:case OpSourceExtension:case OpName:case OpMemberName:case OpString:case OpLine:
        case OpNoLine:case OpModuleProcessed:case OpDecorate:case OpMemberDecorate:case OpDecorateId:case OpDecorateString:
        case OpMemberDecorateString:case OpConstant:case OpTypeInt:case OpTypeFloat:case OpTypeArray:case OpTypeRuntimeArray:
        case OpTypeStruct:case OpTypePointer:
            continue;
        case OpVariable:
            if(k>=4)if(const auto d=derived.find(a[3]);d!=derived.end())full(d->second.bank); // (an initializer naming a bank pointer)
            continue;
        default:break;
        }
        if((op==OpAccessChain || op==OpInBoundsAccessChain) && k>=3) {
            const auto base=derived.find(a[2]);
            if(base!=derived.end()) {
                const unsigned bank=base->second.bank;unsigned level=base->second.level;bool bad=false;
                for(unsigned j=3;j<k && !bad;++j) {
                    const auto c=constant.find(a[j]);
                    if(level==0){bad=c==constant.end() || c->second!=0;level=1;}
                    else if(level==1) {
#if defined(RECOMP_V277_MUTANT_DYNAMIC_IGNORED) // test mutant: a non-constant element index marks nothing
                        if(c==constant.end()){level=2;continue;}
#endif
                        if(c==constant.end() || c->second>=1024)bad=true;
                        else mask[bank]|=std::uint64_t{1}<<(c->second/16);
                        level=2;
                    } else bad=true;
                }
                // (the other chain operands, the indices, are never bank pointers: checked below with the rest)
                if(bad)full(bank);else derived[a[1]]={bank,level};
                for(unsigned j=3;j<k;++j)if(const auto d=derived.find(a[j]);d!=derived.end())full(d->second.bank);
                continue;
            }
        }
        if(op==OpLoad && k>=3) {
            if(const auto d=derived.find(a[2]);d!=derived.end() && d->second.level!=2)full(d->second.bank); // a whole struct / array
            continue; // (operands after the pointer: memory-operand literals)
        }
        // any other instruction naming a bank pointer anywhere in its operands (OpPtrAccessChain, OpStore, OpCopyObject,
        // OpCopyMemory, OpFunctionCall, OpPhi, OpSelect, OpArrayLength, ...): the whole bank (a literal operand that happens
        // to equal such an id also counts: conservative)
        // (literal operands skipped: OpExtInst's operand 3 is the extended instruction's number; OpSwitch's case values,
        // OpBranchConditional's weights and the component / index literals of OpVectorShuffle, OpCompositeExtract and
        // OpCompositeInsert follow their ids)
        const unsigned idOperands=op==OpSwitch?1u:op==OpBranchConditional || op==OpCompositeExtract?3u:
                                  op==OpVectorShuffle || op==OpCompositeInsert?4u:k;
        const unsigned from=op==OpExtInst?4u:0u,to=std::min(k,idOperands);
        for(unsigned j=from;j<to;++j)if(const auto d=derived.find(a[j]);d!=derived.end())full(d->second.bank);
    }
    out.bank=mask;out.parsed=true;out.why="";
    return out;
}

inline ConstantBlockMasks constantBlockMasks(const std::vector<std::uint32_t>& words){return constantBlockMasks(words.data(),words.size());}

} // namespace recomp::gpu::generic::spirv
