// ABI25 stencil-only program contract. The scene draw this band exists for
// binds NO pixel shader, disables colour and depth writes and leaves stencil
// enabled, so the stencil buffer is its only output:
//
//   primitive 4 (triangle list), 36 vertices, stride 36, no indices
//   pixel_object 0, no textures, colour and depth targets both bound
//   RB_COLOR_MASK 0, RB_DEPTHCONTROL 0x00720213
//
// The absent pixel stage is the defining property, so these cases pin that a
// program carrying any pixel code is refused however its other fields read.
#include "native_stencil_only_plan.h"
#include "native_stencil_tile_batch.h"
#include "xenos_depth_stencil_state.h"
#include <string_view>
#include <cstdio>
using namespace recomp::gpu;

namespace {
unsigned passed=0;
void expect(bool condition,const char* what) {
    if(!condition){std::fprintf(stderr,"STENCIL_ONLY_FAIL %s\n",what);std::abort();}
    ++passed;
}
NativeTranslatedDrawProgram makeProgram() {
    NativeTranslatedDrawProgram p;
    p.abiVersion=25;p.directTriangles=true;p.indexed=false;p.stride=36;p.interpolators=0;
    // A textureless band says so explicitly; slot 0 is the struct default.
    p.textureSlot=UINT32_MAX;
    p.vertexIdentity={1,2};p.vertexCode={0x07230203,1};
    return p;
}
}

