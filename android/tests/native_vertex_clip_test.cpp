#include "native_vertex_clip.h"
#include <cassert>
#include <limits>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    unsigned cases=0;
    for(unsigned plane=0;plane<4;++plane)for(float w:{0.125f,1.f,4096.f}) {
        std::array<std::array<float,4>,3> p{{{0,0,0,w},{0,0,.5f*w,w},{0,0,w,w}}};
        for(auto& v:p)v[plane/2]=w*(plane%2?2.f:-2.f);
        assert(nativeCommonOutsideXYPlane(p)==plane);++cases;
        auto mixed=p;mixed[1][plane/2]=0;assert(!nativeCommonOutsideXYPlane(mixed));
        auto edge=p;edge[2][plane/2]=w*(plane%2?1.f:-1.f);assert(!nativeCommonOutsideXYPlane(edge));
        auto near=p;near[2][plane/2]=w*(plane%2?1.001f:-1.001f);assert(!nativeCommonOutsideXYPlane(near));
        for(float bad:{0.f,-1.f,1e-8f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
            auto invalid=p;invalid[1][3]=bad;assert(!nativeCommonOutsideXYPlane(invalid));++cases;
        }
    }
    assert(!nativeCommonOutsideXYPlane({}));
    std::array<std::array<float,4>,3> z{{{0,0,-100,1},{0,0,-100,1},{0,0,-100,1}}};
    assert(!nativeCommonOutsideXYPlane(z)); // XY only, never inferred Z clipping.
    std::array<std::array<float,4>,3> behind{{{-1,3,-2,-1},{2,-4,0,-2},{0,0,-4,-1}}};
    assert(nativeAllVerticesBehindW(behind) && !nativeCommonOutsideXYPlane(behind));
    for(float w:{0.f,1.f,-1e-8f,-1e-6f,-.001f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        auto bad=behind;bad[1][3]=w;assert(!nativeAllVerticesBehindW(bad));++cases;
    }
    for(float x:{1e21f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
        auto bad=behind;bad[1][0]=x;assert(!nativeAllVerticesBehindW(bad));++cases;
    }
    assert(!nativeAllVerticesBehindW({}));
    Gta5InlineDraw d;d.primitive=5;d.vertexCount=3;d.stride=16;d.vertices.resize(48);
    d.deviceState.resize(13860);
    d.scissorRegisters=std::array<std::uint32_t,3>{0,0,0x800080};
    auto set=[&](unsigned r,unsigned v){auto at=*gta5DrawRegisterOffset(r);for(unsigned i=0;i<4;++i)d.deviceState[at+i]=v>>(24-8*i);};
    for(const auto r:std::array<std::array<unsigned,2>,9>{{{0x2204,0x80000},{0x2205,0x18000},{0x2206,0x43F},
        {0x2208,4},{0x210F,0x42800000},{0x2110,0x42800000},{0x2111,0xC2800000},{0x2112,0x42800000},{0x2302,4}}})set(r[0],r[1]);
    assert(nativeClippedFanState(d,16));
    assert(nativeClippedFanOutside(d,behind)==4);
    std::array<std::array<float,4>,3> above{{{0,2,0,1},{.5,2,.5,1},{1,2,1,1}}};
    assert(nativeClippedFanOutside(d,above)==3);
    above[1][1]=1.01f;assert(!nativeClippedFanOutside(d,above)); // <2px margin.
    above[1][1]=2;
    d.scissorRegisters=std::array<std::uint32_t,3>{0,0,0x2D00500};
    // Positive X beyond a narrow viewport may still be INSIDE a larger scissor.
    for(auto& p:above){p[0]=2;p[1]=0;}
    assert(!nativeClippedFanOutside(d,above));
    d.scissorRegisters=std::array<std::uint32_t,3>{0,0,0x800080};
    assert(nativeClippedFanOutside(d,above)==1);
    unsigned crossingCases=0;
    for(unsigned plane=0;plane<4;++plane)for(float w:{-2.f,0.f,-1e-8f,1e-8f}) {
        std::array<std::array<float,4>,3> p{{{0,0,0,w},{0,0,.5f,1},{0,0,1,1}}};
        for(auto& v:p)v[plane/2]=plane%2?4.f:-4.f;
        assert(nativeCommonOutsideHomogeneousXYPlane(p)==plane);
        assert(nativeClippedFanOutside(d,p)==plane);++crossingCases;
        // One inside vertex defeats common-plane rejection even if the other
        // two divide to apparently offscreen coordinates.
        auto visible=p;visible[1]={0,0,.5f,1};assert(!nativeClippedFanOutside(d,visible));
        auto edge=p;edge[1][plane/2]=plane%2?1.f:-1.f;
        assert(!nativeCommonOutsideHomogeneousXYPlane(edge) && !nativeClippedFanOutside(d,edge));
        edge[1][plane/2]=plane%2?1.01f:-1.01f;
        assert(nativeCommonOutsideHomogeneousXYPlane(edge) && !nativeClippedFanOutside(d,edge));
        assert(!nativeClippedFanOutside(d,std::span(p).first(2)));
        for(unsigned lane=0;lane<4;++lane)for(float bad:{1e21f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
            auto invalid=p;invalid[1][lane]=bad;
            assert(!nativeCommonOutsideHomogeneousXYPlane(invalid) && !nativeClippedFanOutside(d,invalid));
        }
    }
    std::array<std::array<float,4>,3> apparentOutside{{{-4,0,0,-1},{4,-1,.5f,1},{4,1,.5f,1}}};
    // Dividing each vertex by W would put all three at X=4, but homogeneous
    // clipping crosses the view volume. This must NOT be elided.
    assert(!nativeClippedFanOutside(d,apparentOutside));
    assert(!nativeCommonOutsideHomogeneousXYPlane(z) && !nativeCommonOutsideHomogeneousXYPlane({}));
    for(unsigned r:{0x2204u,0x2205u,0x2206u,0x2208u,0x2284u,0x2292u,0x2293u,0x2183u,0x2184u}) {
        const auto old=*gta5DrawRegister(d,r);set(r,old^1);assert(!nativeClippedFanState(d,16));
        assert(!nativeClippedFanOutside(d,behind));set(r,old);
    }
    d.vertices.pop_back();assert(!nativeClippedFanState(d,16));d.vertices.push_back(0);
    d.vertexCount=2;assert(!nativeClippedFanState(d,16));
    d.primitive=13;d.vertexCount=4;d.vertices.resize(64);set(0x2205,0x18002);
    unsigned quadCases=0;
    for(unsigned plane=0;plane<4;++plane) {
        std::array<std::array<float,4>,4> q{{{0,-1,0,-2},{0,-1,0,1},{0,1,0,1},{0,1,0,-2}}};
        const auto axis=plane/2;
        for(auto& p:q)p[axis]=(plane%2?1.f:-1.f)*(axis?-4.f:4.f);
        assert(nativeCommonOutsideScissorHalfPlane(d,q)==plane);
        const auto result=nativeClippedQuadListOutside(d,q);assert(result && result->scissorExcluded==1);
        auto mixed=q;mixed[2]={0,0,.5f,1};assert(!nativeClippedQuadListOutside(d,mixed));
        auto edge=q;edge[2]={0,0,.5f,1};edge[2][axis]=(plane%2?1.f:-1.f)*(axis?-1.01f:1.01f);
        assert(!nativeClippedQuadListOutside(d,edge));++quadCases;
    }
    d.vertexCount=12;d.vertices.resize(192);
    std::array<std::array<float,4>,12> quads{{{4,-1,0,-2},{4,-1,0,1},{4,1,0,1},{4,1,0,-2},
        {1,1,0,-1},{-1,1,0,-1},{-1,-1,0,-1},{1,-1,0,-1},
        {0,0,.5f,1},{0,0,.5f,1},{0,0,.5f,1},{0,0,.5f,1}}};
    const auto all=nativeClippedQuadListOutside(d,quads);
    assert(all && all->behind==1 && all->scissorExcluded==1 && all->degenerate==1);
    quads[11][0]=1;assert(!nativeClippedQuadListOutside(d,quads));quads[11][0]=0;
    assert(!nativeClippedQuadListOutside(d,std::span(quads).first(8)));
    for(unsigned r:{0x2204u,0x2205u,0x2206u,0x2208u,0x2284u,0x2292u,0x2293u,0x2183u,0x2184u}) {
        const auto old=*gta5DrawRegister(d,r);set(r,old^1);assert(!nativeClippedQuadListOutside(d,quads));set(r,old);
    }
    quads[11][0]=std::numeric_limits<float>::quiet_NaN();assert(!nativeClippedQuadListOutside(d,quads));
    std::printf("NATIVE_QUAD_CLIP_PASS crossing_w_cases=%u mixed_categories=3 bad_state_and_extent_rejected=1\n",quadCases);
    std::printf("NATIVE_VERTEX_CLIP_PASS numeric_cases=%u state_and_extent_negatives=1\n",cases);
    std::printf("NATIVE_FAN_HOMOGENEOUS_PASS crossing_w_cases=%u divided_w_false_positive_rejected=1\n",crossingCases);
}
