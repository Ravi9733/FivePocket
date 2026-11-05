#include "native_vertex_clip_executor.h"
#include "process_limits.h"
#include <cassert>
#include <cstdio>
#include <fstream>
using namespace recomp::gpu;
std::vector<std::uint32_t> module(const char* p) {
    std::ifstream f(p,std::ios::binary|std::ios::ate);assert(f);const auto n=f.tellg();assert(n>0 && n%4==0 && n<262144);
    std::vector<std::uint32_t> words(n/4);f.seekg(0);assert(f.read(reinterpret_cast<char*>(words.data()),n));return words;
}
int main(int argc,char** argv) {
    assert(argc==3 && initializeProbeProcess());
    ProbeDevice device;assert(device.inspect() && device.create({}));
    NativeClippedFanProgram p;p.stride=16;p.positionCode=module(argv[1]);
    auto executor=NativeVertexClipExecutor::create(device,p);assert(executor);
    p.positionCode=module(argv[2]);auto incomplete=NativeVertexClipExecutor::create(device,p);assert(incomplete);
    Gta5InlineDraw draw;draw.primitive=5;draw.stride=16;draw.deviceState.resize(13860);
    draw.scissorRegisters=std::array<std::uint32_t,3>{0,0,0x800080};
    const auto set=[&](unsigned r,unsigned v){auto at=*gta5DrawRegisterOffset(r);for(unsigned i=0;i<4;++i)draw.deviceState[at+i]=v>>(24-8*i);};
    for(const auto r:std::array<std::array<unsigned,2>,9>{{{0x2204,0x80000},{0x2205,0x18000},{0x2206,0x43F},
        {0x2208,4},{0x210F,0x42800000},{0x2110,0x42800000},{0x2111,0xC2800000},{0x2112,0x42800000},{0x2302,4}}})set(r[0],r[1]);
    const auto vertex=[&](unsigned i,const std::array<float,4>& v){for(unsigned c=0;c<4;++c){std::uint32_t bits;std::memcpy(&bits,&v[c],4);
        for(unsigned b=0;b<4;++b)draw.vertices[i*16+c*4+b]=bits>>(24-8*b);}};
    unsigned cases=0;
    for(unsigned count:{3u,63u,64u,65u,90u,16384u})for(unsigned plane=0;plane<4;++plane) {
        draw.vertexCount=count;draw.vertices.resize(count*16);
        std::array<float,4> position{0,0,.5,1};position[plane/2]=plane%2?2:-2;
        for(unsigned i=0;i<count;++i)vertex(i,position);
        const auto out=executor->positions(draw);assert(out && out->size()==count);
        for(const auto& v:*out)assert(v==position);
        assert(executor->execute(draw)==plane);
        assert(!incomplete->execute(draw));
        vertex(count-1,{0,0,.5,1});assert(!executor->execute(draw)); // One inside defeats common-plane proof.
        vertex(count-1,{0,0,.5,std::numeric_limits<float>::infinity()});assert(!executor->execute(draw));
        ++cases;
    }
    draw.vertices.pop_back();assert(!executor->execute(draw));
    unsigned behindCases=0;
    for(unsigned count:{3u,63u,64u,65u,90u,16384u}) {
        draw.vertexCount=count;draw.vertices.resize(count*16);
        for(unsigned i=0;i<count;++i)vertex(i,{float(i%11)-5.f,3.f,-2.f,-1.f});
        const auto out=executor->positions(draw);assert(out && out->size()==count);
        assert(executor->execute(draw)==4 && !incomplete->execute(draw));
        vertex(count-1,{0,0,0,0});assert(!executor->execute(draw));
        vertex(count-1,{0,0,0,1});assert(!executor->execute(draw));
        vertex(count-1,{0,0,0,-1e-8f});assert(!executor->execute(draw));
        vertex(count-1,{0,0,0,std::numeric_limits<float>::quiet_NaN()});assert(!executor->execute(draw));++behindCases;
    }
    unsigned mixedWFanCases=0;
    for(unsigned count:{3u,63u,64u,65u,90u,16384u})for(unsigned plane=0;plane<4;++plane) {
        draw.vertexCount=count;draw.vertices.resize(count*16);
        for(unsigned i=0;i<count;++i) {
            std::array<float,4> position{0,0,.5f,i%3==0?-2.f:i%3==1?0.f:1.f};
            position[plane/2]=plane%2?4.f:-4.f;vertex(i,position);
        }
        assert(executor->execute(draw)==plane && !incomplete->execute(draw));
        vertex(count-1,{0,0,.5,1});assert(!executor->execute(draw));
        ++mixedWFanCases;
    }
    std::printf("NATIVE_MIXED_W_FAN_GPU_PASS cases=%u missing_invocation_rejected=1\n",mixedWFanCases);
    draw.primitive=13;set(0x2205,0x18002);
    unsigned quadCases=0;
    for(unsigned count:{4u,64u,68u,16384u}) {
        draw.vertexCount=count;draw.vertices.resize(count*16);
        for(unsigned i=0;i<count;++i)vertex(i,{float(i),0,.5f,i%2?-.1f:2.f});
        const auto out=executor->quadPositions(draw);assert(out && out->size()==count);
        for(unsigned i=0;i<count;++i)assert(((*out)[i]==std::array<float,4>{float(i),0,.5f,i%2?-.1f:2.f}));
        assert(!executor->positions(draw) && !executor->execute(draw));
        assert(!incomplete->quadPositions(draw));
        vertex(count-1,{0,0,.5f,std::numeric_limits<float>::infinity()});assert(!executor->quadPositions(draw));++quadCases;
    }
    draw.vertexCount=3;draw.vertices.resize(48);assert(!executor->quadPositions(draw));
    unsigned quadClippedCases=0;
    for(unsigned count:{4u,64u,68u,16384u}) {
        draw.vertexCount=count;draw.vertices.resize(count*16);
        for(unsigned i=0;i<count;++i)vertex(i,{4,float(i%4<2?-1:1),0,i%4==0 || i%4==3?-2.f:1.f});
        const auto clipped=executor->executeQuads(draw);
        assert(clipped && clipped->scissorExcluded==count/4 && !clipped->behind && !clipped->degenerate);
        assert(!incomplete->executeQuads(draw));
        vertex(count-1,{0,0,.5,1});assert(!executor->executeQuads(draw));
        for(unsigned i=0;i<count;++i)vertex(i,{float(i%4),0,0,-1});
        const auto behind=executor->executeQuads(draw);assert(behind && behind->behind==count/4);
        for(unsigned i=0;i<count;++i)vertex(i,{0,0,.5,1});
        const auto point=executor->executeQuads(draw);assert(point && point->degenerate==count/4);
        vertex(count-1,{0,0,.5,std::numeric_limits<float>::quiet_NaN()});assert(!executor->executeQuads(draw));
        ++quadClippedCases;
    }
    std::printf("NATIVE_QUAD_CLIP_GPU_PASS count_cases=%u mixed_w_scissor_behind_point=1 missing_invocation_rejected=1\n",quadClippedCases);
    std::printf("NATIVE_VERTEX_CLIP_GPU_PASS cases=%u behind_cases=%u quad_cases=%u all_positions_exact=1 missing_invocation_rejected=1 targets_untouched=1\n",cases,behindCases,quadCases);
}