int main() {
    // Production dispatch reducer: no diagnostic string can acknowledge a
    // failed tile, including the former false-success "depth-uninitialized".
    constexpr const char* refusals[]={"target-unusable","tile-range","program-input",
        "pipeline-options","viewport-scissor","input-header","stencil-state",
        "no-stencil-aspect","stencil-uninitialized","no-depth-aspect",
        "depth-uninitialized","surfaces-undescribed","surfaces-differ",
        "pipeline-setup","command-begin","fence","completion-guards",
        "drawn","",nullptr};
    for(const char* reason:refusals)for(unsigned failedTile=0;failedTile<2;++failedTile) {
        unsigned calls=0;
        const auto batch=executeNativeStencilTileBatch(2,[&](unsigned tile,const char** why) {
            ++calls;*why=reason;return tile!=failedTile;
        });
        expect(!batch.completed,"a refused tile never acknowledges the draw");
        expect(batch.accepted==failedTile && calls==failedTile+1,"stop at first refusal, preserve partial count");
        expect(std::string_view(batch.stage)==(reason?reason:"tile-draw"),"diagnostic only, with null fallback");
    }
    for(const char* reason:{"drawn","empty-intersection","depth-uninitialized"}) {
        const auto batch=executeNativeStencilTileBatch(2,[&](unsigned,const char** why) {
            *why=reason;return true;
        });
        expect(batch.completed && batch.accepted==2,"all accepted tiles complete independent of text");
    }
    {
        unsigned calls=0;
        const auto batch=executeNativeStencilTileBatch(0,[&](unsigned,const char**) {++calls;return true;});
        expect(!batch.completed && !batch.accepted && !calls,"empty tile list is not execution");
    }
    std::puts("STENCIL_TILE_BATCH_PASS refusals=20 first_and_later_tile=1 diagnostic_not_authority=1 empty_rejected=1");
    const auto program=makeProgram();
    expect(validateNativeStencilOnlyProgram(program),"captured stencil program admitted");

    // The absent pixel stage is the band, not an incidental property.
    {auto p=program;p.pixelCode={0x07230203,2};expect(!validateNativeStencilOnlyProgram(p),"pixel code refused");}
    {auto p=program;p.pixelIdentity={3,4};expect(!validateNativeStencilOnlyProgram(p),"pixel identity refused");}
    // A vertex stage is still required; nothing rasterises without one.
    {auto p=program;p.vertexCode.clear();expect(!validateNativeStencilOnlyProgram(p),"absent vertex stage refused");}
    {auto p=program;p.vertexIdentity.clear();expect(!validateNativeStencilOnlyProgram(p),"absent vertex identity refused");}

    // No interpolators to carry and no texture to sample.
    {auto p=program;p.interpolators=1;expect(!validateNativeStencilOnlyProgram(p),"interpolators refused");}
    {auto p=program;p.textureSlots={0};expect(!validateNativeStencilOnlyProgram(p),"sampled slot refused");}
    {auto p=program;p.textureSlot=0;expect(!validateNativeStencilOnlyProgram(p),"default slot refused");}

    // Its own topology, and its own ABI.
    {auto p=program;p.indexed=true;expect(!validateNativeStencilOnlyProgram(p),"indexed refused");}
    {auto p=program;p.directTriangles=false;expect(!validateNativeStencilOnlyProgram(p),"non-direct topology refused");}
    {auto p=program;p.abiVersion=24;expect(!validateNativeStencilOnlyProgram(p),"ABI24 refused");}
    {auto p=program;p.abiVersion=26;expect(!validateNativeStencilOnlyProgram(p),"unminted ABI refused");}
    {auto p=program;p.stride=0;expect(!validateNativeStencilOnlyProgram(p),"zero stride refused");}
    {auto p=program;p.stride=35;expect(!validateNativeStencilOnlyProgram(p),"unaligned stride refused");}

    // Neighbouring bands must not accept it, nor it theirs.
    expect(!validateNativeOffsetSampleProgram(program),"offset sample band refuses ABI25");
    expect(!validateNativeRectangleMrtProgram(program),"MRT band refuses ABI25");
    expect(!validateNativeDepthOnlyRectangleProgram(program),"depth-only band refuses ABI25");
    expect(!validateNativeFixed16Program(program),"fixed16 band refuses ABI25");

    // The guest state this band exists for, decoded by the tree's own decoder.
    {
        const auto state=decodeXenosDepthStencilState(0x00720213u,0x00FFFF20u,0x00FFFF00u);
        expect(state.has_value(),"captured depth/stencil state decodes");
        expect(state->stencilEnable,"stencil is the output");
        expect(state->depthEnable && !state->depthWrite,"depth is tested but never written");
        expect(!state->backfaceEnable,"one face state");
        expect(state->depthCompare==1,"depth compare LESS");
        expect(state->front.compare==2,"stencil compare EQUAL");
        expect(state->front.fail==0 && state->front.pass==0,"keep on stencil fail and on pass");
        expect(state->front.depthFail==1,"zero where the depth test fails");
        expect(state->front.reference==32 && state->front.readMask==255 && state->front.writeMask==255,
            "reference 32, read/write masks 255 from actual stencil registers");
        expect(state->back==state->front,"the back face inherits it");
        expect(xenosCompare(state->front.compare,state->front.reference&state->front.readMask,32u),
            "matching stencil passes");
        expect(!xenosCompare(state->front.compare,state->front.reference&state->front.readMask,37u),
            "nonmatching stencil fails and must remain unchanged");
        expect(applyXenosStencilOperation(state->front.depthFail,state->front.reference,0xFFu)==0,
            "the depth-fail operation zeroes");
        expect(applyXenosStencilOperation(state->front.pass,state->front.reference,0xFFu)==0xFF,
            "the pass operation keeps");
    }

    // The plan, against the state the device recorded for this draw.
    {
        constexpr std::uint32_t SurfaceInfo=0x14000500;  // pitch 1280, one sample
        constexpr std::uint32_t Color=0x1026FFC0,Depth=0x1026FE40;
        constexpr std::uint32_t Width=1280,Height=720;
        const auto put=[](std::vector<std::uint8_t>& v,unsigned at,std::uint32_t w) {
            for(unsigned b=0;b<4;++b)v[at+b]=static_cast<std::uint8_t>(w>>(24-8*b));
        };
        const auto header=[&](std::uint32_t base,std::uint32_t format=0) {
            std::vector<std::uint8_t> h(40,0);put(h,24,SurfaceInfo);put(h,28,base|(format<<16));
            put(h,36,((Width-1)<<18)|((Height-1)<<3));
            return h;
        };
        Gta5InlineDraw draw;
        draw.primitive=4;draw.vertexCount=36;draw.stride=36;draw.vertices.assign(36*36,0);
        draw.deviceState.assign(13860,0);
        const auto set=[&](unsigned r,std::uint32_t v){put(draw.deviceState,*gta5DrawRegisterOffset(r),v);};
        set(0x2000,SurfaceInfo);set(0x2104,0);set(0x2200,0x00720213);
        set(0x210D,0x00FFFF20);set(0x210C,0x00FFFF00);set(0x2201,0x00010001);
        set(0x2204,0x80000);set(0x2205,0x18000);set(0x2206,0x43F);
        set(0x2002,0x00010000);  // Actual RB_DEPTH_INFO: base 0, D24FS8.
        set(0x2004,0x00000600);  // RB_COLOR2_INFO: must not be read as depth.
        // Half-extents: x scale/offset 640, y scale -360 with offset 360.
        set(0x210F,0x44200000);set(0x2110,0x44200000);set(0x2111,0xC3B40000);set(0x2112,0x43B40000);
        set(0x2113,0xBF800000);set(0x2114,0x3F800000);
        set(0x2302,4);
        put(draw.deviceState,10368,SurfaceInfo);
        put(draw.deviceState,12816,Color);put(draw.deviceState,12832,Depth);
        draw.scissorRegisters=std::array<std::uint32_t,3>{0,0,(Height<<16)|Width};
        Gta5BoundSurfaceCapture bindings;
        bindings.surfaces[0].object=Color;bindings.surfaces[0].header=header(0);
        bindings.surfaces[4].object=Depth;bindings.surfaces[4].header=header(0,1);
        const auto refuses=[&](const Gta5InlineDraw& d,const NativeTranslatedDrawProgram& p,
                               const Gta5BoundSurfaceCapture& b,const char* reason) {
            const char* why=nullptr;
            expect(!planNativeStencilOnlyDraw(d,p,b,&why),reason);
            expect(why && std::string_view(why)==reason,reason);
        };
        {
            const char* why=nullptr;
            const auto plan=planNativeStencilOnlyDraw(draw,program,bindings,&why);
            expect(plan.has_value(),"captured stencil draw admitted");
            expect(plan->colorObject==Color && plan->depthObject==Depth,"both surfaces identified");
            expect(plan->width==Width && plan->height==Height,"extent from owned state");
            expect(plan->pitch==1280,"pitch from surface info");
            expect(plan->depthBase==0,"depth base from RB_DEPTH_INFO, not colour2");
            expect(plan->state.front.reference==32 && plan->state.front.readMask==255 && plan->state.front.writeMask==255,
                "stencil words are not blend controls");
            expect(plan->minDepth==1 && plan->maxDepth==0 && plan->cull==0 && !plan->clockwise,
                "captured reverse depth and two-sided raster state");
            expect(plan->state.stencilEnable && !plan->state.depthWrite,"stencil only");
        }
        // The absent pixel stage, again at the plan.
        {auto d=draw;d.pixelShader=0x1005b590;refuses(d,program,bindings,"pixel-stage-present");}
        {auto d=draw;Gta5InlineDraw::Texture x;x.slot=0;d.textures.push_back(x);
         refuses(d,program,bindings,"textures-bound");}
        // Colour must be masked off, and stencil must actually be the output.
        {auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(0x2104),0x0F);
         refuses(d,program,bindings,"colour-mask");}
        {auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(0x2200),0x00720212);
         refuses(d,program,bindings,"stencil-disabled");}
        {auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(0x2200),0x00720217);
         refuses(d,program,bindings,"depth-writes-enabled");}
        // A face that keeps on every path, or writes through a zero mask, is
        // not an output however enabled the stencil test is.
        {auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(0x2200),0x00000203);
         refuses(d,program,bindings,"stencil-writes-nothing");}
        {auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(0x210D),0x00000001);
         refuses(d,program,bindings,"stencil-writes-nothing");}
        {auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(0x2201),0xFFFFFFFFu);
         expect(planNativeStencilOnlyDraw(d,program,bindings).has_value(),"masked blend word is not stencil state");}
        {auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(0x2200),0x00720293u);
         put(d.deviceState,*gta5DrawRegisterOffset(0x210C),0x00ABCD12u);
         const auto p=planNativeStencilOnlyDraw(d,program,bindings);
         expect(p && p->state.back.reference==0x12 && p->state.back.readMask==0xCD && p->state.back.writeMask==0xAB,
             "two-sided stencil reads actual backface stencil word, not blend1");}
        for(unsigned r:{0x2204u,0x2205u,0x2206u}) {
            auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(r),0);
            refuses(d,program,bindings,"raster-state");
        }
        for(unsigned bits:{0x7FC00000u,0x7F800000u,0xC0000000u}) {
            auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(0x2113),bits);
            refuses(d,program,bindings,"depth-viewport");
        }
        // Both surfaces are required, and they may not be the same one.
        {auto b=bindings;b.surfaces[4].header.reset();refuses(draw,program,b,"depth-header");}
        {auto b=bindings;b.surfaces[4].object=0;refuses(draw,program,b,"depth-register");}
        {auto b=bindings;b.surfaces[0].object=0;refuses(draw,program,b,"colour-register");}
        {auto d=draw;put(d.deviceState,12832,Color);auto b=bindings;b.surfaces[4].object=Color;
         refuses(d,program,b,"depth-aliases-colour");}
        {auto d=draw;put(d.deviceState,12816+4,0x10270000);
         auto b=bindings;b.surfaces[1].object=0x10270000;refuses(d,program,b,"extra-slot-bound");}
        // Only the captured D24FS8 band; D24S8 also has stencil but is unproven.
        {auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(0x2002),0);
         refuses(d,program,bindings,"depth-format");}
        {auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(0x2004),0xFFFFFFFFu);
         expect(planNativeStencilOnlyDraw(d,program,bindings).has_value(),"masked colour2 cannot alter depth admission");}
        {auto b=bindings;b.surfaces[4].header=header(80,1);refuses(draw,program,b,"depth-header-register");}
        {auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(0x2002),0x10800u);
         refuses(d,program,bindings,"depth-info");}
        {auto b=bindings;put(*b.surfaces[4].header,36,((Width-1)<<18)|((384-1)<<3));
         refuses(draw,program,b,"depth-dimensions");}
        {auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(0x2002),0x10600u);
         auto b=bindings;b.surfaces[4].header=header(1536,1);
         refuses(d,program,b,"depth-edram-wrap");}
        {auto b=bindings;b.surfaces[4].header=header(0,1);b.surfaces[4].header->at(24)=0;
         refuses(draw,program,b,"depth-surface-info");}
        // Topology: a triangle list, whole triangles only.
        {auto d=draw;d.primitive=8;refuses(d,program,bindings,"program-profile");}
        {auto d=draw;d.vertexCount=35;d.vertices.assign(35*36,0);refuses(d,program,bindings,"program-profile");}
        // Binned surfaces are a different pass.
        {auto d=draw;put(d.deviceState,10368,SurfaceInfo|0x10000u);
         put(d.deviceState,*gta5DrawRegisterOffset(0x2000),SurfaceInfo|0x10000u);
         refuses(d,program,bindings,"surface-samples");}
    }

    std::printf("NATIVE_STENCIL_ONLY_PASS absent_pixel_stage_required=1 stencil_is_the_output=1 "
        "neighbouring_bands_excluded=4 cases=%u gpu=0 gameplay=0\n",passed);
    return 0;
}
