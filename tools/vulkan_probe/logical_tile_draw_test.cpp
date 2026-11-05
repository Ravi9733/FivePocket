// Actual stopped layout/headers, AUTHORED translated shaders and draw inputs.
// Exercises the connected native producer/consumer, not live game admission.
#include "native_logical_tile_target.h"
#include "native_texture_image.h"
#include "native_visibility_queries.h"
#include "gta5_logical_tile_resolve_fixture.h"
#include <bit>
#include <cstdio>
#include <fstream>
#include <string>
using namespace recomp::gpu;
using namespace recomp::gpu::test;
int main(int argc,char** argv) {
    assert(argc==2);std::setvbuf(stdout,nullptr,_IONBF,0);const std::string dir=argv[1];
    const auto read=[&](const std::string& name,unsigned size=0) {
        std::ifstream f(dir+"/"+name,std::ios::binary|std::ios::ate);assert(f);
        const auto n=f.tellg();assert(n>0 && n<=262144 && (!size || n==size));f.seekg(0);
        std::vector<std::uint8_t> b(n);assert(f.read(reinterpret_cast<char*>(b.data()),n));return b;
    };
    const auto shader=[&](const std::string& name) {
        const auto b=read(name);assert(b.size()%4==0);std::vector<unsigned> v(b.size()/4);std::memcpy(v.data(),b.data(),b.size());return v;
    };
    // Unknown/new ABI must reject BEFORE querying an uninitialized device.
    { ProbeDevice empty;for(unsigned abi:{0u,3u,14u,15u,UINT32_MAX}) {
        NativeTranslatedResources r(VK_NULL_HANDLE);NativeTranslatedDrawProgram p;p.abiVersion=abi;
        assert(!r.setup(empty,VK_NULL_HANDLE,p,{}));
    }}
    Gta5ClearCapture capture;capture.state=read("scissor-device-state.bin",13860);
    for(unsigned i=0;i<5;++i) {
        auto& s=capture.surfaces[i];s.object=tileWord(*capture.state,12816+i*4);
        s.tileCache=tileWord(*capture.state,13104+i*4);s.header=read("scissor-surface-"+std::to_string(i)+".bin",40);
    }
    const auto layout=*describeGta5DriverTiling(*capture.state);assert(layout.width==1280 && layout.height==720 && layout.tiles.size()==2);
    ProbeDevice device;ShaderRequirements requirements;requirements.vertexStoresAndAtomics=requirements.fragmentStoresAndAtomics=true;
    assert(device.inspect() && device.create(requirements,false,false,true));
    assert(device.independentBlendEnabled && !device.sampleRateShadingEnabled);
    auto target=NativeLogicalTileTarget::create(device,*capture.state);assert(target);
    NativeLogicalTileTarget::Clear clear;clear.tiles=3;clear.aspects=63;clear.depth=0;clear.stencil=37;
    for(unsigned i=0;i<4;++i)clear.colors[i]={std::uint8_t(17+i*19),61,137,241};
    NativeTranslatedDrawProgram program;program.vertexCode=shader("subset-15-format-0.vert.spv");
    program.stride=24;program.indexed=true;program.abiVersion=14;program.textureSlot=UINT32_MAX;
    program.geometryDepth20e4=true;program.geometryMrtFormats={0,0,0,0};
    NativeTranslatedPipelineOptions options;options.colorAttachmentCount=4;options.minDepth=0;options.maxDepth=1;
    XenosDepthStencilState z;z.depthEnable=z.depthWrite=z.stencilEnable=true;z.depthCompare=4;
    z.front={7,0,2,0,91,255,255};options.depthStencil=z;
    std::vector<std::uint8_t> input(8208+3*24);tileWord(input,0,3);
    const auto vertices=[&](float depth) {
        const std::array<std::array<float,2>,3> xy{{{-1,-1},{3,-1},{-1,3}}};
        for(unsigned i=0;i<3;++i) {
            tileFloat(input,8208+i*24,xy[i][0]);tileFloat(input,8212+i*24,xy[i][1]);tileFloat(input,8216+i*24,depth);
            tileWord(input,8220+i*24,0);tileWord(input,8224+i*24,0);tileWord(input,8228+i*24,0);
        }
    };
    vertices(std::bit_cast<float>(0x3F000002u));
    const auto draw=[&](unsigned tile,const std::array<unsigned,4>& scissor,const std::vector<XenosTextureDescription>& textures=std::vector<XenosTextureDescription>{},bool* visible=nullptr,std::uint32_t* diagnostics=nullptr) {
        return target->drawCapturedIndexed(capture,tile,program,input,3,{2,0,1},{1,1,0,0},scissor,textures,options,{},visible,diagnostics);
    };
    program.geometryMrtOutputs=15;program.pixelCode=shader("subset-15-format-0.frag.spv");
    assert(target->clear(clear) && draw(0,{32,352,64,64}));
    const auto unmeasured=target->readback();assert(unmeasured);
    bool visible=false;std::uint32_t diagnostics=0;
    assert(target->clear(clear) && draw(0,{32,352,64,64},{},&visible) && visible);
    const auto measured=target->readback();assert(measured);
    assert(measured->colors==unmeasured->colors && measured->depth==unmeasured->depth && measured->stencil==unmeasured->stencil);
    // Identical geometry fails strict GREATER. The reused query must reset,
    // otherwise it would incorrectly retain the previous visible result.
    assert(draw(0,{32,352,64,64},{},&visible) && !visible);
    assert(target->readback()->colors==measured->colors);
    visible=true;const auto beforeEmpty=target->completedDrawSerial();
    diagnostics=UINT32_MAX;
    assert(draw(0,{32,400,64,64},{},&visible,&diagnostics) && !visible && diagnostics==0 && target->completedDrawSerial()==beforeEmpty);
    visible=true;assert(!draw(0,{1281,0,1,1},{},&visible) && visible);
    visible=false;assert(!draw(2,{0,0,1280,720},{},&visible) && !visible);
    assert(target->clear(clear) && draw(0,{32,352,64,64},{},&visible) && visible);
    // Stencil rejection is independently observable, not just depth/culling.
    auto queryDepth=*options.depthStencil;options.depthStencil->depthEnable=false;
    options.depthStencil->front.compare=0;
    assert(draw(0,{32,352,64,64},{},&visible) && !visible);options.depthStencil=queryDepth;
    std::puts("NATIVE_TILE_VISIBILITY_GPU_PASS visible=1 depth_hidden=1 stencil_hidden=1 empty_clip=1 reset=1 invalid_preserves_result=1 image_equivalence=1 precise_count=0 guest_publication=0");
    assert(target->clear(clear));
    for(bool expected:{true,false}) {
        NativeVisibilityQueries queries;NativeVisibilityQueries::Journal queryJournal;
        assert(queries.command(8,queryJournal));const auto previous=queryJournal;
        assert(!queries.draw(17,[&]()->std::optional<NativeVisibilityQueries::Measurement>{
            bool measured=false;if(!draw(0,{32,352,64,64},{},&measured))return std::nullopt;
            assert(measured==expected);
            return NativeVisibilityQueries::Measurement{measured,NativeVisibilityQueries::Stage::FinalSamples};
        }));
        assert(queryJournal==previous && queries.failed() && !queries.command(0x108,queryJournal));
    }
    std::puts("NATIVE_VISIBILITY_FINAL_STAGE_REJECTION_GPU_PASS visible_and_hidden_rejected=1 no_status_publication=1 actual_gpu_measurements=1");
    const auto originalPixel=program.pixelCode;
    program.pixelCode=shader("query-stage-control.frag.spv");
    assert(target->clear(clear));const auto beforeKilled=target->readback();assert(beforeKilled);
    diagnostics=0;
    assert(draw(0,{32,352,64,64},{},&visible,&diagnostics) && !visible);
    assert((diagnostics&0xA5u)==5u); // Entered and killed; no depth store.
    const auto killed=target->readback();assert(killed && killed->colors==beforeKilled->colors && killed->depth==beforeKilled->depth && killed->stencil==beforeKilled->stencil);
    tileWord(input,8204,1); // Authored predicate control, not a captured constant.
    assert(draw(0,{32,352,64,64},{},&visible,&diagnostics) && visible && (diagnostics&0xA5u)==0xA1u);
    const auto beforeHidden=target->readback();assert(beforeHidden);
    assert(draw(0,{32,352,64,64},{},&visible,&diagnostics) && !visible && (diagnostics&0xA5u)==0xA1u);
    assert(target->readback()->colors==beforeHidden->colors);
    diagnostics=0x12345678u;assert(!draw(2,{0,0,1,1},{},&visible,&diagnostics) && diagnostics==0x12345678u);
    program.pixelCode=originalPixel;tileWord(input,8204,0);
    std::puts("NATIVE_QUERY_STAGE_DIAGNOSTICS_GPU_PASS kill_observed=1 late_depth_rejection_distinguished=1 invalid_preserves=1 kill_preserves_images=1 authored_shader=1 xenos_equivalence=0");
    for(unsigned mask=1;mask<16;++mask) {
        program.geometryMrtOutputs=mask;program.pixelCode=shader("subset-"+std::to_string(mask)+"-format-0.frag.spv");
        assert(target->clear(clear) && draw(0,{32,352,64,64}));
        auto s=target->readback();assert(s);
        for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x) {
            const auto at=y*1280+x;const bool inside=x>=32 && x<96 && y>=352 && y<384;
            for(unsigned slot=0;slot<4;++slot)for(unsigned c=0;c<4;++c) {
                const auto expected=inside && (mask&(1u<<slot))?std::uint8_t(((slot+1)&(1u<<c))?255:0):clear.colors[slot][c];
                assert(s->colors[slot][at*4+c]==expected);
            }
            assert(s->depth[at]==(inside?.5f:0.f) && s->stencil[at]==(inside?91:37));
        }
    }
    std::puts("MRT_SUBSETS_PASS subsets=15 sparse_outputs_preserved=1 bin_scissor=1 color_depth_stencil_exact=1");
    // RB_COLOR_MASK is per attachment AND per component, independent of the
    // shader's complete exports. Preserve channels masked off at the merger.
    for(unsigned mask=0;mask<16;++mask) {
        for(unsigned slot=0;slot<4;++slot)options.colorWriteMasks[slot]=(mask+slot)&15;
        assert(target->clear(clear) && draw(0,{32,352,64,64}));
        auto s=target->readback();assert(s);
        for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x) {
            const auto at=y*1280+x;const bool inside=x>=32 && x<96 && y>=352 && y<384;
            for(unsigned slot=0;slot<4;++slot)for(unsigned c=0;c<4;++c) {
                const auto expected=inside && (options.colorWriteMasks[slot]&(1u<<c))?
                    std::uint8_t(((slot+1)&(1u<<c))?255:0):clear.colors[slot][c];
                assert(s->colors[slot][at*4+c]==expected);
            }
            assert(s->depth[at]==(inside?.5f:0.f) && s->stencil[at]==(inside?91:37));
        }
    }
    options.colorWriteMasks={15,15,15,15};
    std::puts("MRT_CHANNEL_MASKS_PASS masks_per_slot=16 independent_channels=1 preserved_alpha=1 depth_stencil_unchanged_policy=1");
    // Upload four genuinely different borrowed textures, bound in translated
    // fetch order5/6/3/4. Pixel MAX nodes and descriptor routing run on the GPU.
    const auto desc=XenosTextureDescription::decode({(1u<<22)|2,0x10000006u,0,0xD10u|(21u<<19),0,0x200});assert(desc);
    const auto texturePlan=planXenosTexture2D(*desc);assert(texturePlan);
    std::array<std::unique_ptr<NativeTextureImage>,4> images;
    std::array<std::array<std::uint8_t,4>,4> textureColors{};
    VkSamplerCreateInfo si{};si.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;si.magFilter=si.minFilter=VK_FILTER_NEAREST;
    si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler{};assert(vkCreateSampler(device.device,&si,nullptr,&sampler)==VK_SUCCESS);
    for(unsigned slot=0;slot<4;++slot) {
        textureColors[slot]={std::uint8_t(31+slot*41),std::uint8_t(151-slot*29),std::uint8_t(83+slot*31),std::uint8_t(251-slot*47)};
        std::vector<std::uint8_t> bytes(texturePlan->sourceBytes);std::copy(textureColors[slot].begin(),textureColors[slot].end(),bytes.begin());
        images[slot]=NativeTextureImage::create(device,*desc,bytes);assert(images[slot]);
        options.externalTextures.push_back({sampler,images[slot]->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }
    const std::vector<XenosTextureDescription> textures(4,*desc);tileWord(input,4,4);program.geometryMrtOutputs=15;
    for(unsigned centroid=0;centroid<2;++centroid)for(unsigned rotation=0;rotation<2;++rotation) {
        program.pixelCode=shader("four-textures-linear-centroid-"+std::to_string(centroid)+".frag.spv");
        program.geometryCentroidMask=centroid;
        assert(target->clear(clear) && draw(0,{0,0,1280,720},textures) && draw(1,{0,0,1280,720},textures));
        const auto s=target->readback();assert(s);
        for(unsigned at=0;at<1280*720;++at) {
            for(unsigned slot=0;slot<4;++slot)for(unsigned c=0;c<4;++c)assert(s->colors[slot][at*4+c]==textureColors[(slot+rotation)%4][c]);
            assert(s->depth[at]==.5f && s->stencil[at]==91);
        }
        // Different native float32 but same20e4: later draw must fail GREATER.
        std::rotate(options.externalTextures.begin(),options.externalTextures.begin()+1,options.externalTextures.end());
        vertices(std::bit_cast<float>(0x3F000004u));assert(draw(0,{0,0,1280,720},textures));
        assert(target->readback()->colors==s->colors);vertices(std::bit_cast<float>(0x3F000002u));
        if(rotation)std::rotate(options.externalTextures.begin(),options.externalTextures.begin()+2,options.externalTextures.end());
    }
    std::puts("CURRENT_TILE_MRT_TEXTURE_PASS independent_textures=4 rotated_control=1 centroid_modes=2 pretest20e4=1 actual_scene_shaders=0");
    // Actual GPU draw -> PRE-clear resolve -> POST-clear physical reuse -> next
    // tile draw, with persistent Xenos destination bytes and padding checked.
    assert(target->clear(clear));vertices(.5f);assert(draw(0,{0,0,1280,720},textures));
    std::array<std::vector<std::uint8_t>,4> destinations;
    for(unsigned tile=0;tile<2;++tile) {
        const auto& r=layout.tiles[tile].logical;
        for(unsigned slot:{0u,2u,3u}) {
            auto f=authoredTileResolve(capture);f.request.flags=0x110|slot;tileWord(*f.surfaces.state,13084,layout.tiles[tile].selectLow);
            for(unsigned i=0;i<4;++i)tileWord(*f.arguments.rectangle,i*4,r[i]);
            for(unsigned i=0;i<2;++i)tileWord(*f.arguments.point,i*4,r[i]);
            const auto p=tileResolvePlan(f);assert(p);if(!tile)destinations[slot].assign(p->storage.sourceBytes,0xA5);
            auto linear=linearizeXenosTexture2D(p->storage.description,destinations[slot]);assert(linear);
            for(unsigned y=r[1];y<r[3];++y)for(unsigned x=0;x<1280;++x)for(unsigned c=0;c<4;++c)
                (*linear)[(y*1280+x)*4+c]=textureColors[slot][c];
            const auto expected=storeXenosTexture2D(p->storage.description,*linear,destinations[slot]);assert(expected);
            const auto output=target->prepareCapturedColorResolve(f.surfaces,f.request,f.arguments,f.destination,destinations[slot]);
            assert(output && *output==*expected);destinations[slot]=*output;
        }
        if(!tile) {
            assert(target->carryCapturedTile(capture,0,1));vertices(.75f);
            assert(draw(1,{0,0,1280,720},textures));
        }
    }
    std::puts("CURRENT_TILE_MRT_DRAW_PASS actual_headers=5 draw_resolve_carry_draw=1 resolves=6 exact_destination_bytes=1 guest_publication=0");
    const auto prior=target->readback();assert(prior);const auto serial=target->completedDrawSerial();
    auto saved=program;program.abiVersion=13;assert(!draw(0,{0,0,1280,720},textures));program=saved;
    program.geometryMrtFormats[2]=1;assert(!draw(0,{0,0,1280,720},textures));program=saved;
    options.colorAttachmentCount=1;assert(!draw(0,{0,0,1280,720},textures));options.colorAttachmentCount=4;
    device.independentBlendEnabled=false;assert(!draw(0,{0,0,1280,720},textures));device.independentBlendEnabled=true;
    options.colorWriteMasks[1]=16;assert(!draw(0,{0,0,1280,720},textures));options.colorWriteMasks[1]=15;
    assert(!draw(2,{0,0,1280,720},textures) && !draw(0,{1281,0,0,0},textures));
    auto missing=capture;missing.surfaces[2].header.reset();
    assert(!target->drawCapturedIndexed(missing,0,program,input,3,{0,1,2},{1,1,0,0},{0,0,1280,720},textures,options));
    const auto after=target->readback();assert(after && after->colors==prior->colors && after->depth==prior->depth &&
        after->stencil==prior->stencil && target->completedDrawSerial()==serial);
    auto fresh=NativeLogicalTileTarget::create(device,*capture.state);assert(fresh);
    assert(!fresh->drawCapturedIndexed(capture,0,program,input,3,{0,1,2},{1,1,0,0},{0,0,1280,720},textures,options));
    auto dsOnly=clear;dsOnly.aspects=0x30;assert(fresh->clear(dsOnly));
    assert(fresh->drawCapturedIndexed(capture,0,program,input,3,{0,1,2},{1,1,0,0},{0,0,32,32},textures,options));
    // The depth0/full-color writer establishes a per-pixel implication, not
    // whole-tile initialization. This exact region was drawn and is readable;
    // extending by one untouched pixel must still reject publication.
    const auto writtenRegion=fresh->readColorRegion(0,{0,0,32,32});assert(writtenRegion);
    for(unsigned pixel=0;pixel<32*32;++pixel)for(unsigned channel=0;channel<4;++channel)
        assert((*writtenRegion)[pixel*4+channel]==textureColors[0][channel]);
    assert(!fresh->readColorRegion(0,{0,0,33,32}) && !fresh->readColorRegion(0,{32,0,33,1}));
    assert(!fresh->readColorRegion(0,{0,0,1280,384}) && !fresh->readback());
    // Nonfinite original VS inputs invalidate publication, not just the draw.
    tileFloat(input,8208,std::numeric_limits<float>::infinity());
    assert(!draw(0,{0,0,1280,720},textures) && !target->readback());
    options.externalTextures.clear();vkDestroySampler(device.device,sampler,nullptr);
    std::puts("CURRENT_TILE_MRT_REJECTION_PASS missing_feature_format_header_abi_rejected=1 prior_data_preserved=1 undefined_not_promoted=1 invalid_vertex_retires=1 gameplay=0");
}
