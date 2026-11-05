#include "native_indexed_view_exclusion.h"
#include "native_visibility_queries.h"
#include <cassert>
#include <cstdio>
#include <limits>
using namespace recomp::gpu;
int main() {
    Gta5InlineDraw draw;draw.deviceState.resize(13860);draw.primitive=4;draw.vertexCount=6;
    const auto set=[&](unsigned reg,unsigned value) {
        const auto at=*gta5DrawRegisterOffset(reg);
        for(unsigned i=0;i<4;++i)draw.deviceState[at+i]=value>>(24-8*i);
    };
    for(const auto& r:std::array<std::array<unsigned,2>,10>{{{0x2204,0x80000},{0x2205,0x18002},
            {0x2206,0x43F},{0x2208,4},{0x210F,0x42800000},{0x2110,0x42800000},
            {0x2111,0xC2800000},{0x2112,0x42800000},{0x2302,4},{0x2293,0x11}}})set(r[0],r[1]);
    const std::array<unsigned,4> rectangle{0,0,128,128};
    const std::vector<std::uint32_t> indices{0,1,2,3,4,5};
    const std::vector<std::array<float,4>> original{{-4,0,.5,1},{-4,1,.5,1},{-4,-1,.5,1},
        {4,0,.5,1},{4,1,.5,1},{4,-1,.5,1}};
    unsigned cases=0;
    const auto check=[&](const auto& p,const auto& ix,const auto& rect) {
        ++cases;return nativeIndexedViewExclusion(draw,p,ix,rect);
    };
    auto result=check(original,indices,rectangle);
    assert(result && result->triangles==2 && result->outsideXY==2 && !result->behind);
    // No one plane excludes the entire draw, but each triangle is outside.
    assert(!nativeCommonOutsideHomogeneousXYPlane(original));
    auto p=original;for(unsigned i=0;i<3;++i)p[i]={0,0,.5f,-1};
    result=check(p,indices,rectangle);assert(result && result->behind==1 && result->outsideXY==1);
    p[1][3]=1;assert(!check(p,indices,rectangle)); // Crosses the view volume.
    p=original;p[0][3]=-1;assert(check(p,indices,rectangle)); // Linear plane still proves it.
    p=original;p[0]={0,0,.5,1};assert(!check(p,indices,rectangle));
    for(float x:{-1.f,-1.001f,-1.01f}) {
        p=original;for(unsigned i=0;i<3;++i)p[i][0]=x;
        assert(!check(p,indices,rectangle)); // Boundary or <2px separation.
    }
    // Outside the viewport alone is insufficient if the screen rectangle is larger.
    assert(!check(original,indices,std::array<unsigned,4>{0,0,1280,720}));
    for(unsigned lane=0;lane<4;++lane)for(float bad:{1e21f,std::numeric_limits<float>::infinity(),
            std::numeric_limits<float>::quiet_NaN()}) {
        p=original;p[5][lane]=bad;assert(!check(p,indices,rectangle));
    }
    assert(!check(original,std::vector<std::uint32_t>{0,1,6},rectangle));
    assert(!check(original,std::vector<std::uint32_t>{0,1},rectangle));
    assert(!check(original,std::vector<std::uint32_t>{},rectangle));
    assert(!check(std::span(original).first(3),indices,rectangle));
    for(auto rect:std::array<std::array<unsigned,4>,4>{{{0,0,0,128},{0,0,128,0},{0,0,1281,128},{2,0,1,128}}})
        assert(!check(original,indices,rect));
    for(unsigned reg:{0x2204u,0x2205u,0x2206u,0x2208u,0x2183u,0x2184u}) {
        const auto prior=*gta5DrawRegister(draw,reg);set(reg,prior^1);
        assert(!check(original,indices,rectangle));set(reg,prior);
    }
    set(0x2293,0x91);assert(!check(original,indices,rectangle));set(0x2293,0x11);
    draw.primitive=5;assert(!check(original,indices,rectangle));draw.primitive=4;
    auto many=indices;many.resize(65535);for(unsigned i=0;i<many.size();++i)many[i]=i%6;
    result=check(original,many,rectangle);assert(result && result->triangles==21845);
    many.push_back(0);assert(!check(original,many,rectangle));
    // Host-only producer -> query end -> condition connection. These positions
    // are authored, not claimed to be actual fenced GPU vertex measurements.
    NativeVisibilityQueries queries;NativeVisibilityQueries::Journal journal;
    assert(queries.command(8,journal));
    assert(queries.draw(0x11,[&]()->std::optional<NativeVisibilityQueries::Measurement> {
        if(!check(original,indices,rectangle))return std::nullopt;
        return NativeVisibilityQueries::Measurement{false,NativeVisibilityQueries::Stage::PotentialVisibility};
    }));
    assert(!journal.contains(0xC44));assert(queries.condition(0x108)==true);
    assert(queries.command(0x108,journal) && queries.idle());
    assert(queries.condition(0x108)==false && journal.at(0xC44).first==0 && journal.at(0xC44).second==256);
    std::printf("INDEXED_VIEW_EXCLUSION_PASS cases=%u per_triangle=1 negative_w=1 crossing_rejected=1 max_indices=65535 query_connection_authored=1 gpu_execution=0\n",cases);
}
