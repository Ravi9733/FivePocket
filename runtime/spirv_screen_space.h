#pragma once
// Attempt v157: vertex shaders drawn with the Xenos viewport transform disabled (PA_CL_VTE_CNTL scale/offset bits 0-5
// all clear). The guest position is then already a window position in pixels (Xenia draw_util: a disabled scale is 1,
// a disabled offset 0; its host shaders undo the host viewport with ndc_scale/ndc_offset). The pack's modules
// (XenosRecomp HLSL -> DXC -fvk-invert-y) store SV_Position after the translator's epilogue
// `oPos.xy += g_HalfPixelOffset * oPos.w` and DXC's y negation, i.e. P = (x, -y, z, w) with x, y guest window
// pixels. Vulkan clips to |x|,|y| <= w, so the pixels must become NDC in the shader:
//     P' = P * S + O * P.w,   S = (sx, sy, 1, 1), O = (ox, oy, 0, 0)
// screenSpacePosition() inserts exactly that before every whole-vector OpStore to the BuiltIn Position output
// (four instructions and eight constants; nothing else changes). The renderer uses a fixed 4096 x 4096 host
// viewport, so S = (2/4096, -2/4096) and O = (-1, -1): window = guest pixel for every target size (one variant per
// vertex shader). Refused (nullopt + why): no Position output, a Position written through an access chain (a
// component store), or no float / vec4 type.
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace recomp::gpu::generic::spirv {

struct ScreenSpaceResult {std::optional<std::vector<std::uint32_t>> code;const char* why="";unsigned stores=0;};

inline ScreenSpaceResult screenSpacePosition(const std::vector<std::uint32_t>& in,float sx,float sy,float ox,float oy) {
    enum : unsigned {OpTypeFloat=22,OpTypeVector=23,OpConstant=43,OpConstantComposite=44,OpFunction=54,OpVariable=59,
        OpStore=62,OpAccessChain=65,OpInBoundsAccessChain=66,OpPtrAccessChain=67,OpDecorate=71,OpCompositeExtract=81,
        OpFAdd=129,OpFMul=133,OpVectorTimesScalar=142,DecorationBuiltIn=11,BuiltInPosition=0,StorageOutput=3};
    ScreenSpaceResult r;
    if(in.size()<5 || in[0]!=0x07230203u){r.why="not-spirv";return r;}
    struct Ins {std::size_t at;unsigned op,count;};
    std::vector<Ins> list;
    for(std::size_t i=5;i<in.size();) {
        const unsigned count=in[i]>>16,op=in[i]&0xFFFF;
        if(!count || i+count>in.size()){r.why="malformed";return r;}
        list.push_back({i,op,count});i+=count;
    }
    // Position output variable(s), float and vec4 types, the first function.
    std::vector<std::uint32_t> decorated;
    std::uint32_t floatType=0,vec4Type=0;std::size_t firstFunction=in.size();
    for(const auto& x:list) {
        const auto* w=&in[x.at];
        if(x.op==OpDecorate && x.count>=4 && w[2]==DecorationBuiltIn && w[3]==BuiltInPosition)decorated.push_back(w[1]);
        else if(x.op==OpTypeFloat && x.count==3 && w[2]==32)floatType=w[1];
        else if(x.op==OpFunction && firstFunction==in.size())firstFunction=x.at;
    }
    for(const auto& x:list) {
        const auto* w=&in[x.at];
        if(x.op==OpTypeVector && x.count==4 && floatType && w[2]==floatType && w[3]==4)vec4Type=w[1];
    }
    std::vector<std::uint32_t> position;
    for(const auto& x:list) {
        const auto* w=&in[x.at];
        if(x.op==OpVariable && x.count>=4 && w[3]==StorageOutput)
            for(const auto d:decorated)if(d==w[2])position.push_back(w[2]);
    }
    if(position.empty()){r.why="no-position-output";return r;}
    if(!floatType || !vec4Type){r.why="no-vec4-type";return r;}
    const auto isPosition=[&](std::uint32_t id){for(const auto p:position)if(p==id)return true;return false;};
    for(const auto& x:list)
        if((x.op==OpAccessChain || x.op==OpInBoundsAccessChain || x.op==OpPtrAccessChain) && x.count>=4 && isPosition(in[x.at+3])) {
            r.why="position-component-store";return r;
        }
    std::uint32_t bound=in[3];
    const auto fresh=[&]{return bound++;};
    const auto bits=[](float f){std::uint32_t u;std::memcpy(&u,&f,4);return u;};
    const std::uint32_t cSx=fresh(),cSy=fresh(),cOx=fresh(),cOy=fresh(),cOne=fresh(),cZero=fresh(),cS=fresh(),cO=fresh();
    std::vector<std::uint32_t> constants{
        (4u<<16)|OpConstant,floatType,cSx,bits(sx),(4u<<16)|OpConstant,floatType,cSy,bits(sy),
        (4u<<16)|OpConstant,floatType,cOx,bits(ox),(4u<<16)|OpConstant,floatType,cOy,bits(oy),
        (4u<<16)|OpConstant,floatType,cOne,bits(1.0f),(4u<<16)|OpConstant,floatType,cZero,bits(0.0f),
        (7u<<16)|OpConstantComposite,vec4Type,cS,cSx,cSy,cOne,cOne,
        (7u<<16)|OpConstantComposite,vec4Type,cO,cOx,cOy,cZero,cZero};
    std::vector<std::uint32_t> out;out.reserve(in.size()+constants.size()+64);
    out.insert(out.end(),in.begin(),in.begin()+5);
    for(const auto& x:list) {
        if(x.at==firstFunction)out.insert(out.end(),constants.begin(),constants.end());
        const auto* w=&in[x.at];
        if(x.op==OpStore && x.count>=3 && isPosition(w[1])) {
            const std::uint32_t value=w[2],pw=fresh(),scaled=fresh(),offset=fresh(),sum=fresh();
            const std::uint32_t add[]{(5u<<16)|OpCompositeExtract,floatType,pw,value,3,
                                      (5u<<16)|OpFMul,vec4Type,scaled,value,cS,
                                      (5u<<16)|OpVectorTimesScalar,vec4Type,offset,cO,pw,
                                      (5u<<16)|OpFAdd,vec4Type,sum,scaled,offset};
            out.insert(out.end(),std::begin(add),std::end(add));
            out.insert(out.end(),w,w+x.count);
            out[out.size()-x.count+2]=sum; // the store now writes the transformed position
            ++r.stores;continue;
        }
        out.insert(out.end(),w,w+x.count);
    }
    if(!r.stores){r.why="no-position-store";return r;}
    if(firstFunction==in.size()){r.why="no-function";return r;}
    out[3]=bound;
    r.code=std::move(out);return r;
}

} // namespace recomp::gpu::generic::spirv
