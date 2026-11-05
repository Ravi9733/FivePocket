#include "native_axis_quad.h"
#include <cassert>
#include <cstdio>
#include <limits>
using namespace recomp::gpu;
int main() {
    Gta5InlineDraw draw;draw.primitive=13;draw.vertexCount=4;draw.stride=16;draw.vertices.resize(64);draw.deviceState.resize(13860);
    draw.scissorRegisters=std::array<std::uint32_t,3>{0,0,0x800080};
    const auto set=[&](unsigned r,unsigned v){auto at=*gta5DrawRegisterOffset(r);for(unsigned i=0;i<4;++i)draw.deviceState[at+i]=v>>(24-8*i);};
    for(const auto r:std::array<std::array<unsigned,2>,9>{{{0x2204,0x80000},{0x2205,0x18002},{0x2206,0x43F},
        {0x2208,4},{0x210F,0x42800000},{0x2110,0x42800000},{0x2111,0xC2800000},{0x2112,0x42800000},{0x2302,4}}})set(r[0],r[1]);
    const std::vector<std::array<float,4>> original{{-.5,2,-.1,2},{-.5,2,.1,-.1},{-.5,-2,.1,-.1},{-.5,-2,-.1,2}};
    unsigned cases=0;
    for(unsigned axis:{0u,1u})for(unsigned rotation=0;rotation<4;++rotation)
    for(float x:{-.5f,.5f})for(float lowW:{-.1f,0.f,.1f})for(bool reverse:{false,true}) {
        auto p=original;for(auto& v:p)v[0]=x;p[1][3]=p[2][3]=lowW;
        if(axis)for(auto& v:p)std::swap(v[0],v[1]);
        std::rotate(p.begin(),p.begin()+rotation,p.end());
        if(reverse)std::reverse(p.begin(),p.end());
        const auto result=nativeSingleCoverageAxisQuadList(draw,p);
        assert(result && result->candidateQuad==0 && result->candidateQuads==1 && result->constantAxis==axis && !result->scissorExcluded && !result->degenerate);++cases;
        std::swap(p[1],p[2]);assert(!nativeSingleCoverageAxisQuadList(draw,p)); // Self crossing order.
    }
    assert(!nativeClippedFanState(draw,16) && nativeQuadPositionState(draw,16));
    // Two overlapping rectangles must never be treated as single coverage.
    auto more=original;more.insert(more.end(),original.begin(),original.end());
    draw.vertexCount=8;draw.vertices.resize(128);assert(!nativeSingleCoverageAxisQuadList(draw,more));
    // Authored adjacent rectangles sharing exactly one Y edge. Different
    // W extents do not defeat the inverse map when X is the same nonzero value.
    auto adjacent=more;
    for(unsigned i=0;i<4;++i)adjacent[i][1]=i<2?1:0;
    for(unsigned i=4;i<8;++i)adjacent[i][1]=i<6?0:-1;
    adjacent[5][3]=adjacent[6][3]=-.5;
    auto separated=nativeSingleCoverageAxisQuadList(draw,adjacent);
    assert(separated && separated->candidateQuads==2 && !separated->scissorExcluded);
    auto transposed=adjacent;for(auto& v:transposed)std::swap(v[0],v[1]);
    separated=nativeSingleCoverageAxisQuadList(draw,transposed);assert(separated && separated->candidateQuads==2 && separated->constantAxis==1);
    for(unsigned i=4;i<8;++i){transposed[i][2]=-.5;transposed[i][3]=-1;}
    separated=nativeSingleCoverageAxisQuadList(draw,transposed);assert(separated && separated->candidateQuads==1 && separated->behind==1);
    auto overlap=adjacent;overlap[4][1]=overlap[5][1]=.01;
    assert(!nativeSingleCoverageAxisQuadList(draw,overlap));
    auto differentX=adjacent;for(unsigned i=4;i<8;++i)differentX[i][0]=.5;
    assert(!nativeSingleCoverageAxisQuadList(draw,differentX));
    // Rectangles can also be disjoint along W; a shared W edge is allowed.
    auto alongW=more;alongW[0][3]=alongW[3][3]=1;alongW[1][3]=alongW[2][3]=.5;
    alongW[4][3]=alongW[7][3]=2;alongW[5][3]=alongW[6][3]=1;
    assert(nativeSingleCoverageAxisQuadList(draw,alongW)->candidateQuads==2);
    alongW[5][3]=alongW[6][3]=.99;assert(!nativeSingleCoverageAxisQuadList(draw,alongW));
    // Exact degenerates contribute no area, even behind the camera.
    for(unsigned i=4;i<8;++i)more[i]={1,1,-1,-1};
    auto result=nativeSingleCoverageAxisQuadList(draw,more);assert(result && result->degenerate==1);
    // This lower quad crosses W=0, but a common homogeneous scissor plane
    // proves that every surviving W>0 point remains below the scissor.
    for(unsigned i=4;i<8;++i){more[i]=original[i-4];more[i][1]-=8;}
    result=nativeSingleCoverageAxisQuadList(draw,more);assert(result && result->scissorExcluded==1);
    // Negative-W vertices cannot be divided then classified as ordinary NDC.
    more[5][1]=100;assert(!nativeSingleCoverageAxisQuadList(draw,more));
    draw.vertexCount=4;draw.vertices.resize(64);
    for(unsigned vertex=0;vertex<4;++vertex)for(unsigned lane=0;lane<4;++lane) {
        auto p=original;p[vertex][lane]+=.001f;assert(!nativeSingleCoverageAxisQuadList(draw,p));++cases;
        p=original;p[vertex][lane]=std::numeric_limits<float>::quiet_NaN();assert(!nativeSingleCoverageAxisQuadList(draw,p));
    }
    for(float x:{0.f,1e-8f}){auto p=original;for(auto& v:p)v[0]=x;assert(!nativeSingleCoverageAxisQuadList(draw,p));}
    auto flat=original;for(auto& v:flat)v[3]=2;assert(!nativeSingleCoverageAxisQuadList(draw,flat));
    const std::vector<std::array<float,4>> front{{-2,-2,.1f,2},{-2,2,.1f,2},{2,2,.1f,2},{2,-2,.1f,2}};
    for(unsigned rotation=0;rotation<4;++rotation)for(bool reverse:{false,true}) {
        auto p=front;std::rotate(p.begin(),p.begin()+rotation,p.end());if(reverse)std::reverse(p.begin(),p.end());
        const auto proof=nativeSingleCoverageAxisQuadList(draw,p);assert(proof && proof->constantAxis==3 && proof->candidateQuads==1);++cases;
        std::swap(p[1],p[2]);assert(!nativeSingleCoverageAxisQuadList(draw,p));
    }
    for(unsigned i=0;i<4;++i)for(unsigned c=0;c<4;++c){auto p=front;p[i][c]+=.001f;assert(!nativeSingleCoverageAxisQuadList(draw,p));}
    auto frontPair=front;frontPair.insert(frontPair.end(),front.begin(),front.end());
    draw.vertexCount=8;draw.vertices.resize(128);assert(!nativeSingleCoverageAxisQuadList(draw,frontPair));
    for(unsigned i=0;i<4;++i)if(frontPair[i][0]>0)frontPair[i][0]=0;
    for(unsigned i=4;i<8;++i)if(frontPair[i][0]<0)frontPair[i][0]=0;
    assert(nativeSingleCoverageAxisQuadList(draw,frontPair)->candidateQuads==2);
    auto badPlane=frontPair;for(unsigned i=4;i<8;++i)badPlane[i][2]=.2f;assert(!nativeSingleCoverageAxisQuadList(draw,badPlane));
    badPlane=frontPair;for(unsigned i=4;i<8;++i)badPlane[i][3]=2.1f;assert(!nativeSingleCoverageAxisQuadList(draw,badPlane));
    frontPair[4][0]=frontPair[5][0]=-.01f;assert(!nativeSingleCoverageAxisQuadList(draw,frontPair));
    draw.vertexCount=4;draw.vertices.resize(64);
    for(float w:{0.f,1e-8f}){auto p=front;for(auto& q:p)q[3]=w;assert(!nativeSingleCoverageAxisQuadList(draw,p));}
    for(unsigned r:{0x2204u,0x2205u,0x2206u,0x2208u,0x2284u,0x2292u,0x2293u,0x2183u,0x2184u}) {
        const auto old=*gta5DrawRegister(draw,r);set(r,old^1);assert(!nativeSingleCoverageAxisQuadList(draw,original));set(r,old);++cases;
    }
    draw.vertices.pop_back();assert(!nativeSingleCoverageAxisQuadList(draw,original));draw.vertices.push_back(0);
    draw.vertexCount=3;assert(!nativeQuadPositionState(draw,16));
    std::printf("NATIVE_AXIS_QUAD_PASS cases=%u negative_w_supported=1 overlapping_candidates_rejected=1\n",cases);
}
