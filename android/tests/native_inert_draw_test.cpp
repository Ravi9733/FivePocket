// ABI21 inert rectangle admission. This draw is admitted because it provably
// writes NOTHING: no pixel shader, no colour channel, no depth, no stencil. The
// register values below are the ones the device actually recorded for it, so
// these cases pin that each of those four is required, and that re-enabling ANY
// single one of them takes the admission away again.
#include "native_inert_draw_plan.h"
#include <cassert>
#include <cstdio>
#include <string_view>
using namespace recomp::gpu;

namespace {
void put(std::vector<std::uint8_t>& v,unsigned at,std::uint32_t word) {
    for(unsigned b=0;b<4;++b)v[at+b]=static_cast<std::uint8_t>(word>>(24-8*b));
}
// Exactly as captured: a 640x16 band, colour mask 0, depth control with stencil
// disabled (bit0) and depth writes disabled (bit2), ZFUNC ALWAYS.
constexpr std::uint32_t CapturedDepthControl=0x00700772;
Gta5InlineDraw makeDraw() {
    Gta5InlineDraw d;
    d.primitive=8;d.vertexCount=3;d.stride=24;d.vertices.assign(3*24,0);
    d.deviceState.assign(13860,0);
    const auto set=[&](unsigned r,std::uint32_t v){put(d.deviceState,*gta5DrawRegisterOffset(r),v);};
    constexpr std::uint32_t regs[][2]{
        {0x2104,0},                    // RB_COLOR_MASK: no colour channel written.
        {0x2200,CapturedDepthControl}, // RB_DEPTHCONTROL: no depth, no stencil.
        {0x210F,0x43A00000},{0x2110,0x43A00000},   // half-extent 320 -> width 640
        {0x2111,0xC1000000},{0x2112,0x41000000},   // half-extent 8   -> height 16
        {0x2302,4}};
    for(const auto& r:regs)set(r[0],r[1]);
    d.scissorRegisters=std::array<std::uint32_t,3>{0,0,(16u<<16)|640u};
    return d;
}
NativeTranslatedDrawProgram makeProgram() {
    NativeTranslatedDrawProgram p;
    p.abiVersion=21;p.depthOnlyRectangle=true;p.indexed=false;p.stride=24;p.interpolators=0;
    p.textureSlot=UINT32_MAX;p.vertexIdentity={1,2,3,4};
    return p;
}
unsigned passed=0;
void expect(bool condition,const char* what) {
    if(!condition){std::fprintf(stderr,"INERT_DRAW_FAIL %s\n",what);std::abort();}
    ++passed;
}
void refuses(const Gta5InlineDraw& d,const NativeTranslatedDrawProgram& p,const char* reason) {
    const char* why=nullptr;
    expect(!planNativeInertRectangleDraw(d,p,&why),reason);
    expect(why && std::string_view(why)==reason,reason);
}
}

int main() {
    const auto draw=makeDraw();const auto program=makeProgram();
    {
        const char* why=nullptr;
        expect(planNativeInertRectangleDraw(draw,program,&why),"captured inert draw admitted");
        expect(why && std::string_view(why)=="inert","admission reports inert");
    }

    // Each of the four writes, re-enabled on its own, withdraws the admission.
    {auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(0x2104),15);
     refuses(d,program,"colour-writes-enabled");}
    {auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(0x2104),1);
     refuses(d,program,"colour-writes-enabled");} // one channel is still a write
    {auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(0x2200),CapturedDepthControl|0x4u);
     refuses(d,program,"depth-writes-enabled");}
    {auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(0x2200),CapturedDepthControl|0x1u);
     refuses(d,program,"stencil-enabled");}

    // The program must be a genuine ABI21 depth-only program. A program that
    // carries a pixel stage is not one, however the device state reads.
    {auto p=program;p.pixelCode={0x07230203,9};refuses(draw,p,"program-contract");}
    {auto p=program;p.vertexCode={0x07230203,1};refuses(draw,p,"program-contract");}
    {auto p=program;p.abiVersion=20;refuses(draw,p,"program-contract");}
    {auto p=program;p.depthOnlyRectangle=false;refuses(draw,p,"program-contract");}
    {auto p=program;p.interpolators=1;refuses(draw,p,"program-contract");}

    // The draw's own shape is pinned, including agreement with the program.
    {auto d=draw;d.primitive=4;refuses(d,program,"rectangle-shape");}
    {auto d=draw;d.vertexCount=0;d.vertices.clear();refuses(d,program,"rectangle-shape");}
    {auto d=draw;d.vertexCount=4;d.vertices.assign(4*24,0);refuses(d,program,"rectangle-shape");}
    {auto p=program;p.stride=40;refuses(draw,p,"rectangle-shape");}
    // Geometry is not part of the proof: the vertex bytes are unreadable on the
    // device for this draw, and cannot make a no-output draw produce output.
    {auto d=draw;d.vertices.clear();
     const char* why=nullptr;
     expect(planNativeInertRectangleDraw(d,program,&why),"absent vertices still admitted");}
    {auto d=draw;Gta5InlineDraw::Texture t;t.slot=0;d.textures={t};
     refuses(d,program,"unexpected-textures");}
    {auto d=draw;d.deviceState.assign(13859,0);refuses(d,program,"device-state-extent");}

    // A coherent draw is still required; malformed state is not admitted.
    {auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(0x210F),0);
     refuses(d,program,"viewport");}
    {auto d=draw;d.scissorRegisters.reset();refuses(d,program,"scissor");}

    // Six original vertices are two rectangles, and remain admissible.
    {auto d=draw;d.vertexCount=6;d.vertices.assign(6*24,0);
     const char* why=nullptr;
     expect(planNativeInertRectangleDraw(d,program,&why),"two rectangles admitted");}

    std::printf("NATIVE_INERT_DRAW_PASS provably_no_output=1 each_write_rechecked=4 "
        "cases=%u gpu=0 presentation=0 gameplay=0\n",passed);
    return 0;
}
