#include "sampled_stencil_fixture.h"
#include <cstdio>
#include <limits>
using namespace recomp::gpu;
int main(int argc,char** argv) {
    assert(argc==2);test::SampledStencilFixture f(argv[1]);const char* why=nullptr;
    auto p=planNativeSampledStencil(f.draw,f.program,f.bindings,&why);
    if(!p)std::printf("SAMPLED_STENCIL_HOST_REFUSED %s\n",why);assert(p);
    assert(p->target.height==720 && (*nativeTranslatedScissor(f.draw))[3]==384);
    assert(!p->target.state.depthEnable && !p->target.state.depthWrite && p->target.state.front.writeMask==16);
    assert(sameNativeSampledStencilInputs(f.draw,f.bindings,f.draw,f.bindings));
    for(unsigned mode=0;mode<6;++mode) {
        auto changed=f.draw;auto bound=f.bindings;
        if(mode==0)changed.textures[0].source[0]^=1;
        if(mode==1)changed.vertexConstants[0]^=1;
        if(mode==2)changed.vertices[0]^=1;
        if(mode==3)(*bound.surfaces[4].header)[0]^=1;
        if(mode==4)changed.scissorRegisters->at(2)^=1;
        if(mode==5)changed.textures[0].fetch[1]^=0x1000;
        assert(!sameNativeSampledStencilInputs(f.draw,f.bindings,changed,bound));
    }
    const auto raw=linearizeXenosTexture2D(p->texture,f.draw.textures[0].source);assert(raw && raw->size()==1280*720*4);
    std::array<unsigned,256> counts{};for(unsigned i=3;i<raw->size();i+=4)++counts[(*raw)[i]];
    std::printf("SAMPLED_STENCIL_ALPHA_HISTOGRAM zero=%u nonzero=%u\n",counts[0],1280*720-counts[0]);
    for(unsigned alpha=0;alpha<256;++alpha)for(unsigned old=0;old<256;++old) {
        const bool pass=xenosAlphaPass(0xAA00000B,float(alpha)/255,0);
        assert(pass==(alpha==0));
        const auto result=pass?evaluateXenosDepthStencil(p->target.state,0,0,old,false).stencil:old;
        assert(result==(alpha?old:old|16));
    }
    const auto nan=std::numeric_limits<float>::quiet_NaN();
    for(unsigned compare=0;compare<8;++compare) {
        assert(xenosAlphaPass(compare|8,nan,0)==(compare==5 || compare==7));
        assert(xenosAlphaPass(compare|8,0,nan)==(compare==5 || compare==7));
        assert(xenosAlphaPass(compare,nan,nan));
    }
    const auto put=[](auto& b,unsigned at,unsigned v){for(unsigned i=0;i<4;++i)b[at+i]=v>>(24-8*i);};
    for(unsigned mode=0;mode<14;++mode) {
        auto draw=f.draw;auto program=f.program;auto bindings=f.bindings;
        switch(mode) {
        case 0:program.abiVersion=5;break;
        case 1:program.pixelCode.clear();break;
        case 2:draw.textures[0].slot=0;break;
        case 3:draw.textures[0].source.pop_back();break;
        case 4:draw.textures[0].fetch[3]|=1u<<19;break;
        case 5:put(draw.deviceState,10500,0x3F800000);break; // ALPHA_REF
        case 6:put(draw.deviceState,10556,0xAA000007);break;
        case 7:put(draw.deviceState,10460,15);break;
        case 8:put(draw.deviceState,10548,0x708777);break;
        case 9:(*bindings.surfaces[4].header)[36]^=1;break;
        case 10:bindings.surfaces[4].object+=4;break;
        case 11:draw.scissorRegisters.reset();break;
        case 12:draw.pixelShader=0;break;
        case 13:draw.deviceState[11068]|=0x10;break;
        }
        assert(!planNativeSampledStencil(draw,program,bindings));
    }
    std::puts("SAMPLED_STENCIL_HOST_PASS actual_state=1 alpha_reference_zero=1 mask16=1 scissor_not_extent=1 negatives=14 gpu=0");
}
