// ABI20 uses the current owned surface, even when its header lives on the
// executing worker's stack. No cached-target substitution is allowed.
#include "native_composite_rectangle_plan.h"
#include <cassert>
#include <cstdio>
#include <string>
using namespace recomp::gpu;

namespace {
constexpr std::uint32_t CurrentTarget=0x640af9e0;
constexpr std::uint32_t TileCacheTarget=0x102718b0;
constexpr std::uint32_t SurfaceInfo=0x14000500;   // pitch 1280, one sample.
constexpr std::uint32_t ColorInfo=0;             // Current base tile 0.

void put(std::vector<std::uint8_t>& v,unsigned at,std::uint32_t word) {
    for(unsigned b=0;b<4;++b)v[at+b]=static_cast<std::uint8_t>(word>>(24-8*b));
}
std::vector<std::uint8_t> header() {
    std::vector<std::uint8_t> h(40,0);put(h,24,SurfaceInfo);put(h,28,ColorInfo);
    put(h,36,((1280u-1)<<18)|((32u-1)<<3));return h;
}
Gta5InlineDraw makeDraw() {
    Gta5InlineDraw d;
    d.primitive=8;d.vertexCount=3;d.stride=24;d.vertices.assign(3*24,0);
    d.deviceState.assign(13860,0);
    const auto set=[&](unsigned r,std::uint32_t v){put(d.deviceState,*gta5DrawRegisterOffset(r),v);};
    // A 1280x32 band: half-extents 640 and 16 in the viewport scale/offset.
    constexpr std::uint32_t regs[][2]{{0x2000,SurfaceInfo},{0x2001,ColorInfo},{0x2104,15},
        {0x210F,0x44200000},{0x2110,0x44200000},{0x2111,0xC1800000},{0x2112,0x41800000},
        {0x2113,0xBF800000},{0x2114,0x3F800000},{0x2302,4},
        {0x2180,0x10010000},{0x2181,4},{0x2182,0x10001},
        {0x2200,0x700770},{0x2201,0x10001},{0x2202,0xAA000007},{0x2203,0x714},
        {0x2204,0x80000},{0x2205,0x18000},{0x2206,0x43F},
        {0x2307,0xFF000},{0x2308,0xFF100},{0x2312,0xFFFF}};
    for(const auto& r:regs)set(r[0],r[1]);
    put(d.deviceState,12816,CurrentTarget);
    put(d.deviceState,13104,TileCacheTarget);
    put(d.deviceState,10368,SurfaceInfo);
    d.scissorRegisters=std::array<std::uint32_t,3>{0,0,(32u<<16)|1280u};
    Gta5InlineDraw::Texture t;t.slot=2;d.textures={t};
    return d;
}
NativeTranslatedDrawProgram makeProgram() {
    NativeTranslatedDrawProgram p;
    p.abiVersion=20;p.compositeRectangle=true;p.stride=24;p.interpolators=1;
    p.textureSlot=2;p.textureSlots={2};p.indexed=false;
    p.vertexCode={1};p.pixelCode={1};
    return p;
}
Gta5BoundSurfaceCapture makeBindings(const Gta5InlineDraw& d) {
    Gta5BoundSurfaceCapture b;
    const auto word=[&](unsigned at) {
        return (std::uint32_t{d.deviceState[at]}<<24)|(std::uint32_t{d.deviceState[at+1]}<<16)|
            (std::uint32_t{d.deviceState[at+2]}<<8)|d.deviceState[at+3];
    };
    for(unsigned i=0;i<5;++i){b.surfaces[i].object=word(12816+4*i);b.surfaces[i].tileCacheObject=word(13104+4*i);}
    b.surfaces[0].header=header();b.surfaces[0].tileCacheHeader=header();
    put(*b.surfaces[0].tileCacheHeader,28,384);
    put(*b.surfaces[0].tileCacheHeader,36,((1280u-1)<<18)|((384u-1)<<3));
    return b;
}
}

