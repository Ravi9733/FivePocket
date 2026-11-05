#include "multisample_draw_probe.h"
#include "image_sample_probe.h"
#include "process_limits.h"
#include "xenos_color8_render.h"
#include <cstdio>
#include <limits>
using namespace recomp::gpu;
std::vector<std::uint32_t> shader(const std::string& path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);assert(f);const auto n=f.tellg();assert(n>=20 && n<=262144 && n%4==0);
    std::vector<std::uint32_t> result(n/4);f.seekg(0);assert(f.read(reinterpret_cast<char*>(result.data()),n));return result;
}
int main(int argc,char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);assert(argc==2 && initializeProbeProcess());const std::string directory=argv[1];
    const auto path=[&](const std::string& name){return directory+"/"+name+".spv";};
    const auto vertex=shader(path("color-sample-transfer.vert"));
    const auto snapshotCode=shader(path("color-sample-snapshot.comp")),snapshotOmit=shader(path("color-sample-snapshot-omit.comp"));
    NativeTranslatedDrawProgram program;program.vertexCode=vertex;program.pixelCode=shader(path("color-sample-source.frag"));program.textureSlot=UINT32_MAX;
    ShaderRequirements requirements;requirements.fragmentStoresAndAtomics=true;
    ProbeDevice d;assert(d.inspect() && d.create(requirements) && d.fragmentStoresAndAtomicsEnabled);
    std::vector<std::array<float,4>> palette;
    for(unsigned i=0;i<256;++i)palette.push_back({xenosPwlGammaByteToLinear(i),xenosPwlGammaByteToLinear(255-i),xenosPwlGammaByteToLinear(i^0xA5),float(i)/255.f});
    for(float f:{0.f,1.f,64.f/1023.f,128.f/1023.f,512.f/1023.f})
        for(float value:{std::nextafter(f,-std::numeric_limits<float>::infinity()),f,std::nextafter(f,std::numeric_limits<float>::infinity())})
            palette.push_back({value,value,value,.5f});
    for(unsigned i=0;i<255;++i)palette.push_back({.5f,.75f/1023.f,2.5f/1023.f,(float(i)+.5f)/255.f});
    assert(palette.size()*16+16<=NativeTranslatedResources::InputBytes);
    std::uint64_t checked=0;unsigned cases=0;
    for(unsigned samples:{1u,2u,4u}) {
        const auto suffix=samples==1?"1":"ms";
        const auto encode=shader(path(std::string("color-sample-encode-")+suffix+".frag"));
        const auto decode=shader(path(std::string("color-sample-decode-")+suffix+".frag"));
        const auto omitted=shader(path(std::string("color-sample-omit-")+suffix+".frag"));
        const auto nonfinite=shader(path(std::string("color-sample-nonfinite-")+suffix+".frag"));
        const auto floatPath=path(std::string("color-sample-read-float-")+suffix);
        const auto bytesPath=path(std::string("color-sample-read-bytes-")+suffix);
        const auto depthPath=path(std::string("color-sample-read-depth-")+suffix);
        const auto stencilPath=path(std::string("color-sample-read-stencil-")+suffix);
        auto linear=MultisampleDrawProbe::create(d,256,257,samples);assert(linear);
        NativeTranslatedPipelineOptions options;options.samples=samples;options.minDepth=0;options.maxDepth=1;
        options.depthStencil=*decodeXenosDepthStencilState(0,0,0);
        for(unsigned format:{0u,1u}) {
            Gta5Color8ClearPlan plan{0x1000,256,257,320,0,(samples==4?8u:4u)*((257*(samples>=2?2:1)+15)/16),samples,format,{17,31,63,255},{}};
            plan.depth=Gta5Depth24ClearPlan{0x2000,256,257,320,plan.tileCount,plan.tileCount,samples,1,*planExactXenosFloat24Clear(0,255)};
            auto encoded=NativeColor8ClearTarget::create(d,plan);assert(encoded && encoded->clear(plan.color));
            SampleReader packedReader(d,*encoded,bytesPath.c_str());
            SampleReader depthReader(d,*encoded,depthPath.c_str(),VK_IMAGE_ASPECT_DEPTH_BIT);
            SampleReader stencilReader(d,*encoded,stencilPath.c_str(),VK_IMAGE_ASPECT_STENCIL_BIT);
            for(unsigned mode:{0u,1u}) {
                std::vector<std::uint8_t> input(std::max(std::size_t{8208},16+palette.size()*16));std::memcpy(input.data(),&mode,4);
                const unsigned count=palette.size();std::memcpy(input.data()+4,&count,4);std::memcpy(input.data()+16,palette.data(),palette.size()*16);
                const auto drawn=linear->draw(program,input,3,{1,1,0,0},{},options,NativeTranslatedBlend::Replace,{0,0,0,1},0,255);assert(drawn);
                SampleReader floatReader(d,256,257,samples,linear->borrowColorSamplesForReadOnlyDiagnostic(),VK_FORMAT_R32G32B32A32_SFLOAT,
                    VK_IMAGE_ASPECT_COLOR_BIT,floatPath.c_str(),5);
                const auto original=floatReader.readWords();assert(original.size()==256*257*samples*5);
                assert(encoded->replaceSamplesFromLinear(linear->borrowColorSamplesForReadOnlyDiagnostic(),256,257,samples,vertex,encode));
                assert(!encoded->isUniformClear() && !encoded->readUniformResolved());
                const auto packed=packedReader.readWords();assert(packed.size()==256*257*samples);
                for(unsigned i=0;i<packed.size();++i) {
                    assert(original[i*5+4]==0xC01AC01Au);std::array<float,4> color{};std::memcpy(color.data(),original.data()+i*5,16);
                    const auto cpu=packXenosColor8Render(color,format);assert(cpu);
                    const auto expected=std::uint32_t((*cpu)[0])|(std::uint32_t((*cpu)[1])<<8)|(std::uint32_t((*cpu)[2])<<16)|(std::uint32_t((*cpu)[3])<<24);
                    if(packed[i]!=expected)std::printf("MISMATCH sample=%u format=%u mode=%u input=%g,%g,%g,%g got=%08x expected=%08x\n",i,format,mode,color[0],color[1],color[2],color[3],packed[i],expected);
                    assert(packed[i]==expected);
                }
                assert(linear->replaceColorFromEncoded(encoded->borrowSamplesForReadOnlyDiagnostic(),format,vertex,decode));
                const auto decoded=floatReader.readWords();
                assert(encoded->replaceSamplesFromLinear(linear->borrowColorSamplesForReadOnlyDiagnostic(),256,257,samples,vertex,encode));
                const auto roundtrip=packedReader.readWords();unsigned differences=0;
                for(unsigned i=0;i<packed.size();++i)if(roundtrip[i]!=packed[i]) {
                    if(differences<8)for(unsigned c=0;c<4;++c)if(std::uint8_t(roundtrip[i]>>(8*c))!=std::uint8_t(packed[i]>>(8*c))) {
                        float actual;std::memcpy(&actual,&decoded[i*5+c],4);const auto byte=std::uint8_t(packed[i]>>(8*c));
                        const auto expected=format && c<3?xenosPwlGammaByteToLinear(byte):float(byte)/255.f;
                        std::printf("ROUNDTRIP_DIFFERENCE samples=%u format=%u mode=%u pixel=%u channel=%u byte=%u new=%u decoded=%.12g cpu=%.12g\n",
                            samples,format,mode,i,c,byte,std::uint8_t(roundtrip[i]>>(8*c)),actual,expected);
                    }
                    ++differences;
                }
                assert(!differences);depthReader.checkWord(0);stencilReader.checkWord(255);
                // Invalid shape/format rejects before writes and preserves bytes.
                assert(!encoded->replaceSamplesFromLinear(linear->borrowColorSamplesForReadOnlyDiagnostic(),255,257,samples,vertex,encode));
                assert(packedReader.readWords()==packed);
                assert(!linear->replaceColorFromEncoded(encoded->borrowSamplesForReadOnlyDiagnostic(),2,vertex,decode));
                // Three separate, single-coverage draws. Decode actual previous
                // bytes, LOAD color (do not clear it), blend, encode each draw.
                // Dyadic source factors avoid an ambiguous separate-vs-FMA
                // oracle while covering nonuniform destination alpha/colors.
                auto previous=packed;const unsigned blendMode=2;std::memcpy(input.data(),&blendMode,4);
                for(unsigned pass=0;pass<3;++pass) {
                    assert(linear->replaceColorFromEncoded(encoded->borrowSamplesForReadOnlyDiagnostic(),format,vertex,decode));
                    assert(linear->draw(program,input,3,{1,1,0,0},{},options,NativeTranslatedBlend::DestinationAlphaAdd,{-100,-100,-100,-100},0,255,true));
                    assert(encoded->replaceSamplesFromLinear(linear->borrowColorSamplesForReadOnlyDiagnostic(),256,257,samples,vertex,encode));
                    const auto current=packedReader.readWords();
                    for(unsigned i=0;i<current.size();++i) {
                        const float alpha=float(std::uint8_t(previous[i]>>24))/255.f;
                        std::array<float,4> color{0,0,0,.75f};
                        for(unsigned c=0;c<3;++c) {
                            const auto byte=std::uint8_t(previous[i]>>(8*c));
                            const float old=format?xenosPwlGammaByteToLinear(byte):float(byte)/255.f;
                            color[c]=old+(.125f*float(1u<<c))*alpha;
                        }
                        const auto cpu=*packXenosColor8Render(color,format);
                        const auto expected=std::uint32_t(cpu[0])|(std::uint32_t(cpu[1])<<8)|(std::uint32_t(cpu[2])<<16)|(std::uint32_t(cpu[3])<<24);
                        if(current[i]!=expected)std::printf("PERSISTENT_BLEND_MISMATCH samples=%u format=%u mode=%u pass=%u sample=%u got=%08x expected=%08x\n",samples,format,mode,pass,i,current[i],expected);
                        assert(current[i]==expected);
                    }
                    previous=current;
                }
                depthReader.checkWord(0);stencilReader.checkWord(255);
                const auto snapshot=encoded->snapshotEncodedSamples(snapshotCode);
                if(samples==1)assert(!snapshot);
                else {
                    assert(snapshot && snapshot->size()==previous.size()*4);
                    for(unsigned i=0;i<previous.size();++i)for(unsigned c=0;c<4;++c)assert((*snapshot)[i*4+c]==std::uint8_t(previous[i]>>(8*c)));
                    assert(!encoded->snapshotEncodedSamples(snapshotOmit));assert(packedReader.readWords()==previous);
                }
                checked+=packed.size();++cases;
            }
            for(const auto& negative:{omitted,nonfinite}) {
                assert(!encoded->replaceSamplesFromLinear(linear->borrowColorSamplesForReadOnlyDiagnostic(),256,257,samples,vertex,negative));
                assert(!encoded->borrowSamplesForReadOnlyDiagnostic() && !encoded->readUniformResolved() && !encoded->isUniformClear());
                assert(encoded->clear(plan.color));packedReader.check(plan.color);depthReader.checkWord(0);stencilReader.checkWord(255);
            }
            std::printf("COLOR_SAMPLE_TRANSFER_CASE_PASS samples=%u format=%u modes=2 repeated_blended_draws=3 negative_modules=2 depth_stencil_preserved=1\n",samples,format);
        }
    }
    std::printf("COLOR_SAMPLE_TRANSFER_GPU_PASS cases=%u individual_samples=%llu render_quantization_exact=1 roundtrip_exact=1 game_draw=0\n",cases,static_cast<unsigned long long>(checked));
}
