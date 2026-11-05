#pragma once
// fsr-modes (agent lane, on v458; render_scale.h): the fragment position a guest pixel shader reads, in GUEST pixels, on a
// render target whose host image is larger or smaller than the console's.
//
// The Xbox 360 gives a pixel shader its window position in the vPos register. The shader translation (XenosRecomp) reads it
// from SV_Position: r = float4((iPos.xy - 0.5) * float2(face, 1), 0, 0), i.e. the SPIR-V FragCoord built-in. 162 of the
// device pack's 1992 pixel shaders load FragCoord (agents/fsr-modes/tools/pack_pixel_units.py); the game uses vPos to turn a
// pixel into a screen texture coordinate (vPos * 1/1280, 1/720 constants: deferred lights, decals, screen-space effects) and
// for stipple patterns. On a scaled target FragCoord counts HOST pixels, so those coordinates would be off by the scale.
//
// scaleFragCoordXY(words, factor): every OpLoad of a variable decorated BuiltIn FragCoord yields
//     vec4(x * factor, y * factor, z, w)
// instead of (x, y, z, w): the load's result id is given to a new OpFMul by a constant vec4(factor, factor, 1, 1) placed right
// after the load, and the load itself gets a fresh id, so every use of the original id sees the scaled value and nothing else
// in the module changes (z, the depth, and w are multiplied by exactly 1.0). With factor = 16 / k (render_scale.h
// fragCoordFactor) the pixel centre (i + 0.5) of host pixel i becomes the guest position (i + 0.5) * 16 / k: the continuous
// position of that host pixel in the 1280x720 frame, so vPos-derived texture coordinates land where they do on the console.
// Above 100 % (factor < 1) the result is also clamped: vec4(max(x * factor, 0.5), max(y * factor, 0.5), max(z, 0), max(w, 0)),
// one GLSL.std.450 FMax by a constant vec4(0.5, 0.5, 0, 0) after the multiply (z is in [0, 1] and w = 1/w is positive, so both
// pass unchanged). Why: the translated shaders read vPos as (FragCoord.xy - 0.5) * (face ? 1 : -1, 1), so the sign of vPos.x
// carries the face; on the console the smallest value is +0 / -0 (the first pixel centre is 0.5). Without the clamp the
// first host column (and row) of a larger target would give 0.5 * factor - 0.5 < 0: a wrong face sign in that one column.
// At or below 100 % (factor >= 1) no position is below 0.5 and nothing is added.
// Refused (nullopt, `why` set; the caller keeps the unscaled module and counts it): not SPIR-V, FragCoord not a float vec4
// input, FragCoord named as a pointer by anything but OpLoad (an access chain, a copy, a store, a call or interpolation argument).
// A module that never loads FragCoord is returned unchanged (changed=false).
// loadsFragCoord(words): whether the module loads it at all (the renderer selects the scaled variant only for those).
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace recomp::gpu::generic::spirv {

namespace fs_detail {
enum : unsigned {OpName=5,OpMemberName=6,OpEntryPoint=15,OpTypeFloat=22,OpTypeVector=23,OpTypePointer=32,OpConstant=43,
    OpConstantComposite=44,OpFunction=54,OpVariable=59,OpLoad=61,OpDecorate=71,OpFMul=133,OpExtInstImport=11,OpExtInst=12,OpMemoryModel=14,
    DecorationBuiltIn=11,BuiltInFragCoord=15,StorageInput=1,GlslFMax=40};
inline constexpr std::uint32_t GlslName[4]{0x4C534C47u,0x6474732Eu,0x3035342Eu,0u}; // "GLSL.std.450" and its terminator, as SPIR-V packs a string
inline bool walk(const std::uint32_t* w,std::size_t n,std::vector<std::size_t>& at) {
    if(n<5 || w[0]!=0x07230203u)return false;
    for(std::size_t k=5;k<n;) {
        const unsigned count=w[k]>>16;
        if(!count || k+count>n)return false;
        at.push_back(k);k+=count;
    }
    return true;
}
// The ids of variables decorated BuiltIn FragCoord.
inline std::vector<std::uint32_t> fragCoordVariables(const std::uint32_t* w,const std::vector<std::size_t>& at) {
    std::vector<std::uint32_t> v;
    for(const auto k:at)if((w[k]&0xFFFFu)==OpDecorate && (w[k]>>16)==4 && w[k+2]==DecorationBuiltIn && w[k+3]==BuiltInFragCoord)v.push_back(w[k+1]);
    return v;
}
inline bool isOne(const std::vector<std::uint32_t>& v,std::uint32_t id){for(const auto x:v)if(x==id)return true;return false;}
// Whether instruction p (opcode op, count words) names one of `vars` as an id operand, other than as the pointer of an OpLoad:
// the instructions that can take a variable's id (a pointer) at all. Only id operand positions are examined (a literal equal to
// the id's number is not a use).
inline bool usesAsPointer(unsigned op,unsigned count,const std::uint32_t* p,const std::vector<std::uint32_t>& vars) {
    unsigned first=0; // the first operand index that is an id which may be a pointer (0: none)
    switch(op) {
    case 12: first=5;break;                                  // OpExtInst: type, result, set, instruction, then ids (interpolateAt*)
    case 57: first=4;break;                                  // OpFunctionCall: type, result, function, arguments
    case 60: case 65: case 66: case 67: case 70: first=3;break; // OpImageTexelPointer, Op(InBounds)(Ptr)AccessChain: base
    case 62: case 63: case 64: first=1;break;                // OpStore, OpCopyMemory(Sized): pointers
    case 68: case 83: first=3;break;                         // OpArrayLength, OpCopyObject
    case 169: case 245: first=3;break;                       // OpSelect, OpPhi
    case 401: case 402: case 403: first=3;break;             // OpPtrEqual, OpPtrNotEqual, OpPtrDiff
    default: return false;
    }
    for(unsigned i=first;i<count;++i)if(isOne(vars,p[i]))return true;
    return false;
}
}

inline bool loadsFragCoord(const std::uint32_t* w,std::size_t n) {
    using namespace fs_detail;
    std::vector<std::size_t> at;
    if(!walk(w,n,at))return false;
    const auto vars=fragCoordVariables(w,at);
    if(vars.empty())return false;
    for(const auto k:at)if((w[k]&0xFFFFu)==OpLoad && (w[k]>>16)>=4 && isOne(vars,w[k+3]))return true;
    return false;
}
inline bool loadsFragCoord(const std::vector<std::uint32_t>& words){return loadsFragCoord(words.data(),words.size());}

struct FragScaleResult {std::optional<std::vector<std::uint32_t>> code;const char* why="";bool changed=false;unsigned loads=0;};

inline FragScaleResult scaleFragCoordXY(const std::vector<std::uint32_t>& words,float factor) {
    using namespace fs_detail;
    FragScaleResult result;
    std::vector<std::size_t> at;
    if(!walk(words.data(),words.size(),at)){result.why="not-spirv";return result;}
    const auto* w=words.data();
    const auto vars=fragCoordVariables(w,at);
    // Every use of a FragCoord variable must be one this rewrite understands; and find what the edits need.
    std::uint32_t floatT=0;std::size_t firstFunction=0;unsigned loads=0;
    for(const auto k:at)if((w[k]&0xFFFFu)==OpTypeFloat && (w[k]>>16)==3 && w[k+2]==32 && !floatT)floatT=w[k+1];
    for(const auto k:at)if((w[k]&0xFFFFu)==OpFunction){firstFunction=k;break;}
    std::uint32_t vec4T=0; // the loads' result type (one type for all of them)
    for(const auto k:at) {
        const unsigned op=w[k]&0xFFFFu,count=w[k]>>16;
        if(op==OpDecorate || op==OpName || op==OpMemberName || op==OpEntryPoint)continue; // (names and the interface list may name it)
        if(op==OpVariable && count>=4 && isOne(vars,w[k+2])) {
            if(w[k+3]!=StorageInput){result.why="fragcoord-not-input";return result;}
            continue;
        }
        if(op==OpLoad && count>=4 && isOne(vars,w[k+3])) {
            if(vec4T && vec4T!=w[k+1]){result.why="fragcoord-load-types";return result;}
            vec4T=w[k+1];++loads;continue;
        }
        if(usesAsPointer(op,count,w+k,vars)) {
#if !defined(RECOMP_RS_MUTANT_FRAG_OTHER_USE) // test mutant: other uses of the variable left unscaled without refusing
            result.why="fragcoord-other-use";return result;
#endif
        }
    }
    if(!loads){result.code=words;return result;} // (not loaded: unchanged)
    // The load type must be a vector of 4 x float32.
    bool vec4Ok=false;
    for(const auto k:at)if((w[k]&0xFFFFu)==OpTypeVector && (w[k]>>16)==4 && w[k+1]==vec4T && floatT && w[k+2]==floatT && w[k+3]==4)vec4Ok=true;
    if(!vec4Ok || !firstFunction){result.why="fragcoord-type";return result;}
#if defined(RECOMP_RS_MUTANT_FRAG_NO_CLAMP) // test mutant: no clamp above 100 %
    const bool clamp=false;
#else
    const bool clamp=factor<1.0f; // (above 100 % only: see the top of the file)
#endif
    std::uint32_t glsl=0;std::size_t memoryModel=0; // the GLSL.std.450 instruction set (imported here when the module has none)
    if(clamp) {
        for(const auto k:at) {
            const unsigned op=w[k]&0xFFFFu,count=w[k]>>16;
            if(op==OpExtInstImport && count==6 && !std::memcmp(w+k+2,GlslName,sizeof GlslName) && !glsl)glsl=w[k+1];
            if(op==OpMemoryModel && !memoryModel)memoryModel=k;
        }
        if(!glsl && !memoryModel){result.why="fragcoord-no-memory-model";return result;}
    }
    std::vector<std::uint32_t> out;out.reserve(words.size()+64+loads*12);
    std::uint32_t bound=words[3];
    std::uint32_t fb,ob,hb;const float one=1.0f,half=0.5f;std::memcpy(&fb,&factor,4);std::memcpy(&ob,&one,4);std::memcpy(&hb,&half,4);
    const std::uint32_t cf=bound++,c1=bound++,cv=bound++;
    const std::uint32_t ch=clamp?bound++:0,c0=clamp?bound++:0,cm=clamp?bound++:0; // 0.5, 0.0, vec4(0.5, 0.5, 0, 0)
    const bool import=clamp && !glsl;if(import)glsl=bound++;
    out.insert(out.end(),words.begin(),words.begin()+5);
    for(const auto k:at) {
        const unsigned op=w[k]&0xFFFFu,count=w[k]>>16;
        if(import && k==memoryModel)out.insert(out.end(),{(6u<<16)|OpExtInstImport,glsl,GlslName[0],GlslName[1],GlslName[2],GlslName[3]}); // (imports go right before OpMemoryModel)
        if(k==firstFunction) { // the three constants, after every type and before the first function
            out.insert(out.end(),{(4u<<16)|OpConstant,floatT,cf,fb,(4u<<16)|OpConstant,floatT,c1,ob});
#if defined(RECOMP_RS_MUTANT_FRAG_Z) // test mutant: depth scaled too
            out.insert(out.end(),{(7u<<16)|OpConstantComposite,vec4T,cv,cf,cf,cf,c1});
#elif defined(RECOMP_RS_MUTANT_FRAG_X_ONLY) // test mutant: only x scaled
            out.insert(out.end(),{(7u<<16)|OpConstantComposite,vec4T,cv,cf,c1,c1,c1});
#else
            out.insert(out.end(),{(7u<<16)|OpConstantComposite,vec4T,cv,cf,cf,c1,c1});
#endif
            if(clamp)out.insert(out.end(),{(4u<<16)|OpConstant,floatT,ch,hb,(4u<<16)|OpConstant,floatT,c0,0u,(7u<<16)|OpConstantComposite,vec4T,cm,ch,ch,c0,c0});
        }
        if(op==OpLoad && count>=4 && isOne(vars,w[k+3])) {
            const std::uint32_t original=w[k+2],raw=bound++;
            out.insert(out.end(),w+k,w+k+count);out[out.size()-count+2]=raw;
            if(!clamp)out.insert(out.end(),{(5u<<16)|OpFMul,vec4T,original,raw,cv});
            else {const std::uint32_t scaled=bound++;
                out.insert(out.end(),{(5u<<16)|OpFMul,vec4T,scaled,raw,cv,(7u<<16)|OpExtInst,vec4T,original,glsl,GlslFMax,scaled,cm});}
            continue;
        }
        out.insert(out.end(),w+k,w+k+count);
    }
    out[3]=bound;
    result.code=std::move(out);result.changed=true;result.loads=loads;
    return result;
}

} // namespace recomp::gpu::generic::spirv
