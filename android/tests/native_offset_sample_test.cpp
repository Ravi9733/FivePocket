// ABI23 offset sample admission. The register values are the ones the device
// recorded for this draw in local-analysis/app-game-cache-replay-ma0_64_a:
// RB_COLOR_MASK 0x0f, RB_DEPTHCONTROL 0x00700770, RB_SURFACE_INFO 0x05000140
// (pitch 320, one sample), scissor 0x00b40140 -> 320x180, one colour target
// 0x10271c10 whose tile cache entry (0x102718b0) is a different object.
//
// The defining difference from the MRT band: nine textures stay bound while the
// pair samples exactly one, so these cases pin that the declared slot is
// SELECTED out of the bound set and the extra bound textures are not inputs.
#include "native_offset_sample_plan.h"
#include <cassert>
#include <cstdio>
#include <string_view>
using namespace recomp::gpu;

namespace {
constexpr std::uint32_t SurfaceInfo=0x05000140;   // pitch 320, one sample
constexpr std::uint32_t Target=0x10271c10,TileCache=0x102718b0;
constexpr std::uint32_t DepthControl=0x00700770;
constexpr std::uint32_t Base=64,Width=320,Height=180;
constexpr unsigned SampledSlot=5;

void put(std::vector<std::uint8_t>& v,unsigned at,std::uint32_t word) {
    for(unsigned b=0;b<4;++b)v[at+b]=static_cast<std::uint8_t>(word>>(24-8*b));
}
std::vector<std::uint8_t> header(std::uint32_t base,std::uint32_t format=0) {
    std::vector<std::uint8_t> h(40,0);put(h,24,SurfaceInfo);put(h,28,base|(format<<16));
    put(h,36,((Width-1)<<18)|((Height-1)<<3));
    return h;
}
Gta5InlineDraw makeDraw() {
    Gta5InlineDraw d;
    d.primitive=8;d.vertexCount=3;d.stride=24;d.vertices.assign(3*24,0);
    d.deviceState.assign(13860,0);
    const auto set=[&](unsigned r,std::uint32_t v){put(d.deviceState,*gta5DrawRegisterOffset(r),v);};
    constexpr std::uint32_t regs[][2]{
        {0x2000,SurfaceInfo},{0x2104,0x0F},{0x2200,DepthControl},
        {0x210F,0x43200000},{0x2110,0x43200000},   // half-extent 160 -> width 320
        {0x2111,0xC3340000},{0x2112,0x43340000},   // half-extent 180 -> height 360
        {0x2302,4}};
    for(const auto& r:regs)set(r[0],r[1]);
    // Height 180 comes from the scissor; the viewport only has to be readable.
    set(0x2111,0xC3340000);set(0x2112,0x43340000);
    put(d.deviceState,10368,SurfaceInfo);
    put(d.deviceState,12816,Target);
    put(d.deviceState,13104,TileCache);
    d.scissorRegisters=std::array<std::uint32_t,3>{0,0,(Height<<16)|Width};
    // Nine bound textures, exactly as the game leaves them; only slot 5 is read.
    for(unsigned slot:{2u,5u,6u,7u,8u,9u,10u,11u,12u}) {
        Gta5InlineDraw::Texture t;t.slot=slot;d.textures.push_back(t);
    }
    return d;
}
NativeTranslatedDrawProgram makeProgram() {
    NativeTranslatedDrawProgram p;
    p.abiVersion=23;p.offsetSample=true;p.indexed=false;p.stride=24;p.interpolators=2;
    p.textureSlot=SampledSlot;p.textureSlots={SampledSlot};
    p.vertexIdentity={1,2};p.pixelIdentity={3,4};
    p.vertexCode={0x07230203,1};p.pixelCode={0x07230203,2};
    return p;
}
Gta5BoundSurfaceCapture makeBindings() {
    Gta5BoundSurfaceCapture c;
    c.surfaces[0].object=Target;c.surfaces[0].tileCacheObject=TileCache;
    c.surfaces[0].header=header(Base);
    return c;
}
unsigned passed=0;
void expect(bool condition,const char* what) {
    if(!condition){std::fprintf(stderr,"OFFSET_SAMPLE_FAIL %s\n",what);std::abort();}
    ++passed;
}
void refuses(const Gta5InlineDraw& d,const NativeTranslatedDrawProgram& p,
             const Gta5BoundSurfaceCapture& b,const char* reason) {
    const char* why=nullptr;
    expect(!planNativeOffsetSampleDraw(d,p,b,&why),reason);
    expect(why && std::string_view(why)==reason,reason);
}
}

