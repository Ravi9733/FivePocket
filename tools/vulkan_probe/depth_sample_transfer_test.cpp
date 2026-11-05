#include "native_color8_clear_target.h"
#include "xenos_float24_depth.h"
#include "image_sample_probe.h"
#include "process_limits.h"
#include <bit>
#include <cstdio>
#include <string>
using namespace recomp::gpu;
std::vector<unsigned> shader(const std::string& path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);assert(f);const auto n=f.tellg();assert(n>=20 && n%4==0 && n<=262144);f.seekg(0);
    std::vector<unsigned> b(n/4);assert(f.read(reinterpret_cast<char*>(b.data()),n));return b;
}
int main(int argc,char** argv) {
    assert(argc==2 && initializeProbeProcess());std::setvbuf(stdout,nullptr,_IONBF,0);const std::string prefix=argv[1];
    ShaderRequirements requirements;requirements.fragmentStoresAndAtomics=true;ProbeDevice device;
    assert(device.inspect() && device.create(requirements));
    const auto transferVertex=shader(prefix+"/color-sample-transfer.vert.spv");
    NativeTranslatedDrawProgram program;program.vertexCode=shader(prefix+"/depth-sample-source.vert.spv");
    program.pixelCode=shader(prefix+"/depth-blend-test.frag.spv");program.textureSlot=UINT32_MAX;
    constexpr unsigned width=17,height=19;unsigned cases=0,total=0;
    for(unsigned samples:{1u,2u,4u}) {
        const std::string suffix=samples==1?"":"-ms";
        const auto transferPixel=shader(prefix+"/depth-sample-transfer"+suffix+".frag.spv");
        const auto omitPixel=shader(prefix+"/depth-sample-transfer-omit"+suffix+".frag.spv");
        const auto invalidPixel=shader(prefix+"/depth-sample-transfer-invalid"+suffix+".frag.spv");
        const std::string readerSuffix=samples==1?"-1":"-ms";
        const auto depthCode=prefix+"/color-sample-read-depth"+readerSuffix+".spv";
        const auto stencilCode=prefix+"/color-sample-read-stencil"+readerSuffix+".spv";
        const auto colorCode=prefix+"/color-sample-read-bytes"+readerSuffix+".spv";
        Gta5Color8ClearPlan plan{0x1000,width,height,80,0,(height*(samples>=2?2:1)+15)/16*(samples==4?2:1),samples,1,{17,61,137,241},{}};
        plan.depth=Gta5Depth24ClearPlan{0x2000,width,height,80,128,plan.tileCount,samples,1,*planExactXenosFloat24Clear(.75,37)};
        auto destination=NativeColor8ClearTarget::create(device,plan);assert(destination);
        assert(!destination->clearColorPreservingDepthStencil(plan.color));
        auto source=NativeLinearSampleTarget::create(device,width,height,samples);assert(source);
        NativeTranslatedPipelineOptions options;options.samples=samples;options.minDepth=0;options.maxDepth=1;
        options.depthStencil=*decodeXenosDepthStencilState(0x76,0,0);
        unsigned caseIndex=0;
        for(const auto& parameters:std::array<std::array<float,2>,7>{{
            {0,0},{0x1p-35f,0x1p-36f},{0x1p-34f,0x1p-33f},{0x1p-16f,0x1p-19f},
            {0x1p-14f,0x1p-18f},{.1234567f,.01f},{.5f,.01f}}}) {
            const unsigned stencil=(37+caseIndex++*31)&255;
            assert(destination->clear(plan.color,*planExactXenosFloat24Clear(.75,stencil)) && destination->isDepthStencilUniform());
            std::vector<std::uint8_t> input(8208);const unsigned words[]{std::bit_cast<unsigned>(parameters[0]),std::bit_cast<unsigned>(parameters[1]),width,height};
            std::memcpy(input.data(),words,sizeof(words));
            assert(source->draw(program,input,3,{1,1,0,0},{},options,NativeTranslatedBlend::Replace,{0,0,0,1},0,0));
            SampleReader sourceReader(device,width,height,samples,source->borrowDepthStencilSamplesForReadOnlyDiagnostic(),
                VK_FORMAT_D32_SFLOAT_S8_UINT,VK_IMAGE_ASPECT_DEPTH_BIT,depthCode.c_str());
            const auto original=sourceReader.readWords();assert(original.size()==width*height*samples);
            assert(destination->replaceDepthFromNativeD32(source->borrowDepthStencilSamplesForReadOnlyDiagnostic(),width,height,samples,transferVertex,transferPixel));
            assert(!destination->isDepthStencilUniform());
            SampleReader depthReader(device,*destination,depthCode.c_str(),VK_IMAGE_ASPECT_DEPTH_BIT),
                stencilReader(device,*destination,stencilCode.c_str(),VK_IMAGE_ASPECT_STENCIL_BIT),
                colorReader(device,*destination,colorCode.c_str());
            const auto actual=depthReader.readWords();assert(actual.size()==original.size());
            for(unsigned i=0;i<actual.size();++i) {
                const auto encoded=truncateXenosFloat24Depth(std::bit_cast<float>(original[i]));assert(encoded);
                assert(actual[i]==std::bit_cast<unsigned>(*decodeXenosFloat24(*encoded)));++total;
            }
            stencilReader.checkWord(stencil);colorReader.check(plan.color);assert(sourceReader.readWords()==original);
            assert(destination->isUniformClear() && destination->readUniformResolved());
            const std::array<std::uint8_t,4> copiedClear{91,17,209,255};
            assert(destination->clearColorPreservingDepthStencil(copiedClear));
            assert(destination->isUniformClear() && !destination->isDepthStencilUniform());
            assert(destination->plan().color==copiedClear && depthReader.readWords()==actual);
            stencilReader.checkWord(stencil);colorReader.check(copiedClear);
            const auto resolvedClear=destination->readUniformResolved();assert(resolvedClear);
            for(unsigned i=0;i<resolvedClear->size();++i)assert((*resolvedClear)[i]==copiedClear[i%4]);
            assert(sourceReader.readWords()==original);
            assert(destination->clear(plan.color,*planExactXenosFloat24Clear(.75,stencil)));
            const bool underflow=std::any_of(original.begin(),original.end(),[](unsigned bits) {
                const float z=std::bit_cast<float>(bits);return z>0 && *truncateXenosFloat24Depth(z)==0;
            });
            assert(destination->replaceDepthFromNativeD32(source->borrowDepthStencilSamplesForReadOnlyDiagnostic(),width,height,samples,
                transferVertex,transferPixel,true)==!underflow);
            if(underflow)assert(!destination->readUniformResolved() && !destination->isDepthStencilUniform());
            assert(destination->clear(plan.color,*planExactXenosFloat24Clear(.75,stencil)));
            assert(destination->replaceDepthFromNativeD32(source->borrowDepthStencilSamplesForReadOnlyDiagnostic(),width,height,samples,transferVertex,transferPixel));
            // Uninitialized destination is rejected before submission, so
            // existing stencil cannot be discarded by a borrowed transfer.
            auto copy=NativeColorSampleTransfer::createDepthPreservingStencil(device,width,height,samples,
                source->borrowDepthStencilSamplesForReadOnlyDiagnostic(),destination->borrowDepthForReadOnlyDiagnostic(),transferVertex,transferPixel);
            assert(copy && !copy->transfer(0,false));
            assert(depthReader.readWords()==actual);stencilReader.checkWord(stencil);colorReader.check(plan.color);
            assert(destination->clear(plan.color,*planExactXenosFloat24Clear(.75,stencil)));
            for(const auto& bad:{omitPixel,invalidPixel}) {
                assert(!destination->replaceDepthFromNativeD32(source->borrowDepthStencilSamplesForReadOnlyDiagnostic(),width,height,samples,transferVertex,bad));
                assert(!destination->isDepthStencilUniform() && !destination->readUniformResolved());
                assert(!destination->clearColorPreservingDepthStencil(copiedClear));
                assert(destination->clear(plan.color,*planExactXenosFloat24Clear(.75,stencil)));
                depthReader.checkWord(std::bit_cast<unsigned>(.75f));stencilReader.checkWord(stencil);colorReader.check(plan.color);
            }
            assert(!destination->replaceDepthFromNativeD32(source->borrowDepthStencilSamplesForReadOnlyDiagnostic(),width+1,height,samples,transferVertex,transferPixel));
            depthReader.checkWord(std::bit_cast<unsigned>(.75f));++cases;
        }
        std::printf("DEPTH_SAMPLE_TRANSFER_CASE_PASS samples=%u cases=7 source_unchanged=1 color_stencil_preserved=1 omitted_invalid_rejected=1 positive_underflow_guard=1 color_only_clear_preserves_depth_stencil=1\n",samples);
    }
    std::printf("DEPTH_SAMPLE_TRANSFER_PASS cases=%u individual_samples=%u truncate_cpu_exact=1 depth_game_admission=0\n",cases,total);
}
