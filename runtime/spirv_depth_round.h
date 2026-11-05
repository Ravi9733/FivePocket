#pragma once
// Package depth-float (diag-df-round): pixel shader SPIR-V edits that make the depth a draw writes into a k_24_8_FLOAT target (a
// D32_SFLOAT_S8_UINT attachment under diag-df-float) a 20e4 value, as the 360's D24FS8 store keeps it, so that every depth test
// against the buffer compares 20e4 values (every 20e4 value is a float32, and float32 order is the code order: xenos_depth_df.h).
// The fragment's depth z (the shader's own FragDepth store, or FragCoord.z) is replaced by
//     q(z) = z > 0 ? (bits(z) >= 0x38800000 ? RNE of the mantissa to 20 bits  : round-even(z * 2^34) * 2^-34) : 0      (NaN -> 0)
// computed with integer instructions only (no float rounding mode or fast-math rewrite can change it): the same function as
// xenos_depth_df.h quantizeXenosFloat20e4 on every float32 (values that reach 2 give the largest 20e4 value, 1.999998; the
// attachment then clamps depth > 1 to 1 as it does today; 0 and 1 are 20e4 values). quantizeSequence() emits it; the host tests run
// the emitted words in the SPIR-V interpreter (tests/spirv_interp_df.cpp) against the C++ function.
//  - roundFragmentDepth(): a module that stores FragDepth (the 105 depth-exporting pixel shaders of the pack): every OpStore to the
//    FragDepth variable stores q(value). A module without one (every other): a FragDepth output (BuiltIn FragDepth, added to the
//    entry point's interface) storing q(FragCoord.z) before every OpReturn of the entry function, and a FragCoord input when the
//    module has none. Both: EarlyFragmentTests dropped (with it Vulkan would test and write the rasterizer's depth before the shader
//    runs, and ignore FragDepth), DepthGreater / DepthLess / DepthUnchanged dropped (rounding moves either way), DepthReplacing
//    declared. Nothing else changes. Refused (nullopt + why): not SPIR-V, not one Fragment entry point, FragDepth written through
//    an access chain, a missing float type.
//  - depthRoundNullModule(): the fragment stage of a draw with no pixel shader (depth only, colour writes masked): entry
//    "shaderMain", OriginUpperLeft, DepthReplacing, FragDepth = q(FragCoord.z).
//  - roundCompositeForTest(): host test only: the vec4 OpCompositeConstruct of four equal floats gets q of that float.
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace recomp::gpu::generic::spirv {

namespace dr_detail {
enum : unsigned {
    OpEntryPoint=15,OpExecutionMode=16,OpCapability=17,OpTypeVoid=19,OpTypeBool=20,OpTypeInt=21,OpTypeFloat=22,OpTypeVector=23,
    OpTypePointer=32,OpTypeFunction=33,OpConstant=43,OpFunction=54,OpFunctionEnd=56,OpVariable=59,OpLoad=61,OpStore=62,
    OpAccessChain=65,OpInBoundsAccessChain=66,OpPtrAccessChain=67,OpDecorate=71,OpMemberDecorate=72,OpDecorationGroup=73,
    OpGroupDecorate=74,OpGroupMemberDecorate=75,OpCompositeConstruct=80,OpCompositeExtract=81,OpConvertUToF=112,OpBitcast=124,
    OpIAdd=128,OpISub=130,OpFMul=133,OpLogicalOr=166,OpLogicalAnd=167,OpSelect=169,OpIEqual=170,OpINotEqual=171,
    OpUGreaterThan=172,OpUGreaterThanEqual=174,OpULessThan=176,OpFOrdGreaterThan=186,OpShiftRightLogical=194,
    OpShiftLeftLogical=196,OpBitwiseOr=197,OpBitwiseAnd=199,OpLabel=248,OpReturn=253,OpDecorateId=332,OpDecorateString=5632,
    OpMemberDecorateString=5633,
    ModeOriginUpperLeft=7,ModeEarlyFragmentTests=9,ModeDepthReplacing=12,ModeDepthGreater=14,ModeDepthLess=15,ModeDepthUnchanged=16,
    DecorationBuiltIn=11,BuiltInFragCoord=15,BuiltInFragDepth=22,StorageInput=1,StorageOutput=3,ModelFragment=4
};
struct Ins {std::size_t at;unsigned op,count;};
inline bool isAnnotation(unsigned op){return op==OpDecorate || op==OpMemberDecorate || op==OpDecorationGroup || op==OpGroupDecorate ||
    op==OpGroupMemberDecorate || op==OpDecorateId || op==OpDecorateString || op==OpMemberDecorateString;}
inline std::uint32_t fbits(float f){std::uint32_t u;std::memcpy(&u,&f,4);return u;}

// Types / constants / variables a rewrite needs: found in the module or declared (words appended to `globals`, placed before the
// first function: types first in creation order, so every declaration follows the ones it names).
struct Globals {
    std::uint32_t& bound;std::vector<std::uint32_t> globals;
    std::uint32_t floatT=0,uintT=0,boolT=0,vec4T=0,inVec4P=0,outFloatP=0;
    std::map<std::pair<std::uint32_t,std::uint32_t>,std::uint32_t> constants; // (type, value bits) -> id
    explicit Globals(std::uint32_t& b):bound(b){}
    void scan(const std::vector<std::uint32_t>& w,const std::vector<Ins>& list) {
        for(const auto& x:list) {
            const auto* p=&w[x.at];
            if(x.op==OpTypeFloat && x.count==3 && p[2]==32 && !floatT)floatT=p[1];
            else if(x.op==OpTypeInt && x.count==4 && p[2]==32 && p[3]==0 && !uintT)uintT=p[1];
            else if(x.op==OpTypeBool && x.count==2 && !boolT)boolT=p[1];
        }
        for(const auto& x:list) {
            const auto* p=&w[x.at];
            if(x.op==OpTypeVector && x.count==4 && floatT && p[2]==floatT && p[3]==4 && !vec4T)vec4T=p[1];
            else if(x.op==OpConstant && x.count==4)constants.emplace(std::pair{p[1],p[3]},p[2]);
        }
        for(const auto& x:list) {
            const auto* p=&w[x.at];
            if(x.op==OpTypePointer && x.count==4 && p[2]==StorageInput && vec4T && p[3]==vec4T && !inVec4P)inVec4P=p[1];
            else if(x.op==OpTypePointer && x.count==4 && p[2]==StorageOutput && floatT && p[3]==floatT && !outFloatP)outFloatP=p[1];
        }
    }
    std::uint32_t type(std::uint32_t& id,std::initializer_list<std::uint32_t> words) { // words: opcode then operands after the result id
        if(id)return id;
        id=bound++;
        globals.push_back((std::uint32_t(words.size()+1)<<16)|*words.begin());
        globals.push_back(id);
        globals.insert(globals.end(),words.begin()+1,words.end());
        return id;
    }
    std::uint32_t uintType(){return type(uintT,{OpTypeInt,32,0});}
    std::uint32_t floatType(){return type(floatT,{OpTypeFloat,32});}
    std::uint32_t boolType(){return type(boolT,{OpTypeBool});}
    std::uint32_t vec4Type(){const auto f=floatType();return type(vec4T,{OpTypeVector,f,4});}
    std::uint32_t inVec4Ptr(){const auto v=vec4Type();return type(inVec4P,{OpTypePointer,StorageInput,v});}
    std::uint32_t outFloatPtr(){const auto f=floatType();return type(outFloatP,{OpTypePointer,StorageOutput,f});}
    std::uint32_t constant(std::uint32_t t,std::uint32_t value) {
        const auto it=constants.find({t,value});if(it!=constants.end())return it->second;
        const std::uint32_t id=bound++;constants[{t,value}]=id;
        globals.insert(globals.end(),{(4u<<16)|OpConstant,t,id,value});
        return id;
    }
    std::uint32_t variable(std::uint32_t pointerType,std::uint32_t storage) {
        const std::uint32_t id=bound++;globals.insert(globals.end(),{(4u<<16)|OpVariable,pointerType,id,storage});return id;
    }
};
// Declares every type and constant quantizeSequence() names (call before it, so the globals are in place).
inline void quantizeGlobals(Globals& g) {
    const auto u=g.uintType(),f=g.floatType();g.boolType();
    for(const std::uint32_t v:{0u,1u,3u,23u,24u,92u,116u,0xFFFFFFF8u,0x3FFFFFF8u,0x007FFFFFu,0x00800000u,0x38800000u})g.constant(u,v);
    g.constant(f,fbits(0.0f));g.constant(f,0x2E800000u); // 0 and 2^-34
}
// Appends q(z) to `code` (z: a float32 id); returns q's id. Every id it names is declared by quantizeGlobals(g).
inline std::uint32_t quantizeSequence(Globals& g,std::vector<std::uint32_t>& code,std::uint32_t z) {
    const auto u=g.uintT,f=g.floatT,b=g.boolT;
    const auto cu=[&](std::uint32_t v){return g.constants.at({u,v});};
    const auto cf=[&](std::uint32_t v){return g.constants.at({f,v});};
    const auto op=[&](unsigned o,std::uint32_t type,std::initializer_list<std::uint32_t> args)->std::uint32_t {
        const std::uint32_t id=g.bound++;
        code.push_back((std::uint32_t(args.size()+3)<<16)|o);code.push_back(type);code.push_back(id);
        code.insert(code.end(),args.begin(),args.end());
        return id;
    };
    const auto bits=op(OpBitcast,u,{z});
    // normal 20e4 range (z >= 2^-14): round the float32 mantissa to 20 bits, ties to even (a carry moves up the exponent)
    const auto s3=op(OpShiftRightLogical,u,{bits,cu(3)});
    const auto lsb=op(OpBitwiseAnd,u,{s3,cu(1)});
    const auto a1=op(OpIAdd,u,{bits,cu(3)});
    const auto a2=op(OpIAdd,u,{a1,lsb});
    const auto nb0=op(OpBitwiseAnd,u,{a2,cu(0xFFFFFFF8u)});
    const auto over=op(OpUGreaterThan,b,{nb0,cu(0x3FFFFFF8u)}); // >= 2 after rounding (and +Inf): the largest 20e4 value (1.999998)
    const auto nb=op(OpSelect,u,{over,cu(0x3FFFFFF8u),nb0});
    const auto nf=op(OpBitcast,f,{nb});
    // denormal 20e4 range (z < 2^-14): the nearest multiple of 2^-34, ties to even (xenos_depth_word.h's shift and rest)
    const auto e=op(OpShiftRightLogical,u,{bits,cu(23)});
    const auto m=op(OpBitwiseAnd,u,{bits,cu(0x007FFFFFu)});
    const auto full=op(OpBitwiseOr,u,{m,cu(0x00800000u)});
    const auto sh0=op(OpISub,u,{cu(116),e});
    const auto big=op(OpUGreaterThan,b,{sh0,cu(24)});
    const auto sh=op(OpSelect,u,{big,cu(24),sh0}); // (<= 24: a defined shift; larger shifts are the tiny case below)
    const auto kept=op(OpShiftRightLogical,u,{full,sh});
    const auto one=op(OpShiftLeftLogical,u,{cu(1),sh});
    const auto mask=op(OpISub,u,{one,cu(1)});
    const auto rest=op(OpBitwiseAnd,u,{full,mask});
    const auto half=op(OpShiftRightLogical,u,{one,cu(1)});
    const auto gt=op(OpUGreaterThan,b,{rest,half});
    const auto eq=op(OpIEqual,b,{rest,half});
    const auto odd0=op(OpBitwiseAnd,u,{kept,cu(1)});
    const auto odd=op(OpINotEqual,b,{odd0,cu(0)});
    const auto tie=op(OpLogicalAnd,b,{eq,odd});
    const auto up=op(OpLogicalOr,b,{gt,tie});
    const auto inc=op(OpSelect,u,{up,cu(1),cu(0)});
    const auto k2=op(OpIAdd,u,{kept,inc});
    const auto tiny=op(OpULessThan,b,{e,cu(92)}); // z < 2^-35: below half the smallest denormal (and float32 denormals, zero)
    const auto k3=op(OpSelect,u,{tiny,cu(0),k2});
    const auto kf=op(OpConvertUToF,f,{k3});      // (k3 <= 2^20: exact)
    const auto df=op(OpFMul,f,{kf,cf(0x2E800000u)}); // (a power of two: exact)
    const auto isNormal=op(OpUGreaterThanEqual,b,{bits,cu(0x38800000u)});
    const auto q0=op(OpSelect,f,{isNormal,nf,df});
    const auto positive=op(OpFOrdGreaterThan,b,{z,cf(fbits(0.0f))}); // (false for zero, negatives and NaN)
    return op(OpSelect,f,{positive,q0,cf(fbits(0.0f))});
}
inline bool parse(const std::vector<std::uint32_t>& w,std::vector<Ins>& list) {
    if(w.size()<5 || w[0]!=0x07230203u)return false;
    for(std::size_t i=5;i<w.size();) {
        const unsigned count=w[i]>>16,op=w[i]&0xFFFF;
        if(!count || i+count>w.size())return false;
        list.push_back({i,op,count});i+=count;
    }
    return true;
}
} // namespace dr_detail

struct DepthRoundResult {std::optional<std::vector<std::uint32_t>> code;const char* why="";unsigned sites=0;bool exported=false;};

inline DepthRoundResult roundFragmentDepth(const std::vector<std::uint32_t>& in) {
    using namespace dr_detail;
    DepthRoundResult r;
    std::vector<Ins> list;
    if(!parse(in,list)){r.why="not-spirv";return r;}
    std::uint32_t bound=in[3];
    Globals g(bound);g.scan(in,list);
    if(!g.floatT){r.why="no-float-type";return r;}
    // the entry point, its function, FragCoord / FragDepth variables
    std::size_t entryAt=SIZE_MAX;unsigned entries=0;std::uint32_t entryFn=0;
    std::set<std::uint32_t> builtinCoord,builtinDepth;
    std::size_t lastMode=SIZE_MAX,lastAnnotation=SIZE_MAX,firstFunction=SIZE_MAX,firstGlobal=SIZE_MAX;
    for(std::size_t k=0;k<list.size();++k) {
        const auto& x=list[k];const auto* p=&in[x.at];
        if(x.op==OpEntryPoint){++entries;if(x.count>=4 && p[1]==ModelFragment){entryAt=k;entryFn=p[2];}}
        else if(x.op==OpExecutionMode)lastMode=k;
        else if(isAnnotation(x.op)) {
            lastAnnotation=k;
            if(x.op==OpDecorate && x.count>=4 && p[2]==DecorationBuiltIn){if(p[3]==BuiltInFragCoord)builtinCoord.insert(p[1]);if(p[3]==BuiltInFragDepth)builtinDepth.insert(p[1]);}
        }
        else if(x.op==OpFunction && firstFunction==SIZE_MAX)firstFunction=k;
        if(firstGlobal==SIZE_MAX && (x.op==OpTypeVoid || x.op==OpTypeBool || x.op==OpTypeInt || x.op==OpTypeFloat || x.op==OpTypeVector ||
                                    x.op==OpTypePointer || x.op==OpTypeFunction || x.op==OpConstant || x.op==OpVariable))firstGlobal=k;
    }
    if(entries!=1 || entryAt==SIZE_MAX){r.why="not-one-fragment-entry";return r;}
    if(firstFunction==SIZE_MAX){r.why="no-function";return r;}
    std::uint32_t coordVar=0,depthVar=0;
    for(const auto& x:list) {
        const auto* p=&in[x.at];
        if(x.op==OpVariable && x.count>=4) {
            if(p[3]==StorageInput && builtinCoord.count(p[2]))coordVar=p[2];
            if(p[3]==StorageOutput && builtinDepth.count(p[2]))depthVar=p[2];
        }
    }
    for(const auto& x:list)
        if((x.op==OpAccessChain || x.op==OpInBoundsAccessChain || x.op==OpPtrAccessChain) && x.count>=4 && depthVar && in[x.at+3]==depthVar)
            {r.why="fragdepth-access-chain";return r;}
    // the entry function's instruction range
    std::size_t fnBegin=SIZE_MAX,fnEnd=SIZE_MAX;
    for(std::size_t k=firstFunction;k<list.size();++k) {
        if(list[k].op==OpFunction && in[list[k].at+2]==entryFn && fnBegin==SIZE_MAX)fnBegin=k;
        if(fnBegin!=SIZE_MAX && list[k].op==OpFunctionEnd){fnEnd=k;break;}
    }
    if(fnBegin==SIZE_MAX || fnEnd==SIZE_MAX){r.why="no-entry-function";return r;}
    quantizeGlobals(g);
    std::vector<std::uint32_t> addInterface,addDecorations;
    std::map<std::size_t,std::vector<std::uint32_t>> before; // list index -> words inserted before it
    std::map<std::size_t,std::uint32_t> storeValue;          // list index of an OpStore -> the value it stores instead
    r.exported=depthVar!=0;
    if(depthVar) { // the shader's own FragDepth stores, in any function
        for(std::size_t k=firstFunction;k<list.size();++k) {
            const auto& x=list[k];
            if(x.op==OpStore && x.count>=3 && in[x.at+1]==depthVar) {
                auto& code=before[k];storeValue[k]=quantizeSequence(g,code,in[x.at+2]);++r.sites;
            }
        }
        if(!r.sites){r.why="fragdepth-never-stored";return r;}
    } else {
        const auto f=g.floatType(),v4=g.vec4Type();
        if(!coordVar) {
            coordVar=g.variable(g.inVec4Ptr(),StorageInput);
            addDecorations.insert(addDecorations.end(),{(4u<<16)|OpDecorate,coordVar,DecorationBuiltIn,BuiltInFragCoord});
        }
        depthVar=g.variable(g.outFloatPtr(),StorageOutput);
        addDecorations.insert(addDecorations.end(),{(4u<<16)|OpDecorate,depthVar,DecorationBuiltIn,BuiltInFragDepth});
        addInterface.push_back(depthVar);
        for(std::size_t k=fnBegin;k<fnEnd;++k)if(list[k].op==OpReturn) {
            auto& code=before[k];
            const std::uint32_t c=bound++,z=bound++;
            code.insert(code.end(),{(4u<<16)|OpLoad,v4,c,coordVar,(5u<<16)|OpCompositeExtract,f,z,c,2});
            const auto q=quantizeSequence(g,code,z);
            code.insert(code.end(),{(3u<<16)|OpStore,depthVar,q});
            ++r.sites;
        }
        if(!r.sites){r.why="no-return";return r;}
    }
    // the entry point's interface: FragCoord / FragDepth when not listed yet
    {
        const auto& x=list[entryAt];const auto* p=&in[x.at];
        // operands: model, function, name (nul-terminated words), interface ids
        std::size_t nameEnd=3;while(nameEnd<x.count){const std::uint32_t wd=p[nameEnd++];if(!(wd&0xFF000000u) || !(wd&0x00FF0000u) || !(wd&0x0000FF00u) || !(wd&0xFFu))break;}
        std::set<std::uint32_t> listed(p+nameEnd,p+x.count);
        if(!listed.count(coordVar) && !r.exported)addInterface.insert(addInterface.begin(),coordVar);
        for(auto it=addInterface.begin();it!=addInterface.end();)if(listed.count(*it))it=addInterface.erase(it);else ++it;
    }
    // the entry's execution modes dropped (one DepthReplacing is added after the last execution mode, or after the entry point)
    const auto dropped=[&](const Ins& x,const std::uint32_t* p) {
        return x.op==OpExecutionMode && x.count>=3 && p[1]==entryFn &&
               (p[2]==ModeEarlyFragmentTests || p[2]==ModeDepthGreater || p[2]==ModeDepthLess || p[2]==ModeDepthUnchanged || p[2]==ModeDepthReplacing);
    };
    std::vector<std::uint32_t> out(in.begin(),in.begin()+5);out.reserve(in.size()+g.globals.size()+before.size()*96+64);
    for(std::size_t k=0;k<list.size();++k) {
        const auto& x=list[k];const auto* p=&in[x.at];
        if(k==firstGlobal && lastAnnotation==SIZE_MAX)out.insert(out.end(),addDecorations.begin(),addDecorations.end());
        if(k==firstFunction)out.insert(out.end(),g.globals.begin(),g.globals.end());
        if(const auto it=before.find(k);it!=before.end())out.insert(out.end(),it->second.begin(),it->second.end());
        if(dropped(x,p)){}
        else if(k==entryAt && !addInterface.empty()) {
            out.push_back((std::uint32_t(x.count+addInterface.size())<<16)|OpEntryPoint);
            out.insert(out.end(),p+1,p+x.count);out.insert(out.end(),addInterface.begin(),addInterface.end());
        } else if(const auto sv=storeValue.find(k);sv!=storeValue.end()) {
            out.insert(out.end(),p,p+x.count);out[out.size()-x.count+2]=sv->second;
        } else out.insert(out.end(),p,p+x.count);
        if(k==(lastMode==SIZE_MAX?entryAt:lastMode))out.insert(out.end(),{(3u<<16)|OpExecutionMode,entryFn,ModeDepthReplacing});
        if(k==lastAnnotation)out.insert(out.end(),addDecorations.begin(),addDecorations.end());
    }
    out[3]=bound;
    r.code=std::move(out);r.why="patched";
    return r;
}

// The fragment stage of a null-pixel-shader draw on a rounded target: FragDepth = q(FragCoord.z), entry "shaderMain".
inline std::vector<std::uint32_t> depthRoundNullModule() {
    using namespace dr_detail;
    std::uint32_t bound=1;
    Globals g(bound);
    const std::uint32_t voidT=bound++,fnT=bound++,main=bound++,label=bound++;
    const auto f=g.floatType(),v4=g.vec4Type();
    const auto coord=g.variable(g.inVec4Ptr(),StorageInput),depth=g.variable(g.outFloatPtr(),StorageOutput);
    quantizeGlobals(g);
    std::vector<std::uint32_t> body;
    const std::uint32_t c=bound++,z=bound++;
    body.insert(body.end(),{(4u<<16)|OpLoad,v4,c,coord,(5u<<16)|OpCompositeExtract,f,z,c,2});
    const auto q=quantizeSequence(g,body,z);
    body.insert(body.end(),{(3u<<16)|OpStore,depth,q});
    std::vector<std::uint32_t> w{0x07230203u,0x00010000u,0u,0u,0u};
    w.insert(w.end(),{(2u<<16)|OpCapability,1u}); // Shader
    w.insert(w.end(),{(3u<<16)|14u,0u,1u});       // OpMemoryModel Logical GLSL450
    // OpEntryPoint Fragment %main "shaderMain" %coord %depth ("shaderMain\0" = 11 bytes: 3 words)
    w.insert(w.end(),{(8u<<16)|OpEntryPoint,ModelFragment,main,0x64616873u,0x614D7265u,0x00006E69u,coord,depth});
    w.insert(w.end(),{(3u<<16)|OpExecutionMode,main,ModeOriginUpperLeft,(3u<<16)|OpExecutionMode,main,ModeDepthReplacing});
    w.insert(w.end(),{(4u<<16)|OpDecorate,coord,DecorationBuiltIn,BuiltInFragCoord,(4u<<16)|OpDecorate,depth,DecorationBuiltIn,BuiltInFragDepth});
    w.insert(w.end(),{(2u<<16)|OpTypeVoid,voidT,(3u<<16)|OpTypeFunction,fnT,voidT});
    w.insert(w.end(),g.globals.begin(),g.globals.end());
    w.insert(w.end(),{(5u<<16)|OpFunction,voidT,main,0u,fnT,(2u<<16)|OpLabel,label});
    w.insert(w.end(),body.begin(),body.end());
    w.insert(w.end(),{(1u<<16)|OpReturn,(1u<<16)|OpFunctionEnd});
    w[3]=bound;
    return w;
}

// Host test only: every OpCompositeConstruct of a float vec4 from four equal operands x builds it from q(x) instead.
inline DepthRoundResult roundCompositeForTest(const std::vector<std::uint32_t>& in) {
    using namespace dr_detail;
    DepthRoundResult r;std::vector<Ins> list;
    if(!parse(in,list)){r.why="not-spirv";return r;}
    std::uint32_t bound=in[3];Globals g(bound);g.scan(in,list);
    if(!g.floatT || !g.vec4T){r.why="no-vec4";return r;}
    std::size_t firstFunction=SIZE_MAX;for(std::size_t k=0;k<list.size();++k)if(list[k].op==OpFunction){firstFunction=k;break;}
    if(firstFunction==SIZE_MAX){r.why="no-function";return r;}
    quantizeGlobals(g);
    std::map<std::size_t,std::vector<std::uint32_t>> before;std::map<std::size_t,std::uint32_t> value;
    for(std::size_t k=firstFunction;k<list.size();++k) {
        const auto& x=list[k];const auto* p=&in[x.at];
        if(x.op==OpCompositeConstruct && x.count==7 && p[1]==g.vec4T && p[3]==p[4] && p[3]==p[5] && p[3]==p[6])
            {value[k]=quantizeSequence(g,before[k],p[3]);++r.sites;}
    }
    if(!r.sites){r.why="no-site";return r;}
    std::vector<std::uint32_t> out(in.begin(),in.begin()+5);
    for(std::size_t k=0;k<list.size();++k) {
        const auto& x=list[k];const auto* p=&in[x.at];
        if(k==firstFunction)out.insert(out.end(),g.globals.begin(),g.globals.end());
        if(const auto it=before.find(k);it!=before.end())out.insert(out.end(),it->second.begin(),it->second.end());
        out.insert(out.end(),p,p+x.count);
        if(const auto v=value.find(k);v!=value.end())for(unsigned j=3;j<7;++j)out[out.size()-x.count+j]=v->second;
    }
    out[3]=bound;r.code=std::move(out);r.why="patched";return r;
}

} // namespace recomp::gpu::generic::spirv
