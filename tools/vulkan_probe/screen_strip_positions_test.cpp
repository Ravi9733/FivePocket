// Exact original VS + captured constants/vertices -> compute positions only.
// Does NOT test the pixel shader, strip rasterization, depth/stencil or blending.
#ifndef RECOMP_PRIVATE_STRIP_POSITION_PACK
#error An explicit private original shader compute pack is required
#endif
#include RECOMP_PRIVATE_STRIP_POSITION_PACK
#include "native_vertex_clip_executor.h"
#include <bit>
#include <cassert>
#include <cstdio>
#include <fstream>
using namespace recomp::gpu;
int main(int argc,char** argv) {
    assert(argc==2);const std::string folder=argv[1];std::setvbuf(stdout,nullptr,_IONBF,0);
    const auto read=[&](const char* name,unsigned size) {
        std::ifstream f(folder+"/"+name,std::ios::binary|std::ios::ate);assert(f && f.tellg()==size);
        f.seekg(0);std::vector<std::uint8_t> b(size);assert(f.read(reinterpret_cast<char*>(b.data()),size));return b;
    };
    const auto be=[](const auto& b,unsigned at){return (std::uint32_t{b[at]}<<24)|
        (std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];};
    assert(read("vertex-shader.bin",300)==screenStripPositions.vertexIdentity);
    assert(read("pixel-shader.bin",492)==screenStripPositions.pixelIdentity);
    Gta5InlineDraw draw;draw.primitive=6;draw.vertexCount=100;draw.stride=8;
    draw.vertices=read("vertices.bin",800);draw.deviceState=read("device-state.bin",13860);
    const auto vc=read("vertex-constants.bin",4096),pc=read("pixel-constants.bin",4096);
    for(unsigned i=0;i<1024;++i){draw.vertexConstants[i]=be(vc,4*i);draw.pixelConstants[i]=be(pc,4*i);}
    const auto expected=read("position-reference-be.bin",1600);
    ProbeDevice device;ShaderRequirements requirements;assert(device.inspect() && device.create(requirements));
    auto executor=NativeVertexClipExecutor::create(device,screenStripPositions);assert(executor);
    for(unsigned kind=0;kind<5;++kind) {
        auto bad=draw;
        if(kind==0)bad.primitive=4;
        if(kind==1)bad.vertexCount=2;
        if(kind==2)bad.vertexCount=65537;
        if(kind==3)bad.stride=12;
        if(kind==4)bad.vertices.pop_back();
        assert(!executor->stripPositions(bad));
    }
    const auto actual=executor->stripPositions(draw);assert(actual && actual->size()==100);
    float maximumError=0;
    for(unsigned i=0;i<100;++i)for(unsigned lane=0;lane<4;++lane) {
        const float want=std::bit_cast<float>(be(expected,16*i+4*lane));
        const float got=(*actual)[i][lane];assert(std::isfinite(got) && std::isfinite(want));
        const float error=std::abs(got-want);maximumError=std::max(maximumError,error);
        // Four FP32 epsilons absolute. This is a host-reference comparison of
        // bounded captured values, NOT Xbox transcendental/depth equivalence.
        if(error>0x1p-21f)std::printf("STRIP_POSITION_MISMATCH vertex=%u lane=%u got=%.9g want=%.9g\n",i,lane,got,want);
        assert(error<=0x1p-21f);
    }
    std::printf("SCREEN_STRIP_POSITIONS_GPU_PASS vertices=100 components=400 extent_negatives=5 "
        "max_abs_error=%.9g tolerance=4.76837158e-7 raster=0 pixel=0 gameplay=0\n",maximumError);
}
