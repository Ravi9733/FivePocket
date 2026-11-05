#pragma once
// Attempt v68: GTA V's skinned vertex shaders read BLENDINDICES as a NORMALIZED k_8_8_8_8 fetch and scale
// it back to bone rows themselves (ped.fxc: index = fetched * 255.002 * 3, then gBoneMtx(index) with
// gBoneMtx(i) = 0 for i >= 192). XenosRecomp declares the input `uint4 iBlendIndices0` (location 18) and
// converts it to float (OpConvertUToF), so it cannot receive a fraction: the renderer fed the raw bytes
// (R8G8B8A8_UINT), the index came out 255 times too large, every bone past 0 read zero matrices and every
// skinned vertex collapsed (v66: 83 ped draws per frame recorded, no character on screen).
//
// This rewrites a module's location-18 input to float: the input variable's pointer type becomes
// Input float (vector or, through an access chain, scalar), every load of it yields float, and every
// OpConvertUToF / OpConvertSToF of such a value becomes OpCopyObject (the same length); composite
// extracts and vector shuffles of it change their result type to the float type of the same shape.
// Any other use leaves the module unchanged (nullopt, with the reason). The renderer then feeds the
// attribute through the fetch's own float format (UNORM/SNORM/USCALED/SSCALED), which is what the Xenos
// vertex fetch hands the shader for either a normalized or an integer fetch.
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace recomp::gpu::generic::spirv {

struct BlendFloatResult {std::optional<std::vector<std::uint32_t>> code;const char* why="";};

inline BlendFloatResult blendIndicesAsFloat(const std::vector<std::uint32_t>& in,unsigned location=18) {
    enum : unsigned {OpExtInst=12,OpEntryPoint=15,OpSwitch=251,OpTypeInt=21,OpTypeFloat=22,OpTypeVector=23,OpTypePointer=32,OpFunction=54,OpVariable=59,OpLoad=61,
        OpAccessChain=65,OpInBoundsAccessChain=66,OpDecorate=71,OpVectorShuffle=79,OpCompositeExtract=81,OpCopyObject=83,
        OpConvertSToF=111,OpConvertUToF=112,DecorationLocation=30,StorageInput=1};
    BlendFloatResult r;
    if(in.size()<5 || in[0]!=0x07230203u){r.why="not-spirv";return r;}
    struct Instruction {std::size_t at;unsigned op,count;};
    std::vector<Instruction> list;
    for(std::size_t i=5;i<in.size();) {
        const unsigned count=in[i]>>16,op=in[i]&0xFFFF;
        if(!count || i+count>in.size()){r.why="malformed";return r;}
        list.push_back({i,op,count});i+=count;
    }
    std::map<std::uint32_t,unsigned> locationOf;
    std::map<std::uint32_t,std::pair<std::uint32_t,unsigned>> vectorOf;   // id -> component type, count
    std::map<std::uint32_t,std::pair<unsigned,unsigned>> intOf;           // id -> width, signedness
    std::map<std::uint32_t,unsigned> floatOf;                             // id -> width
    std::map<std::uint32_t,std::pair<unsigned,std::uint32_t>> pointerOf;  // id -> storage class, pointee
    std::map<std::uint32_t,std::size_t> variableAt;                       // Input variable id -> list index
    std::size_t firstFunction=list.size();
    for(std::size_t k=0;k<list.size();++k) {
        const auto& x=list[k];const auto* w=in.data()+x.at;
        if(x.op==OpDecorate && x.count>=4 && w[2]==DecorationLocation)locationOf[w[1]]=w[3];
        else if(x.op==OpTypeVector && x.count==4)vectorOf[w[1]]={w[2],w[3]};
        else if(x.op==OpTypeInt && x.count==4)intOf[w[1]]={w[2],w[3]};
        else if(x.op==OpTypeFloat && x.count>=3)floatOf[w[1]]=w[2];
        else if(x.op==OpTypePointer && x.count==4)pointerOf[w[1]]={w[2],w[3]};
        else if(x.op==OpVariable && x.count>=4 && w[3]==StorageInput)variableAt[w[2]]=k;
        else if(x.op==OpFunction && firstFunction==list.size())firstFunction=k;
    }
    // The location's Input variable, a pointer to an int32 vector.
    std::uint32_t variable=0;
    for(const auto& [id,k]:variableAt)if(locationOf.count(id) && locationOf[id]==location)variable=id;
    if(!variable){r.why="no-input-at-location";return r;}
    const auto* varWords=in.data()+list[variableAt[variable]].at;
    const auto pointer=pointerOf.find(varWords[1]);
    if(pointer==pointerOf.end() || pointer->second.first!=StorageInput){r.why="input-pointer";return r;}
    const auto vec=vectorOf.find(pointer->second.second);
    if(vec==vectorOf.end() || !intOf.count(vec->second.first) || intOf[vec->second.first].first!=32){r.why="not-int-vector";return r;}
    const unsigned width=vec->second.second;
    std::uint32_t float32=0;for(const auto& [id,w]:floatOf)if(w==32){float32=id;break;}
    if(!float32){r.why="no-float32";return r;}
    std::uint32_t bound=in[3];
    std::vector<std::pair<std::size_t,std::vector<std::uint32_t>>> inserts; // before list index, words
    const auto floatVector=[&](unsigned n,bool create)->std::uint32_t {
        if(n==1)return float32;
        for(const auto& [id,v]:vectorOf)if(v.first==float32 && v.second==n)return id;
        if(!create)return 0;
        const std::uint32_t id=bound++;vectorOf[id]={float32,n};
        inserts.push_back({variableAt[variable],{(4u<<16)|OpTypeVector,id,float32,n}});return id;
    };
    const auto inputPointer=[&](std::uint32_t pointee)->std::uint32_t {
        for(const auto& [id,p]:pointerOf)if(p.first==StorageInput && p.second==pointee)return id;
        const std::uint32_t id=bound++;pointerOf[id]={StorageInput,pointee};
        inserts.push_back({variableAt[variable],{(4u<<16)|OpTypePointer,id,StorageInput,pointee}});return id;
    };
    std::vector<std::uint32_t> out(in);
    const std::uint32_t floatVec=floatVector(width,true);
    out[list[variableAt[variable]].at+1]=inputPointer(floatVec);
    // Values that now carry floats instead of ints: id -> component count (1 for a scalar).
    std::map<std::uint32_t,unsigned> changed;
    std::set<std::uint32_t> pointers{variable}; // the variable and access chains into it
    bool progress=true;
    while(progress) {
        progress=false;
        for(std::size_t k=firstFunction;k<list.size();++k) {
            const auto& x=list[k];auto* w=out.data()+x.at;
            // Any other instruction reading a changed value (its id among the operands) stops the rewrite.
            // Literal operands can equal an id: the component/index literals of shuffles and extracts, the
            // extended instruction number and the case literals of a switch are not compared; the input
            // variable and its access chains are only followed through loads (above).
            const auto uses=[&](unsigned from) {
                for(unsigned j=from;j<x.count;++j) {
                    if((x.op==OpVectorShuffle && j>=5) || (x.op==OpCompositeExtract && j>=4) || (x.op==OpExtInst && j==4) ||
                       (x.op==OpSwitch && j>=3 && (j-3)%2==0))continue;
                    if(changed.count(w[j]))return true;
                }
                return false;
            };
            if((x.op==OpAccessChain || x.op==OpInBoundsAccessChain) && x.count>=5 && pointers.count(w[3])) {
                if(!pointers.count(w[2])){pointers.insert(w[2]);w[1]=inputPointer(float32);progress=true;}
                continue;
            }
            if(x.op==OpLoad && x.count>=4 && pointers.count(w[3])) {
                if(!changed.count(w[2])){const unsigned n=w[3]==variable?width:1u;changed[w[2]]=n;w[1]=floatVector(n,true);progress=true;}
                continue;
            }
            if((x.op==OpConvertUToF || x.op==OpConvertSToF) && x.count==4 && changed.count(w[3])) {
                w[0]=(4u<<16)|OpCopyObject;continue; // result type is already the float of that shape
            }
            if((x.op==OpConvertUToF || x.op==OpConvertSToF) && x.count==4)continue; // (not a use of a changed value)
            // An original copy of a changed value carries floats too (a rewritten conversion does not count:
            // its result was float already).
            if(x.op==OpCopyObject && x.count==4 && changed.count(w[3])) {
                if((in[x.at]&0xFFFF)!=OpCopyObject)continue;
                if(!changed.count(w[2])){changed[w[2]]=changed[w[3]];w[1]=floatVector(changed[w[3]],true);progress=true;}
                continue;
            }
            if(x.op==OpCompositeExtract && x.count>=5 && changed.count(w[3])) {
                if(x.count!=5){r.why="extract-depth";return r;}
                if(!changed.count(w[2])){changed[w[2]]=1;w[1]=float32;progress=true;}
                continue;
            }
            if(x.op==OpVectorShuffle && x.count>=5 && (changed.count(w[3]) || changed.count(w[4]))) {
                if(!(changed.count(w[3]) && changed.count(w[4]))){r.why="shuffle-mixed";return r;}
                if(!changed.count(w[2])){const unsigned n=x.count-5;changed[w[2]]=n;w[1]=floatVector(n,true);progress=true;}
                continue;
            }
            if(x.op==OpEntryPoint || x.op==OpDecorate)continue;
            if(uses(1)){static thread_local char why[48];std::snprintf(why,sizeof why,"use-op-%u",x.op);r.why=why;return r;}
        }
    }
    if(changed.empty()){r.why="never-loaded";return r;}
    // New type/pointer instructions go before the variable (after the types they use), in creation order.
    std::vector<std::uint32_t> result(out.begin(),out.begin()+5);result[3]=bound;
    for(std::size_t k=0;k<list.size();++k) {
        for(const auto& [before,words]:inserts)if(before==k)result.insert(result.end(),words.begin(),words.end());
        result.insert(result.end(),out.begin()+list[k].at,out.begin()+list[k].at+list[k].count);
    }
    r.code=std::move(result);r.why="patched";
    return r;
}

} // namespace recomp::gpu::generic::spirv
