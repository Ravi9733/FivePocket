// Exact original VS/PS/depth texture with an AUTHORED destination/depth owner.
// Tests connected sampling/raster/blend and preservation, NOT live game output.
#ifndef RECOMP_PRIVATE_STRIP_RASTER_PACK
#error An explicit current private screen-strip pack is required
#endif
#include RECOMP_PRIVATE_STRIP_RASTER_PACK
#include "native_screen_strip_plan.h"
#include "native_linear_sample_target.h"
#include "native_texture_image.h"
#include "xenos_color_7e3.h"
#include "native_screen_strip_draw.h"
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
    const auto put=[](auto& b,unsigned at,unsigned value){for(unsigned i=0;i<4;++i)b[at+i]=value>>(24-8*i);};
    Gta5InlineDraw draw;draw.primitive=6;draw.vertexCount=100;draw.stride=8;
    draw.vertices=read("vertices.bin",800);draw.deviceState=read("device-state.bin",13860);
    draw.vertexShader=be(draw.deviceState,13072);draw.pixelShader=be(draw.deviceState,13068);
    const auto vc=read("vertex-constants.bin",4096),pc=read("pixel-constants.bin",4096),meta=read("strip-meta.bin",36);
    for(unsigned i=0;i<1024;++i){draw.vertexConstants[i]=be(vc,i*4);draw.pixelConstants[i]=be(pc,i*4);}
    draw.scissorRegisters=std::array<std::uint32_t,3>{be(meta,0),be(meta,4),be(meta,8)};
    Gta5InlineDraw::Texture t;t.slot=3;for(unsigned i=0;i<6;++i)t.fetch[i]=be(meta,12+i*4);
    t.source=read("texture-3.bin",3768320);draw.textures={t};
    Gta5BoundSurfaceCapture bound;for(unsigned i=0;i<5;++i)bound.surfaces[i].object=be(draw.deviceState,12816+i*4);
    bound.surfaces[0].header=read("surface-0-argument.bin",40);bound.surfaces[4].header=read("surface-4-argument.bin",40);
    const auto vs=read("vertex-shader.bin",300),ps=read("pixel-shader.bin",492);
    const NativeScreenStripIdentity identity{screenStripRaster.vertexIdentity,screenStripRaster.pixelIdentity};
    const char* why=nullptr;const auto plan=planNativeScreenStrip(draw,bound,identity,vs,ps,false,true,&why);
    assert(plan && validateNativeScreenStripProgram(screenStripRaster));
    const auto reference=read("pixel-reference.bin",80*24);
    const auto zeroReference=read("zero-depth-reference.bin",80*24);
    std::vector<std::uint8_t> input(8208+800);put(input,0,100);
    std::copy(vc.begin(),vc.end(),input.begin()+16);std::copy(pc.begin(),pc.end(),input.begin()+4112);
    std::copy(draw.vertices.begin(),draw.vertices.end(),input.begin()+8208);
    ProbeDevice device;ShaderRequirements req;assert(device.inspect() && device.create(req));
    auto image=NativeTextureImage::createDepth24Float(device,plan->texture,t.source);assert(image);
    VkSampler sampler{};VkSamplerCreateInfo s{};s.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    s.minFilter=s.magFilter=VK_FILTER_NEAREST;s.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    s.addressModeU=s.addressModeV=s.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    assert(vkCreateSampler(device.device,&s,nullptr,&sampler)==VK_SUCCESS);
    NativeTranslatedPipelineOptions options;options.depthStencil=plan->depthStencil;
    options.colorWriteMasks={7,0,0,0};options.externalTextures={{sampler,image->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    for(unsigned bad=0;bad<6;++bad) {
        auto p=screenStripRaster;auto o=options;
        if(bad==0)p.screenStrip=false;
        if(bad==1)p.abiVersion=6;
        if(bad==2)o.colorWriteMasks[0]=15;
        if(bad==3)o.samples=2;
        if(bad==4)o.depthStencil->depthWrite=true;
        if(bad==5)o.externalTextures.clear();
        NativeTranslatedResources r(device.device);
        assert(!r.setup(device,VK_NULL_HANDLE,p,{plan->texture},NativeTranslatedBlend::ScreenStripSourceOne,o));
    }
    constexpr unsigned Pixels=1280*720;
    std::vector<float> initial(Pixels*4);std::vector<std::uint32_t> depths(Pixels);
    for(unsigned i=0;i<Pixels;++i) {
        initial[i*4]=1;initial[i*4+1]=.5f;initial[i*4+2]=.25f;initial[i*4+3]=float(i%4)/3.f;
        // Class0 passes; class1 fails stencil; class2 fails depth; class3
        // exercises the read mask on nontrivial full8-bit stencil values.
        depths[i]=(i%4==2?0xF0000000u:0u)|(i%4==1?7u:i%4==3?255u:0u);
    }
    for(bool authored:{true,false}) {
        auto target=NativeLinearSampleTarget::create(device,1280,720,1);assert(target);
        assert(target->loadSingleSampleContents(initial,depths));
        assert(target->readSingleSampleDepthWords()==std::optional{depths});
        auto program=screenStripRaster;
        if(authored)program.pixelCode=screenStripAuthoredPixel; // Explicit test-only colour oracle; not guest identity admission.
        auto result=target->draw(program,input,294,plan->viewport,{plan->texture},options,
            NativeTranslatedBlend::ScreenStripSourceOne,{0,0,0,0},0,0,true,{},true);
        if(!result)std::printf("STRIP_GPU_FAIL authored=%u stage=%s\n",unsigned(authored),target->lastDrawFailure());
        assert(result && result->size()==Pixels*4);
        const auto afterDepth=target->readSingleSampleDepthWords();assert(afterDepth && *afterDepth==depths);
        unsigned changed=0;
        for(unsigned i=0;i<Pixels;++i) {
            assert((*result)[4*i+3]==initial[4*i+3]);
            bool any=false;
            for(unsigned c=0;c<3;++c) {
                const auto got=(*result)[4*i+c];assert(std::isfinite(got));any|=got!=initial[4*i+c];
                if(i%4)assert(got==initial[4*i+c]);
                else if(authored)assert(got==(c==1?.5f:.625f));
            }
            changed+=any;
        }
        assert(changed>0);if(authored)assert(changed==Pixels/4);
        float maximumError=0;
        if(!authored)for(unsigned i=0;i<80;++i) {
            const unsigned x=be(reference,i*24),y=be(reference,i*24+4),at=(y*1280+x)*4;
            assert(x<1280 && y<720);
            for(unsigned c=0;c<3;++c) {
                const float source=std::bit_cast<float>(be(reference,i*24+8+c*4));
                const float alpha=std::clamp(std::bit_cast<float>(be(reference,i*24+20)),0.f,1.f);
                const float want=x%4?initial[at+c]:source+initial[at+c]*(1.f-alpha);
                const float error=std::abs((*result)[at+c]-want);maximumError=std::max(maximumError,error);
                if(error>0.00002f)std::printf("STRIP_PIXEL_MISMATCH x=%u y=%u c=%u got=%.9g want=%.9g error=%.9g\n",
                    x,y,c,(*result)[at+c],want,error);
                assert(error<=0.00002f);
            }
        }
        std::printf("SCREEN_STRIP_RASTER_GPU_PASS authored=%u vertices=100 triangles=98 pixels=921600 "
            "changed=%u reference_pixels=%u max_abs_error=%.9g preserved_alpha_depth_stencil=1 "
            "destination_authored=1 physical_publication=0 gameplay=0\n",unsigned(authored),changed,authored?0u:80u,maximumError);
    }
    // Production connected consumer, with explicit AUTHORED physical owners.
    // This is deliberately the same entry point used by the eventual live
    // dispatcher; no untested alternative publication path in this probe.
    std::vector<std::uint8_t> physicalColor(Pixels*4);
    for(unsigned i=0;i<Pixels;++i) {
        std::uint32_t word=0;assert(packColor7e3({colorBits(1),colorBits(.5f),colorBits(.25f),colorBits(float(i%4)/3.f)},word));
        for(unsigned c=0;c<4;++c)physicalColor[i*4+c]=word>>(8*c);
    }
    const auto seed=[&] {
        NativeEdram32Bands owner;
        assert(owner.recordCompletedRawColorRows(720,1280,1280,720,physicalColor,1));
        assert(owner.recordCompletedDepthWords(0,1280,1280,720,depths,2));
        assert(owner.recordCompletedRawColorRows(1500,80,80,16,std::vector<std::uint8_t>(5120,0xB6),3));
        return owner;
    };
    for(unsigned mode=0;mode<4;++mode) {
        auto owner=seed();const auto before=owner.readColorWords(720,1280,1280,720);
        const auto unrelated=owner.readColorWords(1500,80,80,16);const auto serial=owner.nextSerial();
        auto submitted=draw;auto program=nativeScreenStripProgram;
        //0 original shaders+captured texture,1 original shaders+authored zero
        //depth texture,2/3 authored PS test preblend alpha clamping at both ends.
        if(mode==1)std::fill(submitted.textures[0].source.begin(),submitted.textures[0].source.end(),0);
        if(mode==2)program.draw.pixelCode=screenStripClampHighPixel;
        if(mode==3)program.draw.pixelCode=screenStripClampLowPixel;
        bool completed=false;
        {
            NativeEdram32Mutation mutation(owner);
            completed=executeNativeScreenStripDraw(device,program,submitted,bound,true,mutation,&why);
        }
        if(!completed)std::printf("SCREEN_STRIP_PHYSICAL_FAIL mode=%u stage=%s\n",mode,why);
        assert(completed && owner.nextSerial()==serial+1);
        const auto after=owner.readColorWords(720,1280,1280,720);assert(after);
        assert(owner.readDepthWords(0,1280,1280,720)==std::optional{depths});
        assert(owner.readColorWords(1500,80,80,16)==unrelated);
        unsigned changed=0;
        for(unsigned i=0;i<Pixels;++i) {
            assert(((*after)[i]&0xC0000000u)==((*before)[i]&0xC0000000u));
            if(i%4)assert((*after)[i]==(*before)[i]);
            else if(mode>=2) {
                // High alpha2 clamps to1: only source(40,-1,.5) remains.
                // Low alpha-1 clamps to0: source+unchanged destination.
                const auto expected=mode==2?std::array<float,3>{40,-1,.5f}:std::array<float,3>{1.125f,.75f,.75f};
                for(unsigned c=0;c<3;++c) {unsigned code=0;assert(pack7e3Checked(colorBits(expected[c]),code));
                    assert((((*after)[i]>>(c*10))&1023)==code);}
            }
            changed+=(*after)[i]!=(*before)[i];
        }
        if(mode)assert(changed>0);
        unsigned nonzeroReferences=0;
        if(mode<2)for(unsigned i=0;i<80;++i) {
            const auto& ref=mode?zeroReference:reference;
            const unsigned x=be(ref,i*24),y=be(ref,i*24+4),at=y*1280+x;
            assert(x<1280 && y<720 && x%4==0);
            const float alpha=std::clamp(std::bit_cast<float>(be(ref,i*24+20)),0.f,1.f);
            bool nonzero=false;
            for(unsigned c=0;c<3;++c) {
                const float source=std::bit_cast<float>(be(ref,i*24+8+4*c));nonzero|=source!=0;
                const float want=source+initial[4*at+c]*(1-alpha);unsigned code=0;
                assert(pack7e3Checked(colorBits(want),code));
                const unsigned got=((*after)[at]>>(c*10))&1023;
                if(got!=code)std::printf("STRIP_PHYSICAL_PIXEL_MISMATCH mode=%u x=%u y=%u c=%u got=%u expected=%u\n",mode,x,y,c,got,code);
                assert(got==code);
            }
            nonzeroReferences+=nonzero;
        }
        if(mode==1)assert(nonzeroReferences==80);
        std::printf("SCREEN_STRIP_PHYSICAL_GPU_PASS mode=%u pixels=921600 changed=%u references=%u nonzero_references=%u "
            "alpha_depth_stencil_unrelated_preserved=1 source_and_destination_authored=%u destination_authored=1 "
            "physical_publication=1 live_game=0 gameplay=0\n",mode,changed,mode<2?80u:0u,nonzeroReferences,unsigned(mode>0));
    }
    vkDestroySampler(device.device,sampler,nullptr);
}
