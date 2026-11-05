#include "native_cube_rectangle_plan.h"
#include <bit>
#include <cassert>
#include <cstdio>
#include <limits>
using namespace recomp::gpu;
int main() {
    Gta5InlineDraw d;d.primitive=8;d.vertexCount=6;d.stride=24;d.vertices.resize(144);d.deviceState.resize(13860);
    d.scissorRegisters=std::array<unsigned,3>{0,0,0x1000200};
    const auto put=[](auto& b,unsigned at,unsigned v){for(unsigned i=0;i<4;++i)b[at+i]=v>>(24-8*i);};
    const auto set=[&](unsigned r,unsigned v){put(d.deviceState,*gta5DrawRegisterOffset(r),v);};
    d.pixelConstants[1020]=0x3FC00000;d.pixelConstants[1021]=0x3F800000;
    for(unsigned i=0;i<6;++i){put(d.vertices,i*24+8,0x3F800000);put(d.vertices,i*24+12,0x7FC00000);}
    constexpr unsigned regs[][2]{{0x2000,0x8000230},{0x2001,0},{0x200F,0x20002000},{0x2100,0xFFFFFF},
        {0x2104,15},{0x210F,0x43800000},{0x2110,0x43800000},{0x2111,0xC3000000},{0x2112,0x43000000},
        {0x2113,0xBF800000},{0x2114,0x3F800000},{0x2180,0x10010101},{0x2181,4},{0x2182,0x10001},
        {0x2200,0x700770},{0x2201,0x10001},{0x2202,0xAA000007},{0x2203,0x10},
        {0x2204,0x80000},{0x2205,0x18000},{0x2206,0x43F},{0x2208,4},{0x2302,4},
        {0x2303,0x40000000},{0x2304,0x3F800000},{0x2305,0x40000000},{0x2306,0x3F800000},
        {0x2307,0xFF000},{0x2308,0xFF100},{0x2312,0xFFFF}};
    for(auto& r:regs)set(r[0],r[1]);
    Gta5BoundSurfaceCapture bindings;bindings.surfaces[0].object=0x1000;bindings.surfaces[0].header=std::vector<std::uint8_t>(40);
    put(*bindings.surfaces[0].header,24,0x8000230);put(*bindings.surfaces[0].header,36,(511<<18)|(255<<3));
    put(d.deviceState,12816,0x1000);bindings.surfaces[4].object=0x2000;put(d.deviceState,12832,0x2000);
    Gta5InlineDraw::Texture texture;texture.slot=4;
    texture.fetch={0x810248FE,0x10000086,0x140FE07F,0xA80C14,3,0x610};texture.source.resize(393216);d.textures={texture};
    NativeTranslatedDrawProgram p;p.abiVersion=8;p.cubeRectanglePwl=true;p.stride=24;p.interpolators=1;p.textureSlot=4;p.textureSlots={4};
    const auto check=[&]{return planNativeCubeRectangleDraw(d,p,bindings);};assert(check());
    assert((check()==NativeOffscreenDrawPlan{0x1000,512,256,560,0,112}));
    const std::vector<std::array<float,4>> original{{-1,1,0,1},{0,1,0,1},{-1,-1,0,1},{1,1,0,1},{0,1,0,1},{1,-1,0,1}};
    assert(nativeFullRectangleList(d,original));unsigned cases=0;
    // Actual next draw's geometry covers only the top-left mip rectangle.
    // Full replacement must continue to reject it, including every loop size.
    const auto fullDraw=d;
    d.vertexCount=3;d.vertices.resize(72);
    for(unsigned level=1;level<4;++level) {
        const unsigned width=512>>level,height=256>>level;
        const float right=float(width)/256-1,bottom=1-float(height)/128;
        const std::vector<std::array<float,4>> region{{-1,1,0,1},{right,1,0,1},{-1,bottom,0,1}};
        assert(nativeTopLeftRectangleList(d,region,width,height));
        assert(!nativeFullRectangleList(d,region));
        assert(!nativeTopLeftRectangleList(d,region,width+1,height));
        assert(!nativeTopLeftRectangleList(d,region,width,height+1));
        assert(!nativeTopLeftRectangleList(d,region,0,height));
        auto shifted=region;for(auto& p:shifted)p[0]+=.125f;
        assert(!nativeTopLeftRectangleList(d,shifted,width,height));
    }
    d=fullDraw;
    // The original 82369828 loop reduces the output dimensions per mip.
    // Exercise connected draw-state and coverage planning at all four sizes;
    // this is synthetic state coverage, not a claim these later draws ran live.
    const auto baseDraw=d;const auto baseBindings=bindings;
    for(unsigned mip=0;mip<4;++mip) {
        const auto width=512u>>mip,height=256u>>mip,pitch=(width+79)/80*80;
        d.scissorRegisters=std::array<unsigned,3>{0,0,(height<<16)|width};
        set(0x2000,0x08000000|pitch);
        put(*bindings.surfaces[0].header,24,0x08000000|pitch);
        put(*bindings.surfaces[0].header,36,((width-1)<<18)|((height-1)<<3));
        set(0x210F,std::bit_cast<unsigned>(float(width)/2));
        set(0x2110,std::bit_cast<unsigned>(float(width)/2));
        set(0x2111,std::bit_cast<unsigned>(-float(height)/2));
        set(0x2112,std::bit_cast<unsigned>(float(height)/2));
        assert((check()==NativeOffscreenDrawPlan{0x1000,width,height,pitch,0,pitch/80*((height+15)/16)}));
        assert(nativeFullRectangleList(d,original));
        put(*bindings.surfaces[0].header,36,((width-2)<<18)|((height-1)<<3));
        assert(!check()); // A differently sized producer header must not pass.
        d=baseDraw;bindings=baseBindings;
    }
    // Every source corner can be omitted, in either winding and arbitrary
    // order. Both halves still cover the target exactly once.
    const std::array<std::array<float,4>,4> left{{{-1,1,0,1},{0,1,0,1},{-1,-1,0,1},{0,-1,0,1}}};
    for(unsigned omitted=0;omitted<4;++omitted) {
        std::array<unsigned,3> indices{};unsigned n=0;for(unsigned i=0;i<4;++i)if(i!=omitted)indices[n++]=i;
        do {auto positions=original;for(unsigned i=0;i<3;++i)positions[i]=left[indices[i]];
            assert(nativeFullRectangleList(d,positions));++cases;
        }while(std::next_permutation(indices.begin(),indices.end()));
    }
    for(unsigned i=0;i<6;++i)for(unsigned c=0;c<4;++c) {
        for(float bad:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),1e20f}) {
            auto positions=original;positions[i][c]=bad;assert(!nativeFullRectangleList(d,positions));
        }
    }
    auto positions=original;positions.pop_back();assert(!nativeFullRectangleList(d,positions));
    positions=original;positions[1][0]=-.25f;assert(!nativeFullRectangleList(d,positions)); // Gap.
    positions=original;std::copy(positions.begin(),positions.begin()+3,positions.begin()+3);assert(!nativeFullRectangleList(d,positions)); // Overlap.
    positions=original;positions[0][3]=-1;assert(!nativeFullRectangleList(d,positions));
    positions=original;positions[0][0]+=.0001f;assert(!nativeFullRectangleList(d,positions));
    for(const auto& r:regs) {
        if(r[0]>=0x210F && r[0]<=0x2112)continue; // Geometry independently interprets these.
        set(r[0],r[1]^1);assert(!check());set(r[0],r[1]);
    }
    const auto valid=d;
    d.textures[0].source.pop_back();assert(!check());d=valid;
    d.textures[0].fetch[0]&=~0xFCu;assert(!check());d=valid;
    d.textures[0].literalZero=true;assert(!check());d=valid;
    d.vertexCount=5;assert(!check());d=valid;
    d.pixelConstants[1021]=0;assert(!check());d=valid;
    put(d.vertices,8,0);assert(!check());d=valid;
    put(d.vertices,16,0x7FC00000);assert(!check());d=valid;
    p.cubeRectanglePwl=false;assert(!check());p.cubeRectanglePwl=true;
    p.explicitLodRectangle=true;assert(!check());p.explicitLodRectangle=false;
    p.abiVersion=4;assert(!check());p.abiVersion=8;
    bindings.surfaces[0].header->pop_back();assert(!check());
    std::printf("NATIVE_CUBE_RECTANGLE_PASS mip_target_sizes=4 corner_permutations=%u gaps_overlap_nonfinite_and_state_negatives=1\n",cases);
}
