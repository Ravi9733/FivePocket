#include "indexed_lod_fixture.h"
#include <cstdio>
using namespace recomp::gpu;
using namespace recomp::gpu::test;
int main(int argc,char** argv) {
    assert(argc==2 || (argc==3 && (std::strcmp(argv[2],"--601")==0 || std::strcmp(argv[2],"--602")==0)));
    const auto profile=argc==3?NativeIndexedLodProfile::Indexed601:NativeIndexedLodProfile::Indexed600;
    const bool windows=argc==3 && std::strcmp(argv[2],"--602")==0;
    const auto raster=windows?NativeIndexedLodRaster::ClippedWindows:NativeIndexedLodRaster::LegacyBatches;
    IndexedLodFixture f(argv[1],profile);const char* why=nullptr;
    const auto plan=[&](const auto& draw,const auto& indices,const auto& bound,const auto& textures,bool idle) {
        return planNativeIndexedLod(draw,indices,bound,textures,idle,&why,profile,raster);
    };
    assert(plan(f.draw,f.indices,f.bound,f.textures,true));
    assert(!plan(f.draw,f.indices,f.bound,f.textures,false));
    const auto other=profile==NativeIndexedLodProfile::Indexed601?NativeIndexedLodProfile::Indexed600:NativeIndexedLodProfile::Indexed601;
    assert(!planNativeIndexedLod(f.draw,f.indices,f.bound,f.textures,true,&why,other));
    assert(!planNativeIndexedLod(f.draw,f.indices,f.bound,f.textures,true,&why,static_cast<NativeIndexedLodProfile>(99)));
    unsigned negatives=1;
    for(unsigned reg:{0x2000,0x2001,0x2002,0x2104,0x210D,0x210E,0x210F,0x2110,0x2111,0x2112,0x2113,0x2114,
        0x2180,0x2181,0x2182,0x2183,0x2184,0x2200,0x2201,0x2202,0x2203,0x2204,0x2205,0x2206,0x2207,0x2208,
        0x2284,0x2292,0x2293,0x2300,0x2301,0x2302,0x2307,0x2308,0x2312,0x231C,0x2324,0x2325}) {
        auto draw=f.draw;draw.deviceState[*gta5DrawRegisterOffset(reg)+3]^=1;
        assert(!plan(draw,f.indices,f.bound,f.textures,true));++negatives;
    }
    for(unsigned slot=0;slot<5;++slot)for(unsigned mode=0;mode<3;++mode) {
        auto textures=f.textures;
        if(mode==0)textures[slot].fetch[3]^=1;
        if(mode==1)textures[slot].base.pop_back();
        if(mode==2){if(textures[slot].mips.empty())textures[slot].mips.push_back(1);else textures[slot].mips.pop_back();}
        assert(!plan(f.draw,f.indices,f.bound,textures,true));++negatives;
    }
    for(unsigned kind=0;kind<13;++kind) {
        auto draw=f.draw;auto indices=f.indices;auto bound=f.bound;
        if(kind==0)draw.deviceState.pop_back();
        if(kind==1)draw.vertexCount=0;
        if(kind==2)draw.vertices.pop_back();
        if(kind==3)indices[0]=draw.vertexCount;
        if(kind==4)indices.push_back(0);
        if(kind==5)draw.scissorRegisters.reset();
        if(kind==6)draw.vertexShader^=4;
        if(kind==7)bound.surfaces[0].header.reset();
        if(kind==8)bound.surfaces[4].header->at(24)^=1;
        if(kind==9)bound.surfaces[0].object^=4;
        if(kind==10)draw.deviceState[11068]=1;
        if(kind==11)draw.deviceState[12498]=1;
        if(kind==12)draw.deviceState[12563]=1;
        assert(!plan(draw,indices,bound,f.textures,true));++negatives;
    }
    assert(negatives==67);
    if(windows) {
        assert(!planNativeIndexedLod(f.draw,f.indices,f.bound,f.textures,true,&why,profile));
        assert(!planNativeIndexedLod(f.draw,f.indices,f.bound,f.textures,true,&why,profile,static_cast<NativeIndexedLodRaster>(99)));
        std::vector<unsigned> maximum(3*NativeIndexedLodWindowedMaxTriangles,0);
        assert(plan(f.draw,maximum,f.bound,f.textures,true));maximum.insert(maximum.end(),{0,1,2});
        assert(!plan(f.draw,maximum,f.bound,f.textures,true));
        std::puts("INDEXED602_PLAN_PASS legacy_extent_refused=1 exact_state_negatives=67 bounded_indices=12288 no_execution=1");
    }
    std::printf("INDEXED_LOD_CAPTURE_PASS current_shader_resources=1 textures=5 negatives=%u native_execution=0\n",negatives);
    std::printf("INDEXED_LOD_PROFILE_PASS profile=%u cross_profile_rejected=1 invalid_profile_rejected=1\n",windows?602:argc==3?601:600);
}
