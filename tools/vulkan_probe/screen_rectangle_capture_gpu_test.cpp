// Exact private stopped pair/state. The authored texture is a SEPARATE sampling
// coverage test, never a substituted game resource or proof of gameplay.
#ifndef RECOMP_PRIVATE_SCREEN_PACK
#error An explicit private ABI26 shader pack is required
#endif
#include RECOMP_PRIVATE_SCREEN_PACK
#include "native_offscreen_draw_target.h"
#include "native_vertex_clip_executor.h"
#include "native_edram32_bands.h"
#ifdef RECOMP_PRIVATE_STENCIL_REJECT_IDENTITY
#include RECOMP_PRIVATE_STENCIL_REJECT_IDENTITY
#endif
#include <cassert>
#include <bit>
#include <cmath>
#include <cstdio>
#include <fstream>
using namespace recomp::gpu;
int main(int argc,char** argv) {
    assert(argc==2 || argc==3);std::setvbuf(stdout,nullptr,_IONBF,0);const std::string folder=argv[1];
    const auto read=[&](const char* name,unsigned size) {
        std::ifstream f(folder+"/"+name,std::ios::binary|std::ios::ate);
        assert(f && f.tellg()==size);f.seekg(0);std::vector<std::uint8_t> b(size);
        assert(f.read(reinterpret_cast<char*>(b.data()),size));return b;
    };
    const auto word=[](const auto& b,unsigned at){return (std::uint32_t{b[at]}<<24)|
        (std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];};
    const auto meta=read("replay-metadata.bin",48);
    Gta5InlineDraw draw;draw.primitive=word(meta,0);draw.vertexCount=word(meta,4);draw.stride=word(meta,8);
    assert(draw.primitive==8 && draw.vertexCount==3 && draw.stride==24);
    draw.scissorRegisters=std::array<std::uint32_t,3>{word(meta,12),word(meta,16),word(meta,20)};
    draw.deviceState=read("device-state.bin",13860);draw.vertices=read("vertices.bin",72);
    const auto vc=read("vertex-constants.bin",4096),pc=read("pixel-constants.bin",4096);
    for(unsigned i=0;i<1024;++i){draw.vertexConstants[i]=word(vc,4*i);draw.pixelConstants[i]=word(pc,4*i);}
    const auto& program=nativeScreenSpaceProgram;
    assert(read("vertex-shader.bin",program.draw.vertexIdentity.size())==program.draw.vertexIdentity);
    assert(read("pixel-shader.bin",program.draw.pixelIdentity.size())==program.draw.pixelIdentity);
    Gta5InlineDraw::Texture texture;texture.slot=2;
    for(unsigned i=0;i<6;++i)texture.fetch[i]=word(meta,24+4*i);
    texture.source=read("texture-2.bin",3768320);draw.textures={texture};
    Gta5BoundSurfaceCapture bound;
    for(unsigned i=0;i<5;++i){bound.surfaces[i].object=word(draw.deviceState,12816+4*i);
        bound.surfaces[i].tileCacheObject=word(draw.deviceState,13104+4*i);}
    bound.surfaces[0].header=read("surface-0-argument.bin",40);
    bound.surfaces[0].tileCacheHeader=read("surface-0-tile-cache.bin",40);
    const char* why=nullptr;const auto plan=planNativeCompositeRectangleDraw(draw,program.draw,bound,&why);
    if(!plan)std::printf("SCREEN_GPU_PLAN_REFUSED %s\n",why);
    assert(plan && plan->baseTile==0 && plan->width==1280 && plan->height==720 && plan->tileCount==720);
    const auto desc=XenosTextureDescription::decode(texture.fetch);assert(desc);
    assert(desc->width==1280 && desc->height==720 && desc->format==6 &&
           (desc->swizzle==std::array<unsigned,4>{2,1,0,3}));
    assert(draw.pixelConstants[1016]==0x3C4CCCCDu && draw.pixelConstants[1017]==0x3D008081u);
    assert(draw.pixelConstants[1020]==0x42200000u && draw.pixelConstants[1021]==0xC2200000u &&
           draw.pixelConstants[1022]==0x41FEFFFFu && draw.pixelConstants[1023]==0x3F000000u);
    // Independent scalar FP32 reference, not the translator's expression DAG.
    // Sampling swizzle changes B/R, shader changes them back. Only raw R is
    // reduced with FRAC; GBA remain unchanged. All256 red bytes are covered by
    // the separate authored texture, with x/y-varying GBA to detect wrong texels.
    std::array<std::uint8_t,256> red{};
    for(unsigned r=0;r<256;++r) {
        const float normalized=float(r)/255.0f;
        const float product=normalized*std::bit_cast<float>(draw.pixelConstants[1022]);
        const float fraction=product-std::floor(product);
        const float value=fraction*std::bit_cast<float>(draw.pixelConstants[1017]);
        red[r]=std::uint8_t(std::lround(value*255.0f));
        assert(red[r]<=8);
    }
    ProbeDevice device;ShaderRequirements requirements;assert(device.inspect() && device.create(requirements));
    // Resource-level profile isolation, independent of the state planner.
    for(unsigned kind=0;kind<4;++kind) {
        auto bad=program.draw;NativeTranslatedPipelineOptions options;
        if(kind==0)bad.screenSpaceRectangle=false;
        if(kind==1)bad.interpolators=1;
        if(kind==2)options.samples=2;
        if(kind==3)options.colorAttachmentCount=2;
        NativeTranslatedResources resources(device.device);
        assert(!resources.setup(device,VK_NULL_HANDLE,bad,{*desc},NativeTranslatedBlend::Replace,options));
    }
    auto executor=NativeVertexClipExecutor::create(device,program.positions);assert(executor);
    const auto positions=executor->rectanglePositions(draw);
    assert(positions && nativeFullRectangleList(draw,*positions));
    assert(nativeCompositeCompletedRows(draw,*plan,*positions)==720);
    for(bool authored:{false,true}) {
        auto input=draw;auto source=linearizeXenosTexture2D(*desc,texture.source);assert(source);
        if(authored) {
            for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x) {
                const auto at=4*(y*1280+x);
                (*source)[at]=std::uint8_t(x+7*y);(*source)[at+1]=std::uint8_t(3*x+y);
                (*source)[at+2]=std::uint8_t(x+11*y);(*source)[at+3]=std::uint8_t(13*x+5*y);
            }
            auto storage=storeXenosTexture2D(*desc,*source,texture.source);assert(storage);
            assert(linearizeXenosTexture2D(*desc,*storage)==source);input.textures[0].source=*storage;
        }
        auto target=NativeOffscreenDrawTarget::create(device,*plan);assert(target);
        NativeEdram32Bands bands;std::optional<std::vector<std::uint8_t>> output;
        const bool completed=[&]{NativeEdram32Mutation mutation(bands);
            return executeNativeEdram32ColorRows(mutation,*plan,
                [&]{assert(!bands.knownTiles());return target->drawCompositeRectangles(program.draw,input,bound,*positions,&why);},
                [&]{output=target->readCompletedColorRows();return output;},
                [&]{return target->completedColorRows();});}();
        std::printf("SCREEN_GPU_DRAW authored=%u completed=%u stage=%s\n",unsigned(authored),unsigned(completed),why);
        assert(completed && output && output->size()==1280*720*4 && target->valid());
        unsigned mismatches=0;
        for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x) {
            const unsigned sx=x/80*80+(x%80+40)%80,at=4*(y*1280+x),from=4*(y*1280+sx);
            for(unsigned c=0;c<4;++c) {
                const auto expected=c?(*source)[from+c]:red[(*source)[from]];
                if((*output)[at+c]!=expected) {
                    if(mismatches<8)std::printf("SCREEN_GPU_MISMATCH x=%u y=%u c=%u actual=%u expected=%u\n",x,y,c,(*output)[at+c],expected);
                    ++mismatches;
                }
            }
        }
        std::printf("SCREEN_GPU_BYTES authored=%u mismatches=%u bytes=%zu\n",unsigned(authored),mismatches,output->size());
        assert(!mismatches && bands.knownTiles()==720);
        // The production completion transaction publishes raw physical words,
        // not a saved image handle. Check consumer half-permutation separately.
        const auto depth=bands.readDepthWords(0,1280,1280,720);assert(depth);
        for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x) {
            const auto from=4*(y*1280+x);
            const auto expected=std::uint32_t{red[(*source)[from]]}|(std::uint32_t{(*source)[from+1]}<<8)|
                (std::uint32_t{(*source)[from+2]}<<16)|(std::uint32_t{(*source)[from+3]}<<24);
            assert((*depth)[y*1280+x]==expected);
        }
        if(argc==3) {
#ifdef RECOMP_PRIVATE_STENCIL_REJECT_IDENTITY
            // Connected GPU producer -> production physical owner -> stencil
            // rejection proof. Post state is captured; the preceding screen
            // state is explicitly a derived replay, NOT a saved live owner.
            const std::string next=argv[2];
            const auto nextRead=[&](const char* name,unsigned size) {
                std::ifstream f(next+"/"+name,std::ios::binary|std::ios::ate);assert(f && f.tellg()==size);
                f.seekg(0);std::vector<std::uint8_t> b(size);assert(f.read(reinterpret_cast<char*>(b.data()),size));return b;
            };
            Gta5InlineDraw post;post.primitive=8;post.vertexCount=3;post.stride=36;
            post.vertices=nextRead("vertices.bin",108);post.deviceState=nextRead("device-state.bin",13860);
            post.vertexShader=word(post.deviceState,13072);post.pixelShader=word(post.deviceState,13068);
            post.scissorRegisters=std::array<std::uint32_t,3>{0,0,47187200};
            Gta5BoundSurfaceCapture postBindings;
            for(unsigned i=0;i<5;++i)postBindings.surfaces[i].object=word(post.deviceState,12816+4*i);
            postBindings.surfaces[0].header=nextRead("surface-0-argument.bin",40);
            postBindings.surfaces[4].header=nextRead("surface-4-argument.bin",40);
            NativeStencilRejectedRectangleProof proof;bool display=false;
            #ifdef RECOMP_PRIVATE_CONDITIONAL_STENCIL_REJECT
            const auto& identity=nativeConditionalStencilRejectIdentity;
            #else
            const auto& identity=nativeStencilRejectIdentity;
            #endif
            const auto vs=nextRead("vertex-shader.bin",168),ps=nextRead("pixel-shader.bin",identity.pixel.size());
            {NativeEdram32Mutation mutation(bands);
                const bool reject=proveNativeStencilRejectedRectangle(post,postBindings,identity,
                    vs,ps,false,true,mutation.candidate(),proof,&why);
                assert(reject==!authored && proof.examined==1280*720 && bool(proof.passing)==authored);
                if(!authored)assert(finishNativeReadOnlyDraw(reject,mutation));
                else mutation.commit(); // Test continues inspecting its prior fenced producer; NO draw completion claimed.
            }
            assert(!display && bands.readDepthWords(0,1280,1280,720)==depth && !bands.readDepthWords(720,1280,1280,720));
            std::printf("POST_STENCIL_GPU_CONNECTION_PASS authored=%u examined=%u zero_passing=%u "
                "real_gpu_producer=1 colour_owner_not_invented=1 guest_execution=0 gameplay=0\n",unsigned(authored),proof.examined,unsigned(!proof.passing));
            #ifdef RECOMP_PRIVATE_CONDITIONAL_STENCIL_REJECT
            std::printf("CONDITIONAL_STENCIL_GPU_CONNECTION_PASS authored=%u exact_new_capture=1 "
                "passing_sample_refuses=%u native_shader_execution=0 gameplay=0\n",unsigned(authored),unsigned(authored));
            #endif
#else
            assert(false && "explicit analysed identity required for the connected post proof");
#endif
        }
        const auto stored=storeXenosTexture2D(*desc,*output,texture.source);assert(stored);
        assert(linearizeXenosTexture2D(*desc,*stored)==output);
        assert(bands.invalidate(0,1) && !bands.readDepthWords(0,1280,1280,720));
        std::printf("SCREEN_RECTANGLE_GPU_PASS authored=%u rgba_bytes=3686400 exact=1 generated_xy=1 physical_consumer=1 storage_roundtrip=1 gameplay=0\n",unsigned(authored));
    }
}
