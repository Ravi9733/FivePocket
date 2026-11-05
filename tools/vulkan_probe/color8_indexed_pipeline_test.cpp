#include "native_color8_clear_target.h"
#include "xenos_color8_render.h"
#include "xenos_float24_depth.h"
#include "image_sample_probe.h"
#include "process_limits.h"
#include <bit>
#include <cstdio>
using namespace recomp::gpu;
namespace {
std::vector<unsigned> shader(const std::string& path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);assert(f);const auto n=f.tellg();assert(n>=20 && n%4==0 && n<=262144);f.seekg(0);
    std::vector<unsigned> result(n/4);assert(f.read(reinterpret_cast<char*>(result.data()),n));return result;
}
void word(std::vector<std::uint8_t>& b,unsigned at,unsigned w) {for(unsigned c=0;c<4;++c)b.at(at+c)=w>>(24-8*c);}
void vertex(std::vector<std::uint8_t>& input,unsigned i,float z,const std::array<float,4>& color) {
    const std::array<std::array<float,2>,3> positions{{{-1,-1},{3,-1},{-1,3}}};
    const std::array<float,4> p{positions[i%3][0],positions[i%3][1],z,1};
    for(unsigned c=0;c<4;++c){word(input,8208+i*32+c*4,std::bit_cast<unsigned>(p[c]));word(input,8208+i*32+16+c*4,std::bit_cast<unsigned>(color[c]));}
}
}
int main(int argc,char** argv) {
    assert(argc==2 && initializeProbeProcess());std::setvbuf(stdout,nullptr,_IONBF,0);const std::string prefix=argv[1];
    ShaderRequirements requirements;requirements.fragmentStoresAndAtomics=true;
    requirements.vertexStoresAndAtomics=true;
    ProbeDevice device;assert(device.inspect() && device.create(requirements,false,true));
    assert(device.properties.limits.standardSampleLocations);
    unsigned cases=0;
    for(unsigned format:{0u,1u})for(unsigned samples:{1u,2u,4u}) {
        std::printf("COLOR8_INDEXED_BEGIN format=%u samples=%u\n",format,samples);
        constexpr unsigned width=8,height=8;
        Gta5Color8ClearPlan plan{0x1000,width,height,80,0,(height*(samples>=2?2:1)+15)/16*(samples==4?2:1),samples,format,{17,61,137,241},{}};
        plan.depth=Gta5Depth24ClearPlan{0x2000,width,height,80,128,plan.tileCount,samples,1,*planExactXenosFloat24Clear(0,37)};
        auto target=NativeColor8ClearTarget::create(device,plan);assert(target && target->clear(plan.color));
        const std::string suffix=samples==1?"-1":"-ms";
        SampleReader colorReader(device,*target,(prefix+"/read-color"+suffix+".spv").c_str());
        SampleReader depthReader(device,*target,(prefix+"/read-depth"+suffix+".spv").c_str(),VK_IMAGE_ASPECT_DEPTH_BIT);
        SampleReader stencilReader(device,*target,(prefix+"/read-stencil"+suffix+".spv").c_str(),VK_IMAGE_ASPECT_STENCIL_BIT);
        NativeTranslatedDrawProgram program;program.vertexCode=shader(prefix+"/indexed_depth_test.vert.spv");
        program.pixelCode=shader(prefix+"/color8-"+std::to_string(format)+".frag.spv");
        program.stride=32;program.indexed=true;program.abiVersion=13;program.geometryColor8Format=format;program.geometryDepth20e4=true;program.textureSlot=UINT32_MAX;
        program.geometrySampleDepth=true;
        NativeTranslatedPipelineOptions options;options.samples=samples;options.minDepth=0;options.maxDepth=1;options.sampleShading=true;
        XenosDepthStencilState z;z.depthEnable=z.depthWrite=true;z.depthCompare=4;options.depthStencil=z;
        std::vector<std::uint8_t> input(8208+6*32);word(input,0,6);
        const std::array<float,4> red{1,0,0,1},green{0,1,0,1};
        const auto set=[&](float a,float b,const std::array<float,4>& c=std::array<float,4>{1,0,0,1}) {
            for(unsigned i=0;i<6;++i)vertex(input,i,i<3?a:b,i<3?c:green);
        };
        const auto draw=[&](std::vector<unsigned> indices,std::array<unsigned,4> scissor=std::array<unsigned,4>{0,0,width,height}) {
            ++cases;return target->drawIndexedColor8(program,input,6,indices,{1,1,0,0},scissor,{},options);
        };
        // These depths differ as float32 but are EQUAL in20e4. Later GREATER
        // fragments must fail both across draws AND inside the same index list.
        set(std::bit_cast<float>(0x3F000002u),std::bit_cast<float>(0x3F000004u));
        assert(draw({2,0,1}));assert(!target->isDepthStencilUniform());
        colorReader.check(*packXenosColor8Render(red,format));depthReader.checkWord(0x3F000000u);stencilReader.checkWord(37);
        assert(draw({5,3,4}));colorReader.check(*packXenosColor8Render(red,format));
        assert(target->clear(plan.color));assert(draw({2,0,1,5,3,4}));colorReader.check(*packXenosColor8Render(red,format));
        set(.5f,std::bit_cast<float>(0x3F000008u));assert(draw({5,3,4}));
        colorReader.check(*packXenosColor8Render(green,format));depthReader.checkWord(0x3F000008u);
        const auto priorDepth=depthReader.readWords();assert(target->clearColorPreservingDepthStencil(plan.color));
        assert(depthReader.readWords()==priorDepth && draw({2,0,1}));colorReader.check(plan.color);
        // PWL thresholds, alpha half-way ties and saturating source values.
        for(float f:{-1.f,0.f,17.f/255.f,63.f/1023.f,64.f/1023.f,127.f/1023.f,128.f/1023.f,511.f/1023.f,512.f/1023.f,.75f,1.f,2.f}) {
            const std::array<float,4> value{f,f,f,.5f};set(.5f,.25f,value);
            assert(target->clear(plan.color) && draw({2,0,1}));colorReader.check(*packXenosColor8Render(value,format));
        }
        assert(target->clear(plan.color));set(.5f,.25f);assert(draw({2,0,1},{2,1,3,5}));
        const auto pixels=colorReader.readWords(),depths=depthReader.readWords();
        const auto packed=[](const std::array<std::uint8_t,4>& c){return unsigned(c[0])|(unsigned(c[1])<<8)|(unsigned(c[2])<<16)|(unsigned(c[3])<<24);};
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)for(unsigned s=0;s<samples;++s) {
            const bool inside=x>=2 && x<5 && y>=1 && y<6;const auto at=(y*width+x)*samples+s;
            assert(pixels[at]==packed(inside?*packXenosColor8Render(red,format):plan.color));
            assert(depths[at]==(inside?0x3F000000u:0u));
        }
        stencilReader.checkWord(37);
        // Connected indexed writer -> scratch depth/stencil copy -> read-only
        // comparison. Historical clear values must not replace either aspect.
        assert(target->clear(plan.color));set(.5f,.25f);
        options.depthStencil->stencilEnable=true;
        options.depthStencil->front={7,0,2,0,1,255,255};
        assert(draw({2,0,1},{2,1,3,5}));
        const auto storedDepth=depthReader.readWords(),storedStencil=stencilReader.readWords();
        auto scratch=NativeLinearSampleTarget::create(device,width,height,samples);assert(scratch);
        auto plain=program;plain.abiVersion=12;plain.pixelCode=shader(prefix+"/indexed_depth_test.frag.spv");
        plain.vertexCode=shader(prefix+"/plain_indexed_depth_test.vert.spv");
        auto readonly=options;readonly.sampleShading=false;readonly.depthStencil->depthWrite=false;
        readonly.depthStencil->front={2,0,0,0,37,255,0};
        assert(scratch->draw(plain,input,6,{1,1,0,0},{},readonly,NativeTranslatedBlend::Replace,{0,0,0,0},0,99,false,{2,0,1}));
        for(unsigned copy=0;copy<2;++copy) {
            assert(scratch->replaceDepthStencilFromNative(target->borrowDepthForReadOnlyDiagnostic(),width,height,samples));
            assert(!scratch->replaceDepthStencilFromNative(target->borrowDepthForReadOnlyDiagnostic(),width+1,height,samples));
            SampleReader copiedDepth(device,width,height,samples,scratch->borrowDepthStencilSamplesForReadOnlyDiagnostic(),VK_FORMAT_D32_SFLOAT_S8_UINT,
                VK_IMAGE_ASPECT_DEPTH_BIT,(prefix+"/read-depth"+suffix+".spv").c_str());
            SampleReader copiedStencil(device,width,height,samples,scratch->borrowDepthStencilSamplesForReadOnlyDiagnostic(),VK_FORMAT_D32_SFLOAT_S8_UINT,
                VK_IMAGE_ASPECT_STENCIL_BIT,(prefix+"/read-stencil"+suffix+".spv").c_str());
            assert(copiedDepth.readWords()==storedDepth && copiedStencil.readWords()==storedStencil);
            const auto shaded=scratch->draw(plain,input,6,{1,1,0,0},{},readonly,NativeTranslatedBlend::Replace,{0,0,0,0},0,255,true,{5,3,4},true);
            assert(shaded);
            for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){const bool blocked=x>=2 && x<5 && y>=1 && y<6;
                assert((*shaded)[(y*width+x)*4+1]==(blocked?0.f:1.f));}
            assert(copiedDepth.readWords()==storedDepth && copiedStencil.readWords()==storedStencil);
        }
        assert(depthReader.readWords()==storedDepth && stencilReader.readWords()==storedStencil);
        options.depthStencil=z;
        std::printf("COLOR8_INDEXED_SCRATCH_DEPTH_PASS format=%u samples=%u both_aspects_exact=1 prior_geometry_occludes=1 source_preserved=1\n",format,samples);
        // Slanted surface, including each native sample and non-default depth
        // ranges. Unlike flat-Z fixtures this detects pixel-center collapse.
        for(const std::array<float,2> range: {std::array<float,2>{0,1},{1,0},{.25f,.75f},{1,1}}) {
            assert(target->clear(plan.color));options.minDepth=range[0];options.maxDepth=range[1];
            for(unsigned i=0;i<3;++i)vertex(input,i,std::array<float,3>{.25f,.75f,.5f}[i],red);
            auto reference=NativeLinearSampleTarget::create(device,width,height,samples);assert(reference);
            auto referenceOptions=options;referenceOptions.sampleShading=false;
            assert(reference->draw(plain,input,6,{1,1,0,0},{},referenceOptions,NativeTranslatedBlend::Replace,{0,0,0,1},0,37,false,{0,1,2}));
            SampleReader referenceDepth(device,width,height,samples,reference->borrowDepthStencilSamplesForReadOnlyDiagnostic(),VK_FORMAT_D32_SFLOAT_S8_UINT,
                VK_IMAGE_ASPECT_DEPTH_BIT,(prefix+"/read-depth"+suffix+".spv").c_str());
            assert(draw({0,1,2}));const auto actual=depthReader.readWords(),native=referenceDepth.readWords();
            unsigned distinct=0;
            for(unsigned i=0;i<actual.size();++i) {
                // Standard native sample positions and an exact binary-rational
                // plane: z=.25+(x+y)/8192 in the2048x1024 padded viewport.
                // The fixed depth unit can land one float32 ulp below this
                // exact result. Do not treat that as the sole truth oracle.
                const std::array<std::array<float,2>,4> s4{{{.375f,.125f},{.875f,.375f},{.125f,.625f},{.625f,.875f}}};
                const auto at=samples==1?std::array<float,2>{.5f,.5f}:samples==2?
                    (i%2?std::array<float,2>{.25f,.25f}:std::array<float,2>{.75f,.75f}):s4[i%4];
                const unsigned pixel=i/samples;const float ndc=.25f+(float(pixel%width)+at[0]+float(pixel/width)+at[1])/8192.f;
                const auto cpu=*truncateXenosFloat24Depth(ndc*(range[1]-range[0])+range[0]);
                const auto expected=std::bit_cast<unsigned>(*decodeXenosFloat24(cpu));
                assert(actual[i]==expected);
                const auto fixed=*truncateXenosFloat24Depth(std::bit_cast<float>(native[i]));
                assert((cpu>fixed?cpu-fixed:fixed-cpu)<=1);
                if(i%samples && actual[i]!=actual[i-i%samples])++distinct;
            }
            if(samples>1 && range[0]!=range[1])assert(distinct>0);
            const auto prior=colorReader.readWords();assert(draw({0,1,2}));assert(depthReader.readWords()==actual && colorReader.readWords()==prior);
        }
        options.minDepth=0;options.maxDepth=1;
        assert(target->clear(plan.color));set(.5f,.25f);word(input,8208+12,0);
        assert(!draw({0,1,2}) && !target->borrowSamplesForReadOnlyDiagnostic());
        assert(target->clear(plan.color));set(.5f,.25f);word(input,8208,std::bit_cast<unsigned>(std::numeric_limits<float>::infinity()));
        assert(!draw({0,1,2}) && !target->borrowDepthForReadOnlyDiagnostic());
        assert(target->clear(plan.color));set(.5f,.25f);program.geometrySampleDepth=false;
        assert(!draw({0,1,2}));program.geometrySampleDepth=true;
        assert(target->clear(plan.color));device.vertexStoresAndAtomicsEnabled=false;
        assert(!draw({0,1,2}));device.vertexStoresAndAtomicsEnabled=true;assert(target->clear(plan.color));
        std::printf("COLOR8_INDEXED_SAMPLE_DEPTH_PASS format=%u samples=%u slanted=1 ranges=4 per_sample_cpu_exact=1 native_within_one_code=1 repeated_ties_preserved=1 invalid_vertex_and_legacy_rejected=1\n",format,samples);
        // Nonfinite shader output invalidates BOTH attachments for publication.
        set(.5f,.25f,{std::bit_cast<float>(0x7FC00000u),0,0,1});
        assert(!draw({2,0,1}) && !target->borrowSamplesForReadOnlyDiagnostic() && !target->borrowDepthForReadOnlyDiagnostic());
        assert(target->clear(plan.color));set(.5f,.25f);
        auto saved=program;program.abiVersion=12;assert(!draw({2,0,1}));program=saved;
        assert(!draw({0,1,6}));assert(!target->borrowSamplesForReadOnlyDiagnostic());assert(target->clear(plan.color));
        std::printf("COLOR8_INDEXED_CASE_PASS format=%u samples=%u pretest20e4=1 interdraw_depth=1 intradraw_depth=1 color_exact=1 scissor=1 invalid_rejected=1\n",format,samples);
    }
    std::printf("COLOR8_INDEXED_GPU_PASS cases=%u native_policy=1 gameplay=0\n",cases);
}
