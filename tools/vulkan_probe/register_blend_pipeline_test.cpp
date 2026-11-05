#include "multisample_draw_probe.h"
#include "process_limits.h"
#include <cassert>
#include <cstdio>
#include <fstream>

using namespace recomp::gpu;
static std::vector<std::uint32_t> readCode(const char* path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);assert(f);
    const auto size=static_cast<std::size_t>(f.tellg());assert(size && size%4==0 && size<65536);
    std::vector<std::uint32_t> result(size/4);f.seekg(0);
    assert(f.read(reinterpret_cast<char*>(result.data()),size));return result;
}
int main(int argc,char** argv) {
    assert(argc==3 && initializeProbeProcess());
    ProbeDevice device;assert(device.inspect() && device.create({}));
    NativeTranslatedDrawProgram program;program.abiVersion=12;
    program.vertexCode=readCode(argv[1]);program.pixelCode=readCode(argv[2]);
    program.textureSlot=UINT32_MAX;
    // Authored dyadic floats, not captured game shader/target/texture evidence.
    const std::array<float,4> source{.25f,.5f,.75f,.125f},clear{.125f,.25f,.5f,.5f};
    const auto factor=[&](unsigned f,unsigned c) {
        switch(f) {
        case 0:return 0.f;case 1:return 1.f;
        case 4:return source[c];case 5:return 1-source[c];
        case 6:return source[3];case 7:return 1-source[3];
        case 8:return clear[c];case 9:return 1-clear[c];
        case 10:return clear[3];case 11:return 1-clear[3];
        case 16:return c==3?1.f:std::min(source[3],1-clear[3]);
        default:assert(false);return 0.f;
        }
    };
    const auto expected=[&](std::uint32_t word,unsigned c) {
        const unsigned shift=c==3?16:0;
        const unsigned a=(word>>shift)&31,b=(word>>(shift+8))&31,op=(word>>(shift+5))&7;
        const float x=source[c]*factor(a,c),y=clear[c]*factor(b,c);
        switch(op) {
        case 0:return x+y;case 1:return x-y;case 2:return std::min(source[c],clear[c]);
        case 3:return std::max(source[c],clear[c]);case 4:return y-x;
        default:assert(false);return 0.f;
        }
    };
    std::vector<std::uint32_t> controls{0x07060706,0x00010706};
    for(unsigned shift:{0u,8u,16u,24u})for(unsigned f:{0u,1u,4u,5u,6u,7u,8u,9u,10u,11u,16u})
        controls.push_back((0x07060706u&~(31u<<shift))|(f<<shift));
    for(unsigned shift:{5u,21u})for(unsigned op=0;op<5;++op)
        controls.push_back(0x07060706u|(op<<shift));
    std::vector<std::uint8_t> input(8208);const float z=.5f;std::memcpy(input.data(),&z,4);
    unsigned cases=0,rejected=0;
    for(unsigned samples:{1u,2u,4u}) {
        auto target=MultisampleDrawProbe::create(device,8,8,samples);assert(target);
        NativeTranslatedPipelineOptions options;options.samples=samples;options.minDepth=0;options.maxDepth=1;
        options.depthStencil=*decodeXenosDepthStencilState(0x700002,0,0);
        options.depthStencil->depthCompare=7; // ALWAYS; this fixture isolates the blend unit.
        for(auto control:controls) {
            options.guestBlendControl=control;
            const auto result=target->draw(program,input,3,{1,1,0,0},{},options,
                NativeTranslatedBlend::GuestRegister,clear,.75f,0);assert(result);
            for(unsigned i=0;i<result->size();++i) {
                if((*result)[i]!=expected(control,i%4)) {
                    std::fprintf(stderr,"BLEND_MISMATCH word=%08x samples=%u index=%u got=%g expected=%g\n",
                        control,samples,i,(*result)[i],expected(control,i%4));assert(false);
                }
            }
            ++cases;
        }
        const auto refuse=[&](const NativeTranslatedDrawProgram& p,const NativeTranslatedPipelineOptions& o,
                              NativeTranslatedBlend blend) {
            assert(!target->draw(p,input,3,{1,1,0,0},{},o,blend,clear,.75f,0));++rejected;
        };
        options.guestBlendControl.reset();refuse(program,options,NativeTranslatedBlend::GuestRegister);
        options.guestBlendControl=0x07060706;
        for(auto blend:{NativeTranslatedBlend::Replace,NativeTranslatedBlend::SourceAlpha,NativeTranslatedBlend::DestinationAlphaAdd})
            refuse(program,options,blend);
        auto wrongProgram=program;wrongProgram.abiVersion=4;refuse(wrongProgram,options,NativeTranslatedBlend::GuestRegister);
        auto wrong=options;wrong.colorAttachmentCount=4;refuse(program,wrong,NativeTranslatedBlend::GuestRegister);
        wrong=options;wrong.colorWriteMasks[0]=7;refuse(program,wrong,NativeTranslatedBlend::GuestRegister);
        for(auto word:{0x1000Cu,0x10002u,0x100A1u,0x20010001u}) {
            wrong=options;wrong.guestBlendControl=word;refuse(program,wrong,NativeTranslatedBlend::GuestRegister);
        }
        // Preserve the legacy alpha-replacement output exactly.
        options.guestBlendControl.reset();
        const auto legacy=target->draw(program,input,3,{1,1,0,0},{},options,
            NativeTranslatedBlend::SourceAlpha,clear,.75f,0);assert(legacy);
        for(unsigned i=0;i<legacy->size();++i)assert((*legacy)[i]==expected(0x00010706,i%4));
    }
    assert(cases==168 && rejected==33);
    std::printf("REGISTER_BLEND_GPU_PASS cases=%u rejections=%u separate_alpha=1 legacy_unchanged=1 authored_float_target_only=1 game_admission=0\n",cases,rejected);
}
