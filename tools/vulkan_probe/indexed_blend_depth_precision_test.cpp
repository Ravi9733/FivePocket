#include "indexed_blend_depth_fixture.h"
#include "native_indexed_blend_depth_draw.h"
#include "process_limits.h"
#include <cstdio>
using namespace recomp::gpu;
using namespace recomp::gpu::test;
// Actual capture with authored physical owners. Each accepted pixel must agree
// with exact zero incoming depth / masked stencil, including spatially mixed
// values. No saved colour reference or uniform-depth result is relabeled live.
int main(int argc,char** argv) {
    assert(argc==2 && initializeProbeProcess());std::setvbuf(stdout,nullptr,_IONBF,0);
    const std::string folder=argv[1];IndexedBlendDepthFixture f(folder);
    ProbeDevice gpu;assert(gpu.inspect() && gpu.create({}));
    NativeIndexedBlendDepthProgram p{f.program,
        {f.program.vertexIdentity,f.program.pixelIdentity,indexedBlendCode(folder+"/position.comp.spv"),28},
        indexedBlendCode(folder+"/authored-count.frag.spv")};
    constexpr unsigned pixels=1280*720;unsigned seed=0;
    assert(packColor7e3({colorBits(.125f),colorBits(.25f),colorBits(.5f),colorBits(1.f)},seed));
    std::vector<std::uint8_t> colors(pixels*4);
    for(unsigned i=0;i<pixels;++i)for(unsigned c=0;c<4;++c)colors[4*i+c]=seed>>(8*c);
    std::vector<unsigned> reference;unsigned cases=0,changed=0;
    const std::array<unsigned,12> codes{0,1,2,16,256,512,1023,1024,2048,4096,0x100000,0xE00000};
    for(unsigned mode=0;mode<15;++mode) {
        std::vector<unsigned> depth(pixels,7);
        for(unsigned i=0;i<pixels;++i) {
            if(mode<12)depth[i]=(codes[mode]<<8)|7;
            if(mode==12)depth[i]=6;
            if(mode==13)depth[i]=255; // masked equality, not whole-byte equality.
            if(mode==14)depth[i]=(codes[(i/1280+i%1280)%codes.size()]<<8)|((i&1)?255:6);
        }
        NativeEdram32Bands owner;
        assert(owner.recordCompletedRawColorRows(720,1280,1280,720,colors,1));
        assert(owner.recordCompletedDepthWords(0,1280,1280,720,depth,2));
        const char* why=nullptr;bool ok=false;
        {NativeEdram32Mutation mutation(owner);
            ok=executeNativeIndexedBlendDepthDraw(gpu,p,f.draw,f.indices,f.bound,f.textures,
                f.program.vertexIdentity,f.program.pixelIdentity,true,mutation,&why);}
        if(!ok){std::printf("INDEXED_DEPTH_PRECISION_REFUSED mode=%u stage=%s\n",mode,why?why:"unknown");return 2;}
        const auto words=owner.readColorWords(720,1280,1280,720);assert(words && words->size()==pixels);
        assert(owner.readDepthWords(0,1280,1280,720)==depth);
        if(!mode){reference=*words;for(auto word:*words)changed+=word!=seed;assert(changed);}
        else for(unsigned i=0;i<pixels;++i) {
            const bool pass=(depth[i]>>8)==0 && (depth[i]&7)==7;
            assert(words->at(i)==(pass?reference[i]:seed));
        }
        ++cases;
    }
    std::printf("INDEXED_DEPTH_PRECISION_GPU_PASS cases=%u near_zero_depth=1 mixed_depth_stencil=1 stencil_read_mask=1 depth_preserved=1 changed_pixels=%u actual_shader_mesh=1 authored_owner=1 live_validation=0\n",cases,changed);
}
