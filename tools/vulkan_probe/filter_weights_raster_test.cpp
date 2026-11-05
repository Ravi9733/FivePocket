// Actual current shader pair/vertices/constants through Vulkan rasterization.
// Saved inputs and authored initial physical owner: NOT live guest completion.
#include "native_filter_weights_plan.h"
#include "native_linear_sample_target.h"
#include "native_rgb7e3_replacement.h"
#include "native_filter_weights_draw.h"
#include "mip_sample_probe.h"
#include "process_limits.h"
#ifdef RECOMP_PRIVATE_EXPOSURE_SCALAR
#include "native_exposure_scalar_draw.h"
#include "exposure_scalar_fixture.h"
#ifdef RECOMP_PRIVATE_BRIGHT_PASS
#include "native_bright_pass_draw.h"
#include "bright_pass_fixture.h"
#ifdef RECOMP_PRIVATE_BLOOM_DOWNSAMPLE
#include "native_expanded16_copy_draw.h"
#include "expanded16_copy_fixture.h"
#include "native_rgb7e3_float_resolve.h"
#ifdef RECOMP_PRIVATE_POSTFX_BLUR
#include "native_postfx_blur_draw.h"
#include "postfx_blur_fixture.h"
#ifdef RECOMP_PRIVATE_POSTFX_UPSAMPLE
#include "native_postfx_upsample_draw.h"
#include "postfx_upsample_fixture.h"
#endif
#endif
#endif
#endif
#endif
#ifdef RECOMP_PRIVATE_EXPOSURE_COPY
#include "native_exposure_resolve.h"
#endif
#ifdef RECOMP_PRIVATE_R32_SCALAR
#include "native_r32_scalar_draw.h"
#endif
#ifdef RECOMP_PRIVATE_LUMA_R32_COPY
#include "native_r32_float_resolve.h"
#include "native_serialized_resource_batch.h"
#include "image_sample_probe.h"
#endif
#ifdef RECOMP_PRIVATE_EXPANDED16_LUMA
#include "native_expanded16_luma_draw.h"
#endif
#ifdef RECOMP_PRIVATE_RGB7E3_RESOLVE_CALL
#include "rgb7e3_float_resolve_fixture.h"
#endif
#ifdef RECOMP_PRIVATE_EXPANDED16_MAX
#include "native_expanded16_max_draw.h"
#endif
#include <cassert>
#include <cstdio>
#include <fstream>
using namespace recomp::gpu;
namespace {
std::vector<std::uint8_t> read(const std::string& p,unsigned bytes) {
    std::ifstream f(p,std::ios::binary|std::ios::ate);assert(f && f.tellg()==bytes);f.seekg(0);
    std::vector<std::uint8_t> b(bytes);assert(f.read(reinterpret_cast<char*>(b.data()),bytes));return b;
}
std::vector<unsigned> code(const std::string& p) {
    std::ifstream f(p,std::ios::binary|std::ios::ate);assert(f);const auto size=f.tellg();
    assert(size>=20 && size<262144 && size%4==0);f.seekg(0);std::vector<unsigned> b(size/4);
    assert(f.read(reinterpret_cast<char*>(b.data()),size));return b;
}
unsigned be(const auto& b,unsigned at){return (unsigned{b[at]}<<24)|(unsigned{b[at+1]}<<16)|(unsigned{b[at+2]}<<8)|b[at+3];}
}
int main(int argc,char** argv) {
    assert((argc==2 || (argc==3 && (std::string(argv[2])=="--coordinates" || std::string(argv[2])=="--connected-uv"))) && initializeProbeProcess());
    std::setvbuf(stdout,nullptr,_IONBF,0);const std::string p=argv[1];
    const auto meta=read(p+"/capture-meta.bin",64);
    Gta5InlineDraw d;d.primitive=8;d.vertexCount=3;d.stride=24;d.vertices=read(p+"/vertices.bin",72);
    d.deviceState=read(p+"/device-state.bin",13860);d.vertexShader=be(d.deviceState,13072);d.pixelShader=be(d.deviceState,13068);
    d.scissorRegisters=std::array<unsigned,3>{be(meta,48),be(meta,52),be(meta,56)};
    Gta5InlineDraw::Texture t,v;t.slot=5;v.slot=17;v.literalZero=true;
    for(unsigned i=0;i<6;++i){t.fetch[i]=be(meta,i*4);v.fetch[i]=be(meta,24+i*4);}
    t.source=read(p+"/base.bin",3768320);d.textures={t,v};
    Gta5BoundSurfaceCapture b;for(unsigned i=0;i<5;++i)b.surfaces[i].object=be(d.deviceState,12816+4*i);
    b.surfaces[0].header=read(p+"/surface-0-argument.bin",40);
    NativeTranslatedDrawProgram program;program.abiVersion=35;program.stride=24;program.interpolators=1;
    program.textureSlot=5;program.textureSlots={5};program.vertexIdentity=read(p+"/vertex-shader.bin",132);
    program.pixelIdentity=read(p+"/pixel-shader.bin",708);program.vertexCode=code(p+"/original.vert.spv");program.pixelCode=code(p+"/original.frag.spv");
    const NativeFilterWeightsIdentity identity{program.vertexIdentity,program.pixelIdentity};
    const auto plan=planNativeFilterWeightsDraw(d,b,identity,identity.vertex,identity.pixel,false,true);assert(plan);
    ProbeDevice device;assert(device.inspect() && device.create({}));
    auto image=NativeTextureImage::createRaw10Unsigned(device,plan->texture,t.source);assert(image);const auto before=image->readback();assert(before);
    struct Sampler {VkDevice d;VkSampler s{};~Sampler(){if(s)vkDestroySampler(d,s,nullptr);}} sampler{device.device};
    VkSamplerCreateInfo si{};si.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;si.magFilter=si.minFilter=VK_FILTER_NEAREST;
    si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    assert(vkCreateSampler(device.device,&si,nullptr,&sampler.s)==VK_SUCCESS);
    NativeTranslatedPipelineOptions options;options.depthStencil=decodeXenosDepthStencilState(0x00700760,0xFFFFFF,0);
    options.externalTextures={{sampler.s,image->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    // Rejection occurs before VkRenderPass use: altered profiles cannot sneak
    // through the new resource interface under an existing ABI.
    for(unsigned i=0;i<8;++i) {
        auto bad=program;auto o=options;auto desc=plan->texture;
        if(i==0)bad.abiVersion=36;if(i==1)bad.stride=36;if(i==2)o.samples=2;
        if(i==3)o.depthStencil.reset();if(i==4)o.colorAttachmentCount=2;
        if(i==5)desc.format=6;if(i==6)o.externalTextures.clear();if(i==7)bad.interpolators=2;
        NativeTranslatedResources resources(device.device);assert(!resources.setup(device,VK_NULL_HANDLE,bad,{desc},NativeTranslatedBlend::Replace,o));
    }
    const auto queriesBytes=read(p+"/queries.bin",3249*16);
    std::vector<std::array<float,4>> queries(3249);std::memcpy(queries.data(),queriesBytes.data(),queriesBytes.size());
    if(argc==3) {
        auto diagnostic=program;diagnostic.pixelCode=code(p+"/coordinates.frag.spv");
        std::vector<std::uint8_t> input(8208+72);input[3]=3;
        std::copy(d.vertices.begin(),d.vertices.end(),input.begin()+8208);
        auto target=NativeLinearSampleTarget::create(device,640,360,1);assert(target);
        const auto uv=target->draw(diagnostic,input,6,plan->viewport,{plan->texture},options,
            NativeTranslatedBlend::Replace,{-12345,-12345,-12345,-12345},0,0);
        assert(uv && uv->size()==640*360*4);
        double maxTexelError=0;
        for(unsigned y=0;y<360;++y)for(unsigned x=0;x<640;++x) {
            const auto at=(y*640+x)*4;
            assert((*uv)[at]>=0 && (*uv)[at]<1 && (*uv)[at+1]>=0 && (*uv)[at+1]<1 && (*uv)[at+2]==0 && (*uv)[at+3]==1);
            maxTexelError=std::max({maxTexelError,std::abs(double((*uv)[at])-float((x+.5f)/640))*1280,
                std::abs(double((*uv)[at+1])-float((y+.5f)/360))*720});
        }
        const bool capture=std::string(argv[2])=="--coordinates";
        std::vector<std::uint8_t> reference;
        if(!capture)reference=read(p+"/raster-queries.bin",3249*16);
        for(unsigned i=0;i<queries.size();++i) {
            const auto x=std::lround(queries[i][0]*640-.5f),y=std::lround(queries[i][1]*360-.5f),at=(y*640+x)*4;
            if(capture)std::printf("FILTER_RASTER_UV index=%u xbits=%08X ybits=%08X\n",i,colorBits((*uv)[at]),colorBits((*uv)[at+1]));
            else {
                float expected[4];std::memcpy(expected,reference.data()+i*16,16);
                assert((*uv)[at]==expected[0] && (*uv)[at+1]==expected[1]);
            }
        }
        std::printf("FILTER_RASTER_UV_CAPTURE_PASS pixels=230400 queries=3249 max_source_texel_deviation=%.9g real_vertex_interpolation=1 original_pixel_shader=0 live_publication=0\n",maxTexelError);
        if(capture)return 0;
    }
    std::vector<float> baseline;
    for(unsigned variant=0;variant<2;++variant) {
        const auto bank=read(p+(variant?"/changed-pixel-constants.bin":"/pixel-constants.bin"),4096);
        const auto expectedBytes=read(p+(variant?"/changed-expected.bin":"/expected.bin"),3249*16);
        std::vector<std::array<float,4>> expected(3249);std::memcpy(expected.data(),expectedBytes.data(),expectedBytes.size());
        std::vector<std::uint8_t> input(8208+72);input[3]=3;
        const auto vc=read(p+"/vertex-constants.bin",4096);std::copy(vc.begin(),vc.end(),input.begin()+16);
        for(unsigned i=0;i<1024;++i){d.vertexConstants[i]=be(vc,i*4);d.pixelConstants[i]=be(bank,i*4);}
        std::copy(bank.begin(),bank.end(),input.begin()+4112);std::copy(d.vertices.begin(),d.vertices.end(),input.begin()+8208);
        auto target=NativeLinearSampleTarget::create(device,640,360,1);assert(target);
        const auto output=target->draw(program,input,6,plan->viewport,{plan->texture},options,
            NativeTranslatedBlend::Replace,{-12345,-12345,-12345,-12345},0,0);
        if(!output)std::printf("FILTER_WEIGHTS_RASTER_REFUSED variant=%u stage=%s\n",variant,target->lastDrawFailure());
        assert(output && output->size()==640*360*4);
        unsigned differences=0;double maximum=0;
        for(unsigned i=0;i<queries.size();++i) {
            const auto x=std::lround(queries[i][0]*640-.5f),y=std::lround(queries[i][1]*360-.5f);
            for(unsigned c=0;c<4;++c) {
                const auto actual=(*output)[(y*640+x)*4+c],wanted=expected[i][c];
                const double error=std::abs(double(actual)-wanted);maximum=std::max(maximum,error);
                if(error>2e-5*std::max(1.,std::abs(double(wanted)))) {
                    if(differences<8)std::printf("RASTER_MISMATCH variant=%u x=%ld y=%ld c=%u actual=%.9g wanted=%.9g\n",variant,x,y,c,actual,wanted);
                    ++differences;
                }
            }
        }
        // Entire output must replace the poison-clear sentinel; readback
        // already rejected NaNs/Infs. Both diagonals and every edge are covered.
        for(float value:*output)assert(value>=0 && value<=256);
        std::printf("FILTER_WEIGHTS_RASTER_COMPARE variant=%u mismatches=%u max_absolute=%.9g\n",variant,differences,maximum);
        assert(!differences && image->readback()==before);
        if(!variant)baseline=*output;else assert(*output!=baseline);
        NativeEdram32Bands owner;const std::vector<std::uint8_t> prior(1280*720*4,0x5A);
        assert(owner.recordCompletedRawColorRows(720,1280,1280,720,prior,1));
        {NativeEdram32Mutation mutation(owner);assert(recordCompletedNativeRgb7e3Replacement(mutation,plan->target,*output,Color7e3Rounding::XeniaShiftRound));mutation.commit();}
        auto wanted=std::vector<unsigned>(1280*720,0x5A5A5A5A);
        for(unsigned y=0;y<360;++y)for(unsigned x=0;x<640;++x) {
            std::array<unsigned,4> bits{};for(unsigned c=0;c<4;++c)bits[c]=colorBits((*output)[(y*640+x)*4+c]);
            unsigned packed=0;assert(packColor7e3(bits,packed,Color7e3Rounding::XeniaShiftRound));
            const auto tile=(y/16)*8+x/80;wanted[((tile/16)*16+y%16)*1280+(tile%16)*80+x%80]=packed;
        }
        assert(owner.readColorWords(720,1280,1280,720)==wanted);
        NativeEdram32Bands connected;
        assert(connected.recordCompletedRawColorRows(720,1280,1280,720,prior,1));
        {NativeEdram32Mutation mutation(connected);const char* why=nullptr;
            const bool complete=executeNativeFilterWeightsDraw(device,program,d,b,identity.vertex,identity.pixel,true,mutation,&why);
            if(!complete)std::printf("FILTER_WEIGHTS_EXECUTOR_REFUSED %s\n",why);
            assert(complete);}
        assert(connected.readColorWords(720,1280,1280,720)==wanted && connected.nextSerial()==3);
#ifdef RECOMP_PRIVATE_RGB7E3_RESOLVE_CALL
        // Original shader -> production fenced executor -> shared physical
        // words -> captured NEXT copy -> expanded16 storage -> Vulkan import.
        // The captured call/header is real; prior destination bytes and parent
        // physical owner are authored fixtures, not a live publication receipt.
        const auto call=rgb7e3_test::captured(p+"/resolve");
        const char* resolveWhy=nullptr;const auto resolvePlan=planNativeRgb7e3FloatResolve(call,&resolveWhy);
        if(!resolvePlan)std::printf("FILTER_COPY_PLAN_REFUSED %s\n",resolveWhy);assert(resolvePlan);
        assert(resolvePlan->sourceObject==plan->target.object);
        const auto serial=connected.nextSerial();
        const std::vector<std::uint8_t> previous(resolvePlan->storage.sourceBytes,0xA5);
        const auto completed=packNativeRgb7e3FloatResolve(call,connected,previous,&resolveWhy);
        if(!completed)std::printf("FILTER_COPY_REFUSED %s\n",resolveWhy);assert(completed);
        auto expectedStorage=previous;std::vector<std::uint8_t> expectedLinear(640*360*8);
        for(unsigned y=0;y<360;++y)for(unsigned x=0;x<640;++x) {
            const auto tile=(y/16)*8+x/80;
            const auto encoded=wanted[((tile/16)*16+y%16)*1280+(tile%16)*80+x%80];
            const auto floats=unpackColor7e3(encoded);
            for(unsigned c=0;c<4;++c) {
                const _Float16 reference=colorFloat(floats[c]);std::uint16_t h=0;std::memcpy(&h,&reference,2);
                for(unsigned byte=0;byte<2;++byte) {
                    expectedStorage[(rgb7e3_test::offset(x,y)+c*2+byte)^1]=h>>(byte*8);
                    expectedLinear[(y*640+x)*8+c*2+byte]=h>>(byte*8);
                }
            }
        }
        assert(*completed==expectedStorage && connected.nextSerial()==serial);
        const auto imported=linearizeXenosExpanded16FloatStorage(resolvePlan->destination,*completed);
        assert(imported && *imported==expectedLinear);
        auto texture=NativeTextureImage::create(device,resolvePlan->storage.description,*completed);
        assert(texture && texture->readback()==std::optional{expectedLinear});
        std::printf("FILTER_WEIGHTS_TO_FLOAT_TEXTURE_PASS variant=%u pixels=230400 original_call=1 fenced_producer=1 binary16=1 independent_tiling=1 gpu_texture_readback=1 live_publication=0\n",variant);
#ifdef RECOMP_PRIVATE_EXPANDED16_MAX
#include "expanded16_max_connection.inc"
#endif
#endif
        NativeEdram32Bands unknown;
        {NativeEdram32Mutation mutation(unknown);
            assert(!executeNativeFilterWeightsDraw(device,program,d,b,identity.vertex,identity.pixel,false,mutation));}
        assert(!unknown.knownTiles() && unknown.nextSerial()==1);
    }
    std::puts("FILTER_WEIGHTS_RASTER_PASS pixels=230400 variants=2 cpu_reference_points=3249 current_vertices=1 runtime_constants=1 full_coverage=1 physical7e3_writer=1 padding_preserved=1 live_publication=0 xbox_filter_conformance=0");
    std::puts("FILTER_WEIGHTS_EXECUTOR_PASS variants=2 current_inputs_to_fenced_physical=1 query_pending_refuses=1 failed_no_publication=1 full_parent_words_checked=921600 game_callback=0");
}
