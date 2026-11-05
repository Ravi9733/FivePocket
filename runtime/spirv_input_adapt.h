#pragma once
// Attempt v77: vertex inputs adapted inside the vertex shader, so that attributes the Adreno 740 cannot fetch
// as the Xenos does are still fetched natively (no CPU conversion, no duplicated stream).
//
//  - convert: the device has no USCALED/SSCALED vertex formats (v69 device log: R16G16_SSCALED=80 and
//    R16G16B16A16_SSCALED=94 unsupported), so v69-v71 decoded such attributes on the CPU into float4 streams.
//    The equivalent native path feeds the same bytes as *_SINT / *_UINT (sign or zero extension of every
//    field, exactly the integers SSCALED/USCALED would have converted) and converts them in the shader: the
//    input variable becomes an int vector and every load of it is followed by OpConvertSToF / OpConvertUToF
//    producing the original load's result id (the rest of the module is untouched).
//  - feed: the v66 vfetch feed (r.k = fetched[feed_k], selectors x y z w 0 1) applied after the load with
//    OpVectorShuffle against a constant (0, 1, 0, 0) vector, instead of v67's second copy of the stream in
//    the complementary endian mode (pair swaps) or v66's CPU decode (every other feed).
//  Access chains into the input (DXC emits `OpAccessChain %_ptr_Input_float %in_var_X %int_c` for a single
//  component) are rewritten per component: the chain's index becomes the fed component, a 0/1 selector
//  replaces the loads through it by the constant, and a converted input's chain points at an int scalar.
//
// Any other use of an adapted input (another opcode reading the variable or a chain into it, a chain of a
// chain, a non-constant or out-of-range chain index, an input that is not a 4-component 32-bit float vector, a
// Component decoration, an existing type needed before the variables but declared after them) refuses the
// whole module: nullopt with the reason, and the renderer keeps v71's path for that draw.
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace recomp::gpu::generic::spirv {

// One input to adapt: its Location, the integer type it now receives (0 float as before, 1 signed int +
// OpConvertSToF, 2 unsigned int + OpConvertUToF) and the feed (3 bits per component; 0x688 is xyzw).
struct InputAdapt {unsigned location=0,convert=0,feed=0x688;};
struct AdaptResult {std::optional<std::vector<std::uint32_t>> code;const char* why="";unsigned loads=0,chains=0;};

inline unsigned feedSelector(unsigned feed,unsigned k){return (feed>>(3*k))&7;}

inline AdaptResult adaptInputs(const std::vector<std::uint32_t>& in,const InputAdapt* adapts,unsigned adaptCount) {
    enum : unsigned {OpExtInst=12,OpEntryPoint=15,OpTypeInt=21,OpTypeFloat=22,OpTypeVector=23,OpTypePointer=32,
        OpConstant=43,OpConstantComposite=44,OpFunction=54,OpVariable=59,OpLoad=61,OpStore=62,OpAccessChain=65,
        OpInBoundsAccessChain=66,OpDecorate=71,OpVectorShuffle=79,OpCompositeExtract=81,OpCompositeInsert=82,
        OpCopyObject=83,OpConvertSToF=111,OpConvertUToF=112,OpSelectionMerge=247,OpLoopMerge=246,
        OpBranchConditional=250,OpSwitch=251,DecorationLocation=30,DecorationComponent=31,StorageInput=1};
    AdaptResult r;
    static thread_local char why[64];
    const auto refuse=[&](const char* w)->AdaptResult {r.code.reset();r.why=w;return r;};
    if(in.size()<5 || in[0]!=0x07230203u)return refuse("not-spirv");
    struct Instruction {std::size_t at;unsigned op,count;};
    std::vector<Instruction> list;
    for(std::size_t i=5;i<in.size();) {
        const unsigned count=in[i]>>16,op=in[i]&0xFFFF;
        if(!count || i+count>in.size())return refuse("malformed");
        list.push_back({i,op,count});i+=count;
    }
    std::map<std::uint32_t,unsigned> locationOf;std::set<std::uint32_t> componentDecorated;
    std::map<std::uint32_t,std::pair<unsigned,unsigned>> intOf;              // id -> width, signedness
    std::map<std::uint32_t,unsigned> floatOf;                                // id -> width
    std::map<std::uint32_t,std::pair<std::uint32_t,unsigned>> vectorOf;      // id -> component type, count
    std::map<std::uint32_t,std::pair<unsigned,std::uint32_t>> pointerOf;     // id -> storage class, pointee
    std::map<std::uint32_t,std::pair<std::uint32_t,std::uint32_t>> constantOf; // 32-bit OpConstant id -> type, value
    std::map<std::uint32_t,std::size_t> defined;                             // type/constant id -> list index
    std::map<std::uint32_t,std::size_t> inputVariable;                       // Input OpVariable id -> list index
    std::size_t firstVariable=list.size(),firstFunction=list.size();
    for(std::size_t k=0;k<list.size();++k) {
        const auto& x=list[k];const auto* w=in.data()+x.at;
        if(x.op==OpFunction){firstFunction=k;break;}
        if(x.op==OpDecorate && x.count>=4 && w[2]==DecorationLocation)locationOf[w[1]]=w[3];
        else if(x.op==OpDecorate && x.count>=3 && w[2]==DecorationComponent)componentDecorated.insert(w[1]);
        else if(x.op==OpTypeInt && x.count==4){intOf[w[1]]={w[2],w[3]};defined[w[1]]=k;}
        else if(x.op==OpTypeFloat && x.count>=3){floatOf[w[1]]=w[2];defined[w[1]]=k;}
        else if(x.op==OpTypeVector && x.count==4){vectorOf[w[1]]={w[2],w[3]};defined[w[1]]=k;}
        else if(x.op==OpTypePointer && x.count==4){pointerOf[w[1]]={w[2],w[3]};defined[w[1]]=k;}
        else if(x.op==OpConstant && x.count==4){constantOf[w[2]]={w[1],w[3]};defined[w[2]]=k;}
        else if(x.op==OpVariable && x.count>=4) {
            if(firstVariable==list.size())firstVariable=k;
            if(w[3]==StorageInput)inputVariable[w[2]]=k;
        }
    }
    if(firstFunction==list.size() || firstVariable==list.size())return refuse("no-function");
    std::uint32_t bound=in[3];
    // New declarations: `early` before the first variable (types the retyped variables need), `late` at the
    // end of the global section (constants only the function bodies use), each in creation order.
    std::vector<std::uint32_t> early,late;
    const auto definedBefore=[&](std::uint32_t id,std::size_t index){const auto it=defined.find(id);return it!=defined.end() && it->second<index;};
    const auto newId=[&]{return bound++;};
    std::uint32_t float32=0;for(const auto& [id,w]:floatOf)if(w==32 && definedBefore(id,firstVariable)){float32=id;break;}
    if(!float32)return refuse("no-float32");
    std::uint32_t vec4Float=0;
    for(const auto& [id,v]:vectorOf)if(v.first==float32 && v.second==4 && definedBefore(id,firstVariable)){vec4Float=id;break;}
    // Early types: found among the declarations before the first variable, else created there. A matching
    // type declared later cannot be referenced there and may not be declared twice: refused.
    bool lateType=false;
    const auto intType=[&](unsigned sign)->std::uint32_t {
        for(const auto& [id,t]:intOf)if(t.first==32 && t.second==sign){if(definedBefore(id,firstVariable))return id;lateType=true;return 0;}
        const auto id=newId();intOf[id]={32,sign};defined[id]=firstVariable-1;early.insert(early.end(),{(4u<<16)|OpTypeInt,id,32,sign});return id;
    };
    const auto vectorType=[&](std::uint32_t component)->std::uint32_t {
        for(const auto& [id,v]:vectorOf)if(v.first==component && v.second==4){if(definedBefore(id,firstVariable))return id;lateType=true;return 0;}
        const auto id=newId();vectorOf[id]={component,4};defined[id]=firstVariable-1;early.insert(early.end(),{(4u<<16)|OpTypeVector,id,component,4});return id;
    };
    const auto inputPointer=[&](std::uint32_t pointee)->std::uint32_t {
        for(const auto& [id,p]:pointerOf)if(p.first==StorageInput && p.second==pointee && definedBefore(id,firstVariable))return id;
        const auto id=newId();pointerOf[id]={StorageInput,pointee};defined[id]=firstVariable-1;
        early.insert(early.end(),{(4u<<16)|OpTypePointer,id,StorageInput,pointee});return id;
    };
    // Late constants: any existing one of that type and value, else created at the end of the globals.
    const auto constant=[&](std::uint32_t type,std::uint32_t value)->std::uint32_t {
        for(const auto& [id,c]:constantOf)if(c.first==type && c.second==value)return id;
        const auto id=newId();constantOf[id]={type,value};late.insert(late.end(),{(4u<<16)|OpConstant,type,id,value});return id;
    };
    std::uint32_t constants01=0; // (0, 1, 0, 0): shuffle selectors 4 and 5 read 0.0 and 1.0
    const auto zeroOne=[&]{
        if(!constants01){const auto zero=constant(float32,0),one=constant(float32,0x3F800000u);constants01=newId();
            late.insert(late.end(),{(7u<<16)|OpConstantComposite,vec4Float,constants01,zero,one,zero,zero});}
        return constants01;
    };
    // The inputs to adapt.
    struct Target {InputAdapt a;std::uint32_t variable=0,intScalar=0,intVector=0,intScalarPointer=0;bool shuffle=false;};
    std::map<std::uint32_t,Target> targets; // variable id -> adaptation
    for(unsigned i=0;i<adaptCount;++i) {
        const auto& a=adapts[i];
        if(a.convert>2){std::snprintf(why,sizeof why,"convert-mode-%u",a.convert);return refuse(why);}
        bool identity=true;for(unsigned k=0;k<4;++k){const unsigned s=feedSelector(a.feed,k);if(s>5)return refuse("feed-selector");identity=identity && s==k;}
        if(!a.convert && identity)continue;
        std::uint32_t variable=0;
        for(const auto& [id,k]:inputVariable)if(locationOf.count(id) && locationOf[id]==a.location){if(variable)return refuse("location-shared");variable=id;}
        if(!variable){std::snprintf(why,sizeof why,"no-input-at-location-%u",a.location);return refuse(why);}
        if(targets.count(variable))return refuse("location-twice");
        if(componentDecorated.count(variable))return refuse("component-decoration");
        const auto* varWords=in.data()+list[inputVariable[variable]].at;
        const auto pointer=pointerOf.find(varWords[1]);
        if(pointer==pointerOf.end() || pointer->second.first!=StorageInput || !vec4Float || pointer->second.second!=vec4Float)return refuse("input-not-float4");
        Target t;t.a=a;t.variable=variable;t.shuffle=!identity;
        if(a.convert) {
            t.intScalar=intType(a.convert==1?1u:0u);if(lateType || !t.intScalar)return refuse("declaration-order");
            t.intVector=vectorType(t.intScalar);if(lateType || !t.intVector)return refuse("declaration-order");
        }
        targets[variable]=t;
    }
    if(targets.empty())return refuse("nothing-to-adapt");
    std::vector<std::uint32_t> out(in);
    for(auto& [variable,t]:targets)if(t.a.convert)out[list[inputVariable[variable]].at+1]=inputPointer(t.intVector);
    // Chains into adapted inputs: chain id -> (variable, fed component or 4/5 for a constant 0/1).
    struct Chain {std::uint32_t variable;unsigned selector;};
    std::map<std::uint32_t,Chain> chains;
    // The function bodies, rewritten into `body` (loads may become two or three instructions).
    std::vector<std::uint32_t> body;body.reserve(in.size()-list[firstFunction].at+64);
    const auto usesAdapted=[&](const Instruction& x,const std::uint32_t* w) {
        // An operand equal to an adapted variable or a chain into one. Literal operands can equal an id:
        // the component/index literals of shuffles, extracts and inserts, the extended instruction number,
        // switch case literals, merge and memory-access masks and branch weights are not compared.
        for(unsigned j=1;j<x.count;++j) {
            if((x.op==OpVectorShuffle && j>=5) || (x.op==OpCompositeExtract && j>=4) || (x.op==OpCompositeInsert && j>=5) ||
               (x.op==OpExtInst && j==4) || (x.op==OpSwitch && j>=3 && (j-3)%2==0) || (x.op==OpLoopMerge && j>=3) ||
               (x.op==OpSelectionMerge && j>=2) || (x.op==OpBranchConditional && j>=4) || (x.op==OpLoad && j>=4) || (x.op==OpStore && j>=3))continue;
            if(targets.count(w[j]) || chains.count(w[j]))return true;
        }
        return false;
    };
    for(std::size_t k=firstFunction;k<list.size();++k) {
        const auto& x=list[k];const auto* w=in.data()+x.at;
        if((x.op==OpAccessChain || x.op==OpInBoundsAccessChain) && x.count>=4 && targets.count(w[3])) {
            const auto& t=targets[w[3]];
            if(x.count!=5)return refuse("chain-depth");
            const auto index=constantOf.find(w[4]);
            if(index==constantOf.end() || !intOf.count(index->second.first) || index->second.second>3)return refuse("chain-index");
            const unsigned c=index->second.second,selector=t.shuffle?feedSelector(t.a.feed,c):c;
            chains[w[2]]={w[3],selector};
            std::vector<std::uint32_t> chain(w,w+x.count);
            if(t.a.convert){auto& tt=targets[w[3]];if(!tt.intScalarPointer)tt.intScalarPointer=inputPointer(tt.intScalar);chain[1]=tt.intScalarPointer;}
            if(selector<4 && selector!=c)chain[4]=constant(index->second.first,selector);
            body.insert(body.end(),chain.begin(),chain.end());++r.chains;
            continue;
        }
        if(x.op==OpLoad && x.count>=4 && targets.count(w[3])) {
            const auto& t=targets[w[3]];
            if(w[1]!=vec4Float)return refuse("load-type");
            const std::uint32_t result=w[2];
            const std::uint32_t loaded=newId();
            body.insert(body.end(),{(x.count<<16)|OpLoad,t.a.convert?t.intVector:vec4Float,loaded});body.insert(body.end(),w+3,w+x.count);
            std::uint32_t value=loaded;
            if(t.a.convert) {
                const std::uint32_t converted=t.shuffle?newId():result;
                body.insert(body.end(),{(4u<<16)|(t.a.convert==1?OpConvertSToF:OpConvertUToF),vec4Float,converted,loaded});value=converted;
            }
            if(t.shuffle) {
                bool constants=false;for(unsigned c=0;c<4;++c)constants=constants || feedSelector(t.a.feed,c)>=4;
                const std::uint32_t second=constants?zeroOne():value;
                body.insert(body.end(),{(9u<<16)|OpVectorShuffle,vec4Float,result,value,second});
                for(unsigned c=0;c<4;++c)body.push_back(feedSelector(t.a.feed,c)); // 0-3 this vector, 4/5 (0,1,0,0).x/.y
            }
            ++r.loads;
            continue;
        }
        if(x.op==OpLoad && x.count>=4 && chains.count(w[3])) {
            const auto& ch=chains[w[3]];const auto& t=targets[ch.variable];
            if(w[1]!=float32)return refuse("chain-load-type");
            if(ch.selector>=4) {
                const auto value=constant(float32,ch.selector==4?0u:0x3F800000u);
                body.insert(body.end(),{(4u<<16)|OpCopyObject,float32,w[2],value});
            } else if(t.a.convert) {
                const std::uint32_t loaded=newId();
                body.insert(body.end(),{(x.count<<16)|OpLoad,t.intScalar,loaded});body.insert(body.end(),w+3,w+x.count);
                body.insert(body.end(),{(4u<<16)|(t.a.convert==1?OpConvertSToF:OpConvertUToF),float32,w[2],loaded});
            } else body.insert(body.end(),w,w+x.count);
            ++r.loads;
            continue;
        }
        if(usesAdapted(x,w)){std::snprintf(why,sizeof why,"use-op-%u",x.op);return refuse(why);}
        body.insert(body.end(),w,w+x.count);
    }
    if(!r.loads)return refuse("never-loaded");
    // Module: header, globals with `early` before the first variable and `late` after the last global.
    std::vector<std::uint32_t> result(out.begin(),out.begin()+5);result[3]=bound;
    result.reserve(in.size()+early.size()+late.size()+(body.size()-(in.size()-list[firstFunction].at)));
    for(std::size_t k=0;k<firstFunction;++k) {
        if(k==firstVariable)result.insert(result.end(),early.begin(),early.end());
        result.insert(result.end(),out.begin()+list[k].at,out.begin()+list[k].at+list[k].count);
    }
    result.insert(result.end(),late.begin(),late.end());
    result.insert(result.end(),body.begin(),body.end());
    r.code=std::move(result);r.why="adapted";
    return r;
}

} // namespace recomp::gpu::generic::spirv