int main() {
    const auto draw=makeDraw();const auto program=makeProgram();const auto bindings=makeBindings(draw);
    const auto plan=planNativeCompositeRectangleDraw(draw,program,bindings);
    assert(plan);
    assert(plan->object==CurrentTarget && plan->width==1280 && plan->height==32);
    assert(plan->pitch==1280 && plan->baseTile==0 && plan->tileCount==32);
    {std::array<std::array<float,4>,3> pos{{{-1,-1,0,1},{1,-1,0,1},{-1,1,0,1}}};
     assert(nativeCompositeCompletedRows(draw,*plan,pos)==32);
     pos[0][1]=pos[1][1]=0;assert(nativeCompositeCompletedRows(draw,*plan,pos)==16);
     assert(!nativeFullRectangleList(draw,pos));
     pos[0][1]=pos[1][1]=0.0625f;assert(!nativeCompositeCompletedRows(draw,*plan,pos));
     pos[0][1]=pos[1][1]=0;pos[2][1]=0.5f;assert(!nativeCompositeCompletedRows(draw,*plan,pos));}
    // Unbinding depth preserves an INACTIVE HiZ address, not its enable bits.
    {auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(0x2203),0x00A00714);
     assert(planNativeCompositeRectangleDraw(d,program,makeBindings(d)));}
    {auto d=draw;put(d.deviceState,10372,384);const char* why=nullptr;
     assert(!planNativeCompositeRectangleDraw(d,program,makeBindings(d),&why));
     assert(std::string(why)=="colour-register-header-mismatch");}

    // Missing current headers never fall back to a cache, even if its register
    // word happens to agree. Cached bytes have no authority on this path.
    {auto b=bindings;b.surfaces[0].header.reset();assert(!planNativeCompositeRectangleDraw(draw,program,b));}
    {auto b=bindings;b.surfaces[0].header->resize(32);assert(!planNativeCompositeRectangleDraw(draw,program,b));}
    {auto b=bindings;b.surfaces[0].tileCacheHeader.reset();assert(planNativeCompositeRectangleDraw(draw,program,b));}
    // No second bound colour slot.
    {auto b=bindings;b.surfaces[1].object=0x10271930;assert(!planNativeCompositeRectangleDraw(draw,program,b));}
    // Disabled depth does not consume a binding. Enabling depth without the
    // captured paired HiZ/owned-previous-band proof must still reject.
    {auto d=draw;put(d.deviceState,12832,0x640afa10);auto b=makeBindings(d);
     assert(planNativeCompositeRectangleDraw(d,program,b));
     b.surfaces[4].header=header();assert(planNativeCompositeRectangleDraw(d,program,b));
     put(d.deviceState,*gta5DrawRegisterOffset(0x2200),0x700772);
     assert(!planNativeCompositeRectangleDraw(d,program,b));}
    // Original later iterations enable ALWAYS, never depth/stencil writes.
    {auto d=draw;put(d.deviceState,12832,0x640afa10);auto b=makeBindings(d);
     put(d.deviceState,10372,32);put(*b.surfaces[0].header,28,32);
     b.surfaces[4].header=header();put(*b.surfaces[4].header,28,0x10000);
     put(*b.surfaces[4].header,32,0x11);
     put(d.deviceState,10376,0x10000);d.deviceState[11070]=0x24;
     put(d.deviceState,*gta5DrawRegisterOffset(0x2200),0x700772);
     put(d.deviceState,*gta5DrawRegisterOffset(0x2203),0x715);
     const auto accepted=planNativeCompositeRectangleDraw(d,program,b);
     assert(accepted && accepted->baseTile==32 && accepted->tileCount==32);
     for(unsigned compare=0;compare<7;++compare){auto bad=d;
         put(bad.deviceState,*gta5DrawRegisterOffset(0x2200),0x700702|(compare<<4));
         assert(!planNativeCompositeRectangleDraw(bad,program,b));}
     for(unsigned bit:{0u,2u,7u}){auto bad=d;
         put(bad.deviceState,*gta5DrawRegisterOffset(0x2200),0x700772|(1u<<bit));
         assert(!planNativeCompositeRectangleDraw(bad,program,b));}
     for(unsigned hiz:{0x714u,0x717u,0x711u,0u}){auto bad=d;
         put(bad.deviceState,*gta5DrawRegisterOffset(0x2203),hiz);
         assert(!planNativeCompositeRectangleDraw(bad,program,b));}
     {auto bad=d;bad.deviceState[11070]&=~0x20;assert(!planNativeCompositeRectangleDraw(bad,program,b));}
     {auto later=d;auto bound=b;put(later.deviceState,10372,64);put(*bound.surfaces[0].header,28,64);
      put(later.deviceState,10376,0x10020);put(*bound.surfaces[4].header,28,0x10020);
      put(*bound.surfaces[4].header,32,0x00A00011);
      assert(!planNativeCompositeRectangleDraw(later,program,bound)); // stale HiZ address
      put(later.deviceState,*gta5DrawRegisterOffset(0x2203),0x00A00715);
      assert(planNativeCompositeRectangleDraw(later,program,bound));}
     {auto bad=b;bad.surfaces[4].header.reset();assert(!planNativeCompositeRectangleDraw(d,program,bad));}
     {auto bad=b;bad.surfaces[4].header->resize(32);assert(!planNativeCompositeRectangleDraw(d,program,bad));}
     for(unsigned at:{24u,28u,32u,36u}){auto bad=b;put(*bad.surfaces[4].header,at,0);
         assert(!planNativeCompositeRectangleDraw(d,program,bad));}
     {auto bad=d;put(bad.deviceState,10376,0x10020);assert(!planNativeCompositeRectangleDraw(bad,program,b));}
     {auto bad=b;auto next=d;put(*bad.surfaces[4].header,28,0x10020);put(next.deviceState,10376,0x10020);
         assert(!planNativeCompositeRectangleDraw(next,program,bad));}
     {auto bad=b;auto next=d;bad.surfaces[4].object=0;put(next.deviceState,12832,0);
         assert(!planNativeCompositeRectangleDraw(next,program,bad));}}
    // Bindings must agree with the state they were captured from.
    {auto b=bindings;b.surfaces[0].object=0x11112220;assert(!planNativeCompositeRectangleDraw(draw,program,b));}
    {auto b=bindings;b.surfaces[0].tileCacheObject=0x10271930;assert(!planNativeCompositeRectangleDraw(draw,program,b));}
    // The current header must corroborate the live surface register.
    {auto d=draw;put(d.deviceState,10368,0x14000440);assert(!planNativeCompositeRectangleDraw(d,program,makeBindings(d)));}

    // Program identity: neither the version nor the flag alone is sufficient.
    {auto p=program;p.abiVersion=8;assert(!planNativeCompositeRectangleDraw(draw,p,bindings));}
    {auto p=program;p.compositeRectangle=false;assert(!planNativeCompositeRectangleDraw(draw,p,bindings));}
    {auto p=program;p.cubeRectanglePwl=true;assert(!planNativeCompositeRectangleDraw(draw,p,bindings));}
    {auto p=program;p.fixed16RectangleDepth=true;assert(!planNativeCompositeRectangleDraw(draw,p,bindings));}
    {auto p=program;p.indexed=true;assert(!planNativeCompositeRectangleDraw(draw,p,bindings));}
    {auto p=program;p.directTriangles=true;assert(!planNativeCompositeRectangleDraw(draw,p,bindings));}
    {auto p=program;p.interpolators=2;assert(!planNativeCompositeRectangleDraw(draw,p,bindings));}
    {auto p=program;p.textureSlot=3;assert(!planNativeCompositeRectangleDraw(draw,p,bindings));}

    // Geometry: rectangle_list triples only, and the stride the program declares.
    {auto d=draw;d.vertexCount=4;d.vertices.assign(4*24,0);assert(!planNativeCompositeRectangleDraw(d,program,makeBindings(d)));}
    {auto d=draw;d.primitive=4;assert(!planNativeCompositeRectangleDraw(d,program,makeBindings(d)));}
    {auto d=draw;d.stride=28;assert(!planNativeCompositeRectangleDraw(d,program,makeBindings(d)));}
    {auto d=draw;d.textures.clear();assert(!planNativeCompositeRectangleDraw(d,program,makeBindings(d)));}
    {auto d=draw;d.textures[0].literalZero=true;assert(!planNativeCompositeRectangleDraw(d,program,makeBindings(d)));}
    // Binning/MSAA is not this draw.
    {auto d=draw;d.deviceState[11068]=0x10;assert(!planNativeCompositeRectangleDraw(d,program,makeBindings(d)));}
    // A scissor that does not start at the origin is not this band.
    {auto d=draw;d.scissorRegisters=std::array<std::uint32_t,3>{1,0,(32u<<16)|1280u};
     assert(!planNativeCompositeRectangleDraw(d,program,makeBindings(d)));}

    {auto b=bindings;put(*b.surfaces[0].header,36,((1280u-1)<<18)|((384u-1)<<3));
     assert(!planNativeCompositeRectangleDraw(draw,program,b));}
    for(unsigned r:{0x2104u,0x2180u,0x2200u,0x2201u,0x2203u,0x2204u,0x2205u,0x2206u}) {
        auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(r),0);
        assert(!planNativeCompositeRectangleDraw(d,program,makeBindings(d)));
    }
    {auto d=draw;d.deviceState[11068]=0x20;assert(!planNativeCompositeRectangleDraw(d,program,makeBindings(d)));}
    // ABI26 is explicit: generated r0 XY and position-only VS. The original
    // ABI20 profile still rejects this state even if its texture is readable.
    {auto d=draw;auto b=bindings;auto p=program;
     p.abiVersion=26;p.screenSpaceRectangle=true;p.interpolators=0;
     put(d.deviceState,*gta5DrawRegisterOffset(0x2180),0x10050100);
     put(d.deviceState,*gta5DrawRegisterOffset(0x2181),2);
     put(d.deviceState,*gta5DrawRegisterOffset(0x2182),0);
     put(d.deviceState,*gta5DrawRegisterOffset(0x2202),0xAA000005);
     put(d.deviceState,*gta5DrawRegisterOffset(0x2203),0x10);
     put(d.deviceState,*gta5DrawRegisterOffset(0x2111),0xC3B40000);
     put(d.deviceState,*gta5DrawRegisterOffset(0x2112),0x43B40000);
     d.scissorRegisters=std::array<std::uint32_t,3>{0,0,(720u<<16)|1280u};
     put(*b.surfaces[0].header,36,((1280u-1)<<18)|((720u-1)<<3));
     const auto screen=planNativeCompositeRectangleDraw(d,p,b);
     assert(validateNativeScreenSpaceRectangleProgram(p) && screen && screen->height==720 && screen->tileCount==720);
     assert(!planNativeCompositeRectangleDraw(d,program,b));
     for(unsigned bit:{18u,19u}){auto bad=d;put(bad.deviceState,*gta5DrawRegisterOffset(0x2180),0x10050100^(1u<<bit));
         assert(!planNativeCompositeRectangleDraw(bad,p,b));}
     for(unsigned value:{0u,0x102u,0x12u}){auto bad=d;put(bad.deviceState,*gta5DrawRegisterOffset(0x2181),value);
         assert(!planNativeCompositeRectangleDraw(bad,p,b));}
     {auto bad=d;auto bound=b;put(bad.deviceState,12832,0x12340);bound.surfaces[4].object=0x12340;
      assert(!planNativeCompositeRectangleDraw(bad,p,bound));}
     {auto bad=p;bad.screenSpaceRectangle=false;assert(!planNativeCompositeRectangleDraw(d,bad,b));}
     {auto bad=p;bad.abiVersion=20;assert(!planNativeCompositeRectangleDraw(d,bad,b));}
     {auto bad=p;bad.interpolators=1;assert(!planNativeCompositeRectangleDraw(d,bad,b));}
     {auto bad=p;bad.geometryParameterRegister=0;assert(!validateNativeScreenSpaceRectangleProgram(bad));}
     {auto bad=p;bad.rectangleMrt=true;assert(!validateNativeScreenSpaceRectangleProgram(bad));}
     {auto bad=p;bad.geometryDepth20e4=true;assert(!validateNativeScreenSpaceRectangleProgram(bad));}
     for(unsigned reg:{0x2104u,0x2182u,0x2200u,0x2201u,0x2202u,0x2203u,0x2205u}) {
         auto bad=d;const auto current=*gta5DrawRegister(bad,reg);
         put(bad.deviceState,*gta5DrawRegisterOffset(reg),current^1u);
         assert(!planNativeCompositeRectangleDraw(bad,p,b));}
     std::puts("NATIVE_SCREEN_RECTANGLE_PLAN_PASS generated_r0_xy=1 missing_state_refused=1 old_abi_unchanged=1 gpu=0");}
    printf("NATIVE_COMPOSITE_RECTANGLE_PASS current_target=1 cache_substitution_rejected=1 colour_register_mismatch_rejected=1 disabled_depth_binding=1 always_depth_previous_band=1 writes_stencil_non_always_rejected=1 gpu=0\n");
    return 0;
}