int main() {
    const auto draw=makeDraw();const auto program=makeProgram();const auto bindings=makeBindings();
    {
        const char* why=nullptr;
        const auto plan=planNativeOffsetSampleDraw(draw,program,bindings,&why);
        expect(plan.has_value(),"captured offset sample admitted");
        expect(plan->object==Target,"target from the live register");
        expect(plan->width==Width && plan->height==Height,"320x180 extent from owned state");
        expect(plan->pitch==320,"pitch from surface info");
        expect(plan->baseTile==Base,"base from the header");
        expect(plan->tileCount==(320u/80u)*((Height+15u)/16u),"tile count derived");
    }

    // The declared slot is selected out of the bound set; the other eight bound
    // textures are not inputs and must not be required to match.
    {
        auto d=draw;d.textures.clear();
        Gta5InlineDraw::Texture only;only.slot=SampledSlot;d.textures.push_back(only);
        const char* why=nullptr;
        expect(planNativeOffsetSampleDraw(d,program,bindings,&why).has_value(),"sole bound texture admitted");
    }
    {   // The declared slot missing from the bound set is a real refusal.
        auto d=draw;
        d.textures.erase(std::remove_if(d.textures.begin(),d.textures.end(),
            [](const auto& t){return t.slot==SampledSlot;}),d.textures.end());
        refuses(d,program,bindings,"texture-slots");
    }
    {   // A literal-zero placeholder is not a sampled texture.
        auto d=draw;
        for(auto& t:d.textures)if(t.slot==SampledSlot)t.literalZero=true;
        refuses(d,program,bindings,"texture-slots");
    }
    {   // The same slot bound twice is ambiguous rather than harmless.
        auto d=draw;Gta5InlineDraw::Texture dup;dup.slot=SampledSlot;d.textures.push_back(dup);
        refuses(d,program,bindings,"texture-slots");
    }

    // One colour target, every channel, and colour only.
    {auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(0x2104),0xFF);refuses(d,program,bindings,"colour-mask");}
    {auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(0x2104),0);refuses(d,program,bindings,"colour-mask");}
    {auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(0x2200),DepthControl|0x4u);
     refuses(d,program,bindings,"depth-writes-enabled");}
    {auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(0x2200),DepthControl|0x1u);
     refuses(d,program,bindings,"stencil-enabled");}

    // Exactly one attachment, from a live register carrying a readable header.
    {auto b=bindings;b.surfaces[0].header.reset();refuses(draw,program,b,"target-header");}
    {auto b=bindings;b.surfaces[0].object=0;refuses(draw,program,b,"target-register");}
    {auto d=draw;put(d.deviceState,12816+4,0x10270000);
     auto b=bindings;b.surfaces[1].object=0x10270000;refuses(d,program,b,"extra-slot-bound");}
    {auto d=draw;put(d.deviceState,12832,0x10270000);
     auto b=bindings;b.surfaces[4].object=0x10270000;refuses(d,program,b,"extra-slot-bound");}
    {auto b=bindings;b.surfaces[0].header=header(Base);b.surfaces[0].header->at(24)=0;
     refuses(draw,program,b,"target-surface-info");}
    // The device recorded target_colour=00070388: format 7, k_16_16_16_16_FLOAT.
    // That is the format this pass actually writes, so it is admitted and the
    // plan carries it; the ordinary RGBA8 case stays admitted too.
    {
        auto b=bindings;b.surfaces[0].header=header(Base,7);
        const char* why=nullptr;
        const auto plan=planNativeOffsetSampleDraw(draw,program,b,&why);
        expect(plan.has_value(),"k_16_16_16_16_FLOAT target admitted");
        expect(plan->format==7,"plan carries the float colour format");
        expect(nativeOffscreenTexelBytes(plan->format)==8,"float target is eight bytes per texel");
        expect(plan->tileCount==96,"64bpp footprint includes both halves, not just 48 tiles");
    }
    {auto b=bindings;b.surfaces[0].header=header(2000,7);
     refuses(draw,program,b,"edram-wrap");}
    {auto b=bindings;b.surfaces[0].header=header(1952,7);
     expect(planNativeOffsetSampleDraw(draw,program,b).has_value(),"64bpp fits exactly to EDRAM end");}
    expect(!nativeOffscreenTileCount(80,UINT32_MAX,7),"overflow-sized extent refused");
    expect(!nativeOffscreenTileCount(80,16,15),"unknown format footprint refused");
    expect(nativeOffscreenTexelBytes(0)==4,"RGBA8 stays four bytes per texel");
    // The pyramid's last level writes k_32_FLOAT (14), a single float32 channel.
    {
        auto b=bindings;b.surfaces[0].header=header(Base,14);
        const char* why=nullptr;
        const auto plan=planNativeOffsetSampleDraw(draw,program,b,&why);
        expect(plan.has_value(),"k_32_FLOAT target admitted");
        expect(plan->format==14,"plan carries the single-channel float format");
        expect(nativeOffscreenTexelBytes(plan->format)==4,"k_32_FLOAT is four bytes per texel");
    }
    // Any other colour format needs an attachment this band does not create.
    {auto b=bindings;b.surfaces[0].header=header(Base,4);refuses(draw,program,b,"target-colour-format");}
    // Reading and writing the same storage through two owners.
    {auto d=draw;put(d.deviceState,13104,Target);
     auto b=bindings;b.surfaces[0].tileCacheObject=Target;
     refuses(d,program,b,"target-aliases-tile-cache");}

    // ABI24, the tonemap pass, is the SAME draw with richer compiled
    // arithmetic, so it is admitted through this band's predicates and plans
    // identically. Its captured shape is two interpolators and one slot.
    // ABI24 writes k_16_16 (format 4), not the pyramid's float formats, and its
    // compiled stage carries the encode scale for exactly that target -- so the
    // two pairs admit DISJOINT target formats and each refuses the other's.
    {
        auto p=program;p.abiVersion=24;
        auto b=bindings;b.surfaces[0].header=header(Base,4);
        const char* why=nullptr;
        const auto plan=planNativeOffsetSampleDraw(draw,p,b,&why);
        expect(plan.has_value(),"ABI24 tonemap admitted by the offset sample band");
        expect(plan->object==Target && plan->width==Width && plan->height==Height,
            "ABI24 plans the same target geometry as ABI23");
        expect(plan->format==4,"plan carries the fixed-point colour format");
        expect(nativeOffscreenTexelBytes(plan->format)==4,"k_16_16 is four bytes per texel");
        expect(validateNativeOffsetSampleProgram(p),"ABI24 passes the band validator");
    }
    {   // Each pair refuses the other's target format.
        auto p=program;p.abiVersion=24;
        refuses(draw,p,bindings,"target-colour-format");                       // format 0
        auto b7=bindings;b7.surfaces[0].header=header(Base,7);
        refuses(draw,p,b7,"target-colour-format");
        auto b14=bindings;b14.surfaces[0].header=header(Base,14);
        refuses(draw,p,b14,"target-colour-format");
    }
    {   // ABI23 must not take the fixed-point target: it has no encode scale.
        auto b=bindings;b.surfaces[0].header=header(Base,4);
        refuses(draw,program,b,"target-colour-format");
    }
    // Every other predicate still applies to it; the ABI number alone admits
    // nothing.
    {auto p=program;p.abiVersion=24;p.offsetSample=false;refuses(draw,p,bindings,"program-profile");}
    {auto p=program;p.abiVersion=24;p.indexed=true;refuses(draw,p,bindings,"program-profile");}
    {auto p=program;p.abiVersion=24;p.textureSlots={4,5};refuses(draw,p,bindings,"program-profile");}
    // A refusal that is NOT about the format still names itself with the right one.
    {auto p=program;p.abiVersion=24;auto b=bindings;b.surfaces[0].header=header(Base,4);
     b.surfaces[0].object=0;refuses(draw,p,b,"target-register");}
    {auto p=program;p.abiVersion=24;
     expect(!validateNativeRectangleMrtProgram(p),"MRT band refuses ABI24");
     expect(!validateNativeDepthOnlyRectangleProgram(p),"depth-only band refuses ABI24");
     expect(!validateNativeFixed16Program(p),"fixed16 band refuses ABI24");}
    // The band stops at 24: an unminted ABI is not this band's.
    {auto p=program;p.abiVersion=25;refuses(draw,p,bindings,"program-profile");}

    // The program contract is required, not inferred from the state.
    {auto p=program;p.abiVersion=22;refuses(draw,p,bindings,"program-profile");}
    {auto p=program;p.offsetSample=false;refuses(draw,p,bindings,"program-profile");}
    {auto p=program;p.rectangleMrt=true;refuses(draw,p,bindings,"program-profile");}
    {auto p=program;p.interpolators=1;refuses(draw,p,bindings,"program-profile");}
    {auto p=program;p.interpolators=5;refuses(draw,p,bindings,"program-profile");}
    // Three is the tonemap pass's second pair, whose first interpolator carries
    // two lanes; two and four are the pyramid's own shapes.
    for(unsigned count:{2u,3u,4u}) {
        auto p=program;p.interpolators=count;
        expect(validateNativeOffsetSampleProgram(p),"band admits its interpolator counts");
    }
    {auto p=program;p.indexed=true;refuses(draw,p,bindings,"program-profile");}
    {auto p=program;p.pixelCode.clear();refuses(draw,p,bindings,"program-profile");}
    {auto d=draw;d.primitive=4;refuses(d,program,bindings,"program-profile");}
    {auto d=draw;d.vertexCount=4;d.vertices.assign(4*24,0);refuses(d,program,bindings,"program-profile");}

    // Binned/MSAA surfaces are a different pass.
    {auto d=draw;put(d.deviceState,10368,SurfaceInfo|0x10000u);
     put(d.deviceState,*gta5DrawRegisterOffset(0x2000),SurfaceInfo|0x10000u);
     refuses(d,program,bindings,"surface-samples");}

    // Neighbouring bands must not accept this program, nor it theirs.
    expect(!validateNativeRectangleMrtProgram(program),"MRT band refuses ABI23");
    expect(!validateNativeDepthOnlyRectangleProgram(program),"depth-only band refuses ABI23");
    expect(!validateNativeFixed16Program(program),"fixed16 band refuses ABI23");
    {auto p=program;p.abiVersion=22;p.offsetSample=false;p.rectangleMrt=true;p.textureSlots={9,12};
     expect(!validateNativeOffsetSampleProgram(p),"ABI23 band refuses an MRT program");}

    std::printf("NATIVE_OFFSET_SAMPLE_PASS single_register_target=1 slot_selected_from_bound_set=1 "
        "colour_only=1 abi23_and_abi24=1 disjoint_target_formats=1 cases=%u gpu=0 gameplay=0\n",passed);
    return 0;
}
