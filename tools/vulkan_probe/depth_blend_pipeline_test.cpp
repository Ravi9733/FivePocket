#include "multisample_draw_probe.h"
#include "process_limits.h"
#include <cassert>
#include <cstdio>
#include <fstream>

using namespace recomp::gpu;
std::vector<std::uint32_t> code(const char* path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);assert(f);
    const auto size=static_cast<std::size_t>(f.tellg());assert(size && size%4==0 && size<65536);
    std::vector<std::uint32_t> result(size/4);f.seekg(0);assert(f.read(reinterpret_cast<char*>(result.data()),size));return result;
}
int main(int argc,char** argv) {
    assert(argc==3 && initializeProbeProcess());
    ProbeDevice device;assert(device.inspect() && device.create({}));
    NativeTranslatedDrawProgram program;program.vertexCode=code(argv[1]);program.pixelCode=code(argv[2]);
    program.textureSlot=UINT32_MAX;
    const std::array<float,4> source{.25,.5,.75,.125},clear{.125,.25,.5,.5};
    std::vector<std::uint8_t> input(8208);const float depth=.5;std::memcpy(input.data(),&depth,4);
    unsigned cases=0;
    for(unsigned samples:{1u,2u,4u}) {
        auto target=MultisampleDrawProbe::create(device,8,8,samples);assert(target);
        NativeTranslatedPipelineOptions options;options.samples=samples;options.minDepth=0;options.maxDepth=1;
        options.depthStencil=*decodeXenosDepthStencilState(0x700002,0,0);
        for(auto blend:{NativeTranslatedBlend::Replace,NativeTranslatedBlend::SourceAlpha,NativeTranslatedBlend::DestinationAlphaAdd})
        for(unsigned compare=0;compare<8;++compare)for(float previous:{.25f,.5f,.75f}) {
            options.depthStencil->depthCompare=compare;
            const bool pass=xenosCompare(compare,.5f,previous);
            const auto result=target->draw(program,input,3,{1,1,0,0},{},options,blend,clear,previous,0);assert(result);
            for(unsigned i=0;i<result->size();++i) {
                const auto c=i%4;float expected=clear[c];
                if(pass)expected=c==3 || blend==NativeTranslatedBlend::Replace?source[c]:
                    blend==NativeTranslatedBlend::SourceAlpha?source[c]*source[3]+clear[c]*(1-source[3]):source[c]*clear[3]+clear[c];
                assert((*result)[i]==expected); // Authored dyadic floats: exact expectations.
            }
            ++cases;
        }
        options.depthStencil->depthCompare=7;options.depthStencil->stencilEnable=true;
        for(unsigned compare=0;compare<8;++compare)for(unsigned previous:{0u,37u,255u}) {
            options.depthStencil->front={compare,0,0,0,37,255,0};
            const bool pass=xenosCompare(compare,37u,previous);
            const auto result=target->draw(program,input,3,{1,1,0,0},{},options,NativeTranslatedBlend::Replace,clear,0,previous);assert(result);
            for(unsigned i=0;i<result->size();++i)assert((*result)[i]==(pass?source[i%4]:clear[i%4]));
            ++cases;
        }
        auto invalid=options;invalid.samples=3;
        assert(!target->draw(program,input,3,{1,1,0,0},{},invalid,NativeTranslatedBlend::Replace,clear,0,0));
        options.depthStencil->stencilEnable=false;
        for(auto winding:{VK_FRONT_FACE_COUNTER_CLOCKWISE,VK_FRONT_FACE_CLOCKWISE}) {
            std::array<std::vector<float>,4> colors;
            for(unsigned mode=0;mode<4;++mode) {
                options.frontFace=winding;options.cullMode=mode;
                const auto result=target->draw(program,input,3,{1,1,0,0},{},options,
                    NativeTranslatedBlend::Replace,clear,0,0);assert(result);colors[mode]=*result;
                ++cases;
            }
            for(unsigned i=0;i<colors[0].size();++i) {
                assert(colors[0][i]==source[i%4] && colors[3][i]==clear[i%4]);
                assert((colors[1][i]==source[i%4] && colors[2][i]==clear[i%4]) ||
                       (colors[1][i]==clear[i%4] && colors[2][i]==source[i%4]));
            }
            options.frontFace=winding==VK_FRONT_FACE_CLOCKWISE?VK_FRONT_FACE_COUNTER_CLOCKWISE:VK_FRONT_FACE_CLOCKWISE;
            options.cullMode=VK_CULL_MODE_FRONT_BIT;
            const auto flipped=target->draw(program,input,3,{1,1,0,0},{},options,
                NativeTranslatedBlend::Replace,clear,0,0);assert(flipped && *flipped==colors[2]);
        }
        for(unsigned mode:{4u,UINT32_MAX}) {
            invalid=options;invalid.cullMode=mode;
            assert(!target->draw(program,input,3,{1,1,0,0},{},invalid,NativeTranslatedBlend::Replace,clear,0,0));
        }
        invalid=options;invalid.frontFace=static_cast<VkFrontFace>(2);
        assert(!target->draw(program,input,3,{1,1,0,0},{},invalid,NativeTranslatedBlend::Replace,clear,0,0));
        std::printf("DEPTH_BLEND_GPU_CASE_PASS samples=%u comparisons=8 blends=3 stencil_comparisons=8\n",samples);
    }
    std::printf("DEPTH_BLEND_GPU_PASS cases=%u exact_authored_float_colors=1 stencil_writes_tested=0 gamma_storage_tested=0\n",cases);
}
