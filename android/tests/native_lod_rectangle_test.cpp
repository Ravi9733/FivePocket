#include "native_lod_rectangle_plan.h"
#include <cassert>
#include <cstdio>
#include <limits>
using namespace recomp::gpu;
int main() {
    Gta5InlineDraw d;d.primitive=8;d.vertexCount=3;d.stride=24;d.vertices.resize(72);d.deviceState.resize(13860);
    const auto put=[](auto& b,unsigned at,unsigned v){for(unsigned i=0;i<4;++i)b[at+i]=v>>(24-8*i);};
    const auto set=[&](unsigned r,unsigned v){put(d.deviceState,*gta5DrawRegisterOffset(r),v);};
    constexpr unsigned regs[][2]{{0x2001,0},{0x200F,0x20002000},{0x2100,0xFFFFFF},{0x2104,15},
        {0x2113,0xBF800000},{0x2114,0x3F800000},{0x2180,0x10010201},{0x2181,4},{0x2182,0x10001},
        {0x2200,0x700770},{0x2201,0x10001},{0x2202,0xAA000007},{0x2203,0x10},
        {0x2204,0x80000},{0x2205,0x18000},{0x2206,0x43F},{0x2208,4},{0x2302,4},
        {0x2303,0x40000000},{0x2304,0x3F800000},{0x2305,0x40000000},{0x2306,0x3F800000},
        {0x2307,0xFF000},{0x2308,0xFF100},{0x2312,0xFFFF}};
    for(auto& r:regs)set(r[0],r[1]);
    d.vertexConstants[251*4]=0xBF800000;d.vertexConstants[251*4+1]=0x40000000;
    d.pixelConstants[253*4]=0x3F800000;
    constexpr unsigned constants[]{0x3F400000,0x3E800000,0x3F000000,0xBF000000,0x40800000,0x41000000,0,0xC0000000};
    for(unsigned i=0;i<8;++i)d.pixelConstants[254*4+i]=constants[i];
    Gta5BoundSurfaceCapture b;b.surfaces[0].object=0x1000;b.surfaces[0].header=std::vector<std::uint8_t>(40);
    put(d.deviceState,12816,0x1000);b.surfaces[4].object=0x2000;put(d.deviceState,12832,0x2000);
    NativeTranslatedDrawProgram p;p.abiVersion=9;p.explicitLodRectangle=true;p.stride=24;p.interpolators=1;p.textureSlot=5;p.textureSlots={5};
    NativeMipTextureCapture t;
    const auto check=[&]{return planNativeLodRectangleDraw(d,p,b,t);};unsigned cases=0;
    for(unsigned baseWidth:{128u,256u,512u,1024u})for(unsigned level=1;level<=3;++level) {
        const unsigned baseHeight=baseWidth/2,width=baseWidth>>level,height=baseHeight>>level,pitch=(baseWidth+79)/80*80;
        t.fetch={0x80000002u|(baseWidth/32<<22)|(2u<<10)|(2u<<13),0x10000086,
            (baseWidth-1)|((baseHeight-1)<<13),0xA80E14,195,0x20000210};
        const auto chain=planXenosUnpackedMipChain(t.fetch);assert(chain);
        t.base.resize(chain->baseBytes);t.mips.resize(chain->mipBytes);
        set(0x2000,0x08000000|pitch);put(*b.surfaces[0].header,24,0x08000000|pitch);
        put(*b.surfaces[0].header,36,((baseWidth-1)<<18)|((baseHeight-1)<<3));
        d.scissorRegisters=std::array<unsigned,3>{0,0,(baseHeight<<16)|baseWidth};
        set(0x210F,std::bit_cast<unsigned>(float(baseWidth)/2));set(0x2110,std::bit_cast<unsigned>(float(baseWidth)/2));
        set(0x2111,std::bit_cast<unsigned>(-float(baseHeight)/2));set(0x2112,std::bit_cast<unsigned>(float(baseHeight)/2));
        d.pixelConstants[229*4]=std::bit_cast<unsigned>(float(level));d.pixelConstants[229*4+3]=std::bit_cast<unsigned>(1.f/width);
        const float scale=1.f/(1u<<level);
        const std::array<std::array<float,4>,3> v{{{0,1,0,0},{scale,1,1,0},{0,1-scale,0,1}}};
        for(unsigned i=0;i<3;++i) {
            put(d.vertices,i*24+12,0x7FC00000); // Unused padding must not reject.
            for(unsigned c=0;c<4;++c)put(d.vertices,i*24+(c<2?c*4:16+(c-2)*4),std::bit_cast<unsigned>(v[i][c]));
        }
        const auto plan=check();assert(plan && plan->width==width && plan->height==height && plan->outputMip==level);
        const std::vector<std::array<float,4>> positions{{-1,1,0,1},{2*scale-1,1,0,1},{-1,1-2*scale,0,1}};
        assert(nativeLodRectanglePositions(d,*plan,positions));assert(!nativeFullRectangleList(d,positions));
        auto badPlan=*plan;badPlan.outputMip=32;assert(!nativeLodRectanglePositions(d,badPlan,positions));
        auto badPositions=positions;std::swap(badPositions[1],badPositions[2]);assert(!nativeLodRectanglePositions(d,*plan,badPositions));
        const auto valid=d;const auto validT=t;const auto validP=p;
        for(unsigned at:{229u*4,229u*4+3,253u*4,254u*4,254u*4+1,255u*4+3}) {
            d.pixelConstants[at]=0x7FC00000;assert(!check());d=valid;
        }
        for(unsigned offset:{0u,4u,16u,20u,24u,28u,40u,44u,48u,52u,64u,68u}) {
            put(d.vertices,offset,0x7FC00000);assert(!check());d=valid;
        }
        for(const auto& r:regs){set(r[0],r[1]^1);assert(!check());d=valid;}
        t.base.pop_back();assert(!check());t=validT;t.mips.pop_back();assert(!check());t=validT;
        t.fetch[3]^=1u<<21;assert(!check());t=validT;
        p.explicitLodRectangle=false;assert(!check());p=validP;p.abiVersion=8;assert(!check());p=validP;
        auto header=*b.surfaces[0].header;b.surfaces[0].header->pop_back();assert(!check());b.surfaces[0].header=header;
        // Same geometry/ownership planner, DIFFERENT original literal bank.
        // These are related-domain component cases, not actual later variants.
        p.abiVersion=10;assert(!check());
        constexpr unsigned predicatedConstants[][2]{{229*4+2,0x3F800000},{252*4,0x3F800000},
            {252*4+1,0x40800000},{252*4+2,0x3DE38E39},
            {253*4,0xBF000000},{253*4+1,0xC0000000},{253*4+2,0x40800000},{253*4+3,0x41000000},
            {254*4,0x3F400000},{254*4+1,0x3E800000},{254*4+2,0x3F000000},{254*4+3,0x40800000},
            {255*4,0x3F000000},{255*4+1,0xC0000000},{255*4+2,0xBF800000},{255*4+3,0}};
        for(const auto& c:predicatedConstants)d.pixelConstants[c[0]]=c[1];
        assert(!check());set(0x2180,0x10010901);
        assert(check());const auto predicatedValid=d;
        for(const auto& c:predicatedConstants) {
            d.pixelConstants[c[0]]^=1;assert(!check());d=predicatedValid;
        }
        d.pixelConstants[252*4+3]=0x7FC00000;assert(check()); // Unread literal lane.
        set(0x2180,0x10010801);assert(!check());set(0x2180,0x10010901);
        p.abiVersion=9;assert(!check());p.abiVersion=11;assert(!check());
        constexpr unsigned wideConstants[]{0x3E800000,0x3F400000,0xBF800000,0x3D23D70A,
            0x3F800000,0x40800000,0x41000000,0x3F000000,0,0xBF000000,0xC0000000,0x40800000,
            0x3F000000,0xC0800000,0xC0000000,0,0x41000000,0x3E800000,0x3F000000,0xC0000000};
        for(unsigned i=0;i<20;++i)d.pixelConstants[251*4+i]=wideConstants[i];
        set(0x2180,0x10011401);
        if(baseWidth==512 && level==3) {
            assert(check());const auto wideValid=d;
            for(unsigned i=0;i<20;++i){d.pixelConstants[251*4+i]^=1;assert(!check());d=wideValid;}
            set(0x2180,0x10010901);assert(!check());d=wideValid;
            d.pixelConstants[248*4]=0x7FC00000;assert(check()); // Unread literal range.
            p.abiVersion=10;assert(!check());p.abiVersion=11;
        } else assert(!check());
        p.abiVersion=12;assert(!check());
        d=valid;p=validP;
        ++cases;
    }
    std::printf("NATIVE_LOD_RECTANGLE_PASS dimensions_mips=%u numeric_domain_state_texture_and_position_negatives=1 original_padding_ignored=1 separate_predicated_literal_bank=1\n",cases);
}
