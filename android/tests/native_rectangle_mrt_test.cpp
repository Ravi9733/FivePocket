// ABI22 rectangle MRT admission. Unlike the ABI20 composite, this pass runs
// with LIVE colour registers, so its two targets come from the registers rather
// than the tile cache. These cases pin that both targets must be genuinely
// bound with readable headers, that the remaining colour slots and the depth
// slot must be unbound, that the pass writes colour only, and that two targets
// sharing EDRAM are refused rather than silently aliased.
//
// The register values are the ones the device recorded for this draw:
// RB_COLOR_MASK 0xff, RB_DEPTHCONTROL 0x00700770, RB_SURFACE_INFO 0x0a000280
// (pitch 640, one sample), viewport half-extents 320 and 180 -> 640x360.
#include "native_rectangle_mrt_plan.h"
#include <cassert>
#include <cstdio>
#include <string_view>
using namespace recomp::gpu;

namespace {
constexpr std::uint32_t SurfaceInfo=0x0a000280;
constexpr std::uint32_t Target0=0x10271bd0,Target1=0x10270dc0;
constexpr std::uint32_t Base0=904,Base1=1440;   // 368 and 184 tiles; disjoint.
constexpr std::uint32_t DepthControl=0x00700770;

void put(std::vector<std::uint8_t>& v,unsigned at,std::uint32_t word) {
    for(unsigned b=0;b<4;++b)v[at+b]=static_cast<std::uint8_t>(word>>(24-8*b));
}
// Colour word: base tile in bits 0-10, Xenos colour format in bits 16-19.
// The captured targets are k_16_16_16_16_FLOAT (7) and k_32_FLOAT (14).
constexpr std::uint32_t Format0=7,Format1=14;
std::vector<std::uint8_t> header(std::uint32_t base,std::uint32_t format) {
    std::vector<std::uint8_t> h(40,0);put(h,24,SurfaceInfo);put(h,28,base|(format<<16));
    put(h,36,((640u-1u)<<18)|((360u-1u)<<3));
    return h;
}
Gta5InlineDraw makeDraw() {
    Gta5InlineDraw d;
    d.primitive=8;d.vertexCount=3;d.stride=24;d.vertices.assign(3*24,0);
    d.deviceState.assign(13860,0);
    const auto set=[&](unsigned r,std::uint32_t v){put(d.deviceState,*gta5DrawRegisterOffset(r),v);};
    constexpr std::uint32_t regs[][2]{
        {0x2000,SurfaceInfo},{0x2104,0xFF},{0x2200,DepthControl},
        {0x210F,0x43A00000},{0x2110,0x43A00000},   // half-extent 320 -> width 640
        {0x2111,0xC3340000},{0x2112,0x43340000},   // half-extent 180 -> height 360
        {0x2302,4}};
    for(const auto& r:regs)set(r[0],r[1]);
    put(d.deviceState,10368,SurfaceInfo);
    put(d.deviceState,12816,Target0);put(d.deviceState,12820,Target1);
    d.scissorRegisters=std::array<std::uint32_t,3>{0,0,(360u<<16)|640u};
    Gta5InlineDraw::Texture a;a.slot=9;Gta5InlineDraw::Texture b;b.slot=12;
    d.textures={a,b};
    return d;
}
NativeTranslatedDrawProgram makeProgram() {
    NativeTranslatedDrawProgram p;
    p.abiVersion=22;p.rectangleMrt=true;p.indexed=false;p.stride=24;p.interpolators=2;
    p.textureSlot=9;p.textureSlots={9,12};
    p.vertexIdentity={1,2};p.pixelIdentity={3,4};
    p.vertexCode={0x07230203,1};p.pixelCode={0x07230203,2};
    return p;
}
Gta5BoundSurfaceCapture makeBindings() {
    Gta5BoundSurfaceCapture c;
    c.surfaces[0].object=Target0;c.surfaces[0].header=header(Base0,Format0);
    c.surfaces[1].object=Target1;c.surfaces[1].header=header(Base1,Format1);
    return c;
}
unsigned passed=0;
void expect(bool condition,const char* what) {
    if(!condition){std::fprintf(stderr,"RECTANGLE_MRT_FAIL %s\n",what);std::abort();}
    ++passed;
}
void refuses(const Gta5InlineDraw& d,const NativeTranslatedDrawProgram& p,
             const Gta5BoundSurfaceCapture& b,const char* reason) {
    const char* why=nullptr;
    expect(!planNativeRectangleMrtDraw(d,p,b,&why),reason);
    expect(why && std::string_view(why)==reason,reason);
}
}

