// Private build supplies a reviewed generated shader pack through an explicit
// include macro. No game shader bytes or capture data belong in this file.
#ifndef RECOMP_PRIVATE_COMPOSITE_PACK
#error An explicit private shader pack is required
#endif
#include RECOMP_PRIVATE_COMPOSITE_PACK
#include "native_offscreen_draw_target.h"
#include "native_vertex_clip_executor.h"
#include "native_edram32_bands.h"
#include "native_direct_depth_target.h"
#include "composite_derived_loop_fixture.h"
#include <cassert>
#include <cstdio>
#include <fstream>
using namespace recomp::gpu;
int main(int argc,char** argv) {
    assert(argc==3);std::setvbuf(stdout,nullptr,_IONBF,0);const std::string folder=argv[1];
    assert(std::string(argv[2])=="0" || std::string(argv[2])=="32");
    const unsigned capturedBase=std::string(argv[2])=="32"?32:0;
    const auto read=[&](const char* name,unsigned expected) {
        std::ifstream f(folder+"/"+name,std::ios::binary|std::ios::ate);
        assert(f && f.tellg()==expected);f.seekg(0);
        std::vector<std::uint8_t> b(expected);assert(f.read(reinterpret_cast<char*>(b.data()),expected));return b;
    };
    const auto word=[](const auto& b,unsigned at) {
        return (std::uint32_t{b[at]}<<24)|(std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];
    };
    // Generated from this capture's JSON by the hash-bound private preparer.
    const auto meta=read("replay-metadata.bin",12*4);
    Gta5InlineDraw draw;draw.primitive=word(meta,0);draw.vertexCount=word(meta,4);draw.stride=word(meta,8);
    assert(draw.primitive==8 && draw.vertexCount==3 && draw.stride==24);
    draw.scissorRegisters=std::array<std::uint32_t,3>{word(meta,12),word(meta,16),word(meta,20)};
    draw.deviceState=read("device-state.bin",13860);draw.vertices=read("vertices.bin",72);
    const auto vc=read("vertex-constants.bin",4096),pc=read("pixel-constants.bin",4096);
    for(unsigned i=0;i<1024;++i){draw.vertexConstants[i]=word(vc,4*i);draw.pixelConstants[i]=word(pc,4*i);}
    const auto& program=nativeCompositeProgram;
    assert(read("vertex-shader.bin",program.draw.vertexIdentity.size())==program.draw.vertexIdentity);
    assert(read("pixel-shader.bin",program.draw.pixelIdentity.size())==program.draw.pixelIdentity);
    Gta5InlineDraw::Texture texture;texture.slot=2;
    for(unsigned i=0;i<6;++i)texture.fetch[i]=word(meta,24+4*i);
    texture.source=read("texture-2.bin",3768320);draw.textures={texture};
    Gta5BoundSurfaceCapture bindings;
    for(unsigned i=0;i<5;++i){bindings.surfaces[i].object=word(draw.deviceState,12816+4*i);
        bindings.surfaces[i].tileCacheObject=word(draw.deviceState,13104+4*i);}
    bindings.surfaces[0].header=read("surface-0-argument.bin",40);
    bindings.surfaces[0].tileCacheHeader=read("surface-0-tile-cache.bin",40);
    if(bindings.surfaces[4].object)bindings.surfaces[4].header=read("surface-4-argument.bin",40);
    const char* why=nullptr;const auto plan=planNativeCompositeRectangleDraw(draw,program.draw,bindings,&why);
    if(!plan)std::printf("COMPOSITE_CAPTURE_PLAN_REFUSED %s\n",why);
    assert(plan && plan->baseTile==capturedBase && plan->width==1280 && plan->height==32 && plan->object==0x640af9e0);
    const auto description=XenosTextureDescription::decode(texture.fetch);assert(description);
    assert(description->width==1280 && description->height==720 && description->format==6);
    assert((description->swizzle==std::array<unsigned,4>{2,1,0,3}));
    const auto source=linearizeXenosTexture2D(*description,texture.source);assert(source);
    // Guard the oracle's assumptions against exactly captured constants.
    assert(draw.vertexConstants[920]==0x44A00000u && draw.vertexConstants[921]==0x42000000u);
    assert(draw.vertexConstants[922]==0 && draw.vertexConstants[923]==(capturedBase?0x42000000u:0u));
    assert(draw.vertexConstants[1004]==0x3F000000u && draw.vertexConstants[1005]==0xBF000000u);
    assert(draw.pixelConstants[1020]==0x3C4CCCCDu && draw.pixelConstants[1021]==0x3F000000u &&
           draw.pixelConstants[1022]==0xC2200000u && draw.pixelConstants[1023]==0x42200000u);
    ProbeDevice device;ShaderRequirements requirements;
    assert(device.inspect() && device.create(requirements));
    {NativeOffscreenDrawPlan wide{0x1000,160,32,160,0,8,7};
     assert(NativeOffscreenDrawTarget::create(device,wide));
     wide.tileCount=4;assert(!NativeOffscreenDrawTarget::create(device,wide));
     wide.format=14;assert(NativeOffscreenDrawTarget::create(device,wide));
     wide.format=6;assert(!NativeOffscreenDrawTarget::create(device,wide));}
    std::puts("OFFSCREEN_FORMAT_CAPACITY_GPU_PASS rgba16f_tiles=8 undercount_refused=1 r32f_tiles=4 unknown_refused=1");
    auto executor=NativeVertexClipExecutor::create(device,program.positions);assert(executor);
    const auto positions=executor->rectanglePositions(draw);assert(positions && nativeFullRectangleList(draw,*positions));
    auto target=NativeOffscreenDrawTarget::create(device,*plan);assert(target);
    NativeEdram32Bands bands;std::optional<std::vector<std::uint8_t>> output;
    const bool drawn=[&]{NativeEdram32Mutation mutation(bands);
        return executeNativeEdram32ColorBand(mutation,*plan,
            [&]{assert(!bands.knownTiles());return target->drawCompositeRectangles(program.draw,draw,bindings,*positions,&why);},
            [&]{output=target->readRgba();return output;});}();
    std::printf("COMPOSITE_CAPTURE_DRAW drawn=%u stage=%s current_base=%u cached_base=%u\n",unsigned(drawn),why,
        plan->baseTile,word(*bindings.surfaces[0].tileCacheHeader,28)&2047);
    assert(drawn && output && output->size()==1280*32*4);
    unsigned mismatches=0;
    for(unsigned y=0;y<32;++y)for(unsigned x=0;x<1280;++x) {
        const unsigned sx=x/80*80+(x%80+40)%80;
        for(unsigned c=0;c<4;++c)if((*output)[4*(y*1280+x)+c]!=(*source)[4*((y+capturedBase)*1280+sx)+c])++mismatches;
    }
    std::printf("COMPOSITE_CAPTURE_BYTES mismatches=%u bytes=%zu\n",mismatches,output->size());assert(!mismatches);
    const auto depth=bands.readDepthWords(capturedBase,1280,1280,32);assert(depth);
    for(unsigned i=0;i<1280*32;++i) {
        const auto at=4*(i+capturedBase*1280);const auto expected=std::uint32_t{(*source)[at]}|(std::uint32_t{(*source)[at+1]}<<8)|
            (std::uint32_t{(*source)[at+2]}<<16)|(std::uint32_t{(*source)[at+3]}<<24);
        assert((*depth)[i]==expected);
    }
    assert(!bands.readDepthWords(0,1280,1280,720)); // Only this captured band is known.
    assert(bands.invalidate(capturedBase,1) && !bands.readDepthWords(capturedBase,1280,1280,32));
    std::puts("COMPOSITE_CAPTURE_GPU_PASS current_target=1 original_translated_pair=1 rgba_bytes=163840 exact=1 depth_alias_rows=32 full720_unknown=1 gameplay=0");
    std::printf("COMPOSITE_CAPTURE_BAND_PASS base=%u actual_inputs=1 gpu=1 gameplay=0\n",capturedBase);
    if(capturedBase)return 0; // Do not relabel the following DERIVED loop as live.
    // Separate DERIVED experiment: 823A64D8's loop increments the colour base
    // by32 tiles and c230.w by32 texels. All other submitted shader inputs are
    // retained. Iterations0/1 have separate live captures; do not call these23 live
    // captures or a proof of live ownership. The unused depth binding names
    // the previous band after the first iteration. Original setters enable
    // ALWAYS/no-write depth and paired HiZ; retaining first-band state was an
    // earlier fixture error. Actual second-capture replay is separate above.
    for(bool authoredPartialTail:{false,true}) {
    bands.reset();unsigned totalMismatches=0;
    for(unsigned band=0;band<23;++band) {
        auto next=draw;auto bound=bindings;const auto base=band*32;
        test::deriveCompositeRestoreBand(next,bound,band,authoredPartialTail);
        const auto bandPlan=planNativeCompositeRectangleDraw(next,program.draw,bound,&why);
        if(!bandPlan)std::printf("COMPOSITE_DERIVED_PLAN_REFUSED band=%u stage=%s\n",band,why);
        assert(bandPlan);
        auto bandTarget=NativeOffscreenDrawTarget::create(device,*bandPlan);assert(bandTarget);
        const auto bandPositions=executor->rectanglePositions(next);assert(bandPositions);
        std::optional<std::vector<std::uint8_t>> rgba;
        NativeEdram32Mutation mutation(bands);
        assert(executeNativeEdram32ColorRows(mutation,*bandPlan,
            [&]{assert(!bands.knownTiles());return bandTarget->drawCompositeRectangles(program.draw,next,bound,*bandPositions,&why);},
            [&]{rgba=bandTarget->readCompletedColorRows();return rgba;},
            [&]{return bandTarget->completedColorRows();}));
        const unsigned rows=authoredPartialTail && band==22?16:32;
        assert(bandTarget->completedColorRows()==rows && rgba->size()==1280*rows*4);
        assert(bandTarget->valid()==(rows==32) && bandTarget->readRgba().has_value()==(rows==32));
        for(unsigned y=0;y<rows;++y)for(unsigned x=0;x<1280;++x) {
            const unsigned sx=x/80*80+(x%80+40)%80,sy=std::min(719u,base+y);
            for(unsigned c=0;c<4;++c)
                if((*rgba)[4*(y*1280+x)+c]!=(*source)[4*(sy*1280+sx)+c])++totalMismatches;
        }
        assert(!totalMismatches);
        if(band<22)assert(!bands.readDepthWords(0,1280,1280,720));
    }
    assert(bands.knownTiles()==(authoredPartialTail?720:736));
    assert(bands.readDepthWords(720,1280,1280,16).has_value()==!authoredPartialTail);
    const auto fullDepth=bands.readDepthWords(0,1280,1280,720);assert(fullDepth);
    for(unsigned i=0;i<1280*720;++i) {
        const auto at=4*i;const auto expected=std::uint32_t{(*source)[at]}|(std::uint32_t{(*source)[at+1]}<<8)|
            (std::uint32_t{(*source)[at+2]}<<16)|(std::uint32_t{(*source)[at+3]}<<24);
        assert((*fullDepth)[i]==expected);
    }
    // Disjoint postprocessing must not erase the restored720 tiles. A real
    // overlapping writer must invalidate even one tile; object identity is
    // intentionally irrelevant to this physical byte ownership condition.
    assert(bands.invalidate(720,720) && bands.readDepthWords(0,1280,1280,720)==fullDepth);
    assert(bands.invalidate(500,1) && !bands.readDepthWords(0,1280,1280,720));
    std::printf("COMPOSITE_DERIVED_LOOP_GPU_PASS bands=23 exact_depth_words=921600 final_rows=%u authored_partial_tail=%u overlap_invalidates=1 derived_not_live=1 gameplay=0\n",
        authoredPartialTail?16:32,unsigned(authoredPartialTail));
    // Storage connection only: import the derived restored words into the
    // depth/stencil GPU image without a clear and read both aspects back.
    // This authored storage identity is NOT the game's live binding/owner.
    NativeStencilOnlyPlan storage;storage.depthObject=0x1000;
    storage.width=storage.pitch=1280;storage.height=720;
    auto depthTarget=NativeDirectDepthTarget::create(device,storage,*fullDepth);assert(depthTarget);
    assert(depthTarget->readWords()==fullDepth);
    std::puts("COMPOSITE_TRANSACTION_GPU_PASS production_completion_helper=1 failed_unknown_host_tested=1 original_shader=1 derived_loop=1 gameplay=0");
    }
    std::puts("COMPOSITE_DEPTH_STORAGE_GPU_PASS words=921600 exact=1 no_clear=1 live_owner=0 stencil_draw=0 gameplay=0");
}