int main() {
    const auto draw=makeDraw();const auto program=makeProgram();const auto bindings=makeBindings();
    {
        const char* why=nullptr;
        const auto plan=planNativeRectangleMrtDraw(draw,program,bindings,&why);
        expect(plan.has_value(),"captured rectangle MRT admitted");
        expect(plan->targets[0].object==Target0 && plan->targets[1].object==Target1,"both targets resolved");
        expect(plan->targets[0].width==640 && plan->targets[0].height==360,"extent from owned state");
        expect(plan->targets[0].pitch==640 && plan->targets[1].pitch==640,"pitch from surface info");
        expect(plan->targets[0].baseTile==Base0 && plan->targets[1].baseTile==Base1,"bases from headers");
        expect(plan->formats[0]==Format0 && plan->formats[1]==Format1,"formats from headers");
        expect(plan->targets[0].tileCount==368 && plan->targets[1].tileCount==184,"format-sized physical tile count");
        expect(plan->targets[0].format==7 && plan->targets[1].format==14,"each footprint carries its own format");
    }

    // The pass writes colour only, into both targets.
    {auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(0x2104),0x0F);refuses(d,program,bindings,"colour-mask");}
    {auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(0x2104),0);refuses(d,program,bindings,"colour-mask");}
    {auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(0x2200),DepthControl|0x4u);
     refuses(d,program,bindings,"depth-writes-enabled");}
    {auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(0x2200),DepthControl|0x1u);
     refuses(d,program,bindings,"stencil-enabled");}

    // Both targets must be named by a live register AND carry a header.
    {auto b=bindings;b.surfaces[1].header.reset();refuses(draw,program,b,"target-header");}
    {auto b=bindings;b.surfaces[0].object=0;refuses(draw,program,b,"target-register");}
    {auto d=draw;put(d.deviceState,12820,0);refuses(d,program,bindings,"target-register");}
    {auto b=bindings;b.surfaces[0].header=header(Base0,Format0);b.surfaces[0].header->at(24)=0;
     refuses(draw,program,b,"target-surface-info");}
    // The declared colour format decides the attachment, so an unmapped one is
    // refused rather than approximated with a wider or narrower image.
    {auto b=bindings;b.surfaces[0].header=header(Base0,0);
     refuses(draw,program,b,"target-colour-format");}
    {auto b=bindings;b.surfaces[1].header=header(Base1,4);
     refuses(draw,program,b,"target-colour-format");}

    // Exactly two attachments: nothing else bound, and no depth.
    {auto d=draw;put(d.deviceState,12816+4*2,0x10270000);
     auto b=bindings;b.surfaces[2].object=0x10270000;refuses(d,program,b,"extra-slot-bound");}
    {auto d=draw;put(d.deviceState,12832,0x10270000);
     auto b=bindings;b.surfaces[4].object=0x10270000;refuses(d,program,b,"extra-slot-bound");}

    // Two attachments sharing EDRAM would alias; the pass writes both.
    {auto b=bindings;b.surfaces[1].header=header(Base0,Format1);
     auto d=draw;put(d.deviceState,12820,Target1);refuses(d,program,b,"targets-overlap");}
    // Previously passed: lies in the formerly uncounted second half of RT0.
    {auto b=bindings;b.surfaces[1].header=header(Base0+184,Format1);
     refuses(draw,program,b,"targets-overlap");}
    {auto b=bindings;b.surfaces[1].header=header(Base0+368,Format1);
     expect(planNativeRectangleMrtDraw(draw,program,b).has_value(),"adjacent but nonoverlapping targets admitted");}
    {auto b=bindings;b.surfaces[0].header=header(1800,Format0);
     refuses(draw,program,b,"edram-wrap");}
    {auto b=bindings;b.surfaces[1].object=Target0;b.surfaces[1].header=header(Base1,Format1);
     auto d=draw;put(d.deviceState,12820,Target0);refuses(d,program,b,"targets-identical");}

    // The program contract is required, not inferred from the state.
    {auto p=program;p.abiVersion=20;refuses(draw,p,bindings,"program-profile");}
    {auto p=program;p.rectangleMrt=false;refuses(draw,p,bindings,"program-profile");}
    {auto p=program;p.interpolators=1;refuses(draw,p,bindings,"program-profile");}
    {auto p=program;p.indexed=true;refuses(draw,p,bindings,"program-profile");}
    {auto p=program;p.pixelCode.clear();refuses(draw,p,bindings,"program-profile");}
    {auto p=program;p.geometryDepth20e4=true;refuses(draw,p,bindings,"program-profile");}
    {auto d=draw;d.primitive=4;refuses(d,program,bindings,"program-profile");}
    {auto d=draw;d.vertexCount=4;d.vertices.assign(4*24,0);refuses(d,program,bindings,"program-profile");}

    // Binned/MSAA surfaces are a different pass.
    {auto d=draw;put(d.deviceState,10368,SurfaceInfo|0x10000u);
     put(d.deviceState,*gta5DrawRegisterOffset(0x2000),SurfaceInfo|0x10000u);
     refuses(d,program,bindings,"surface-samples");}

    // Neighbouring bands must not accept this program.
    expect(!validateNativeDepthOnlyRectangleProgram(program),"depth-only band refuses ABI22");
    expect(!validateNativeFixed16Program(program),"fixed16 band refuses ABI22");
    expect(!validateNativeFixed16RectangleProgram(program),"fixed16 rectangle band refuses ABI22");

    std::printf("NATIVE_RECTANGLE_MRT_PASS two_register_targets=1 disjoint_edram_required=1 "
        "colour_only=1 cases=%u gpu=0 gameplay=0\n",passed);
    return 0;
}
