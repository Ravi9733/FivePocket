#pragma once
#include "sampled_stencil_fixture.h"
#include "native_sampled_stencil_draw.h"
#include "native_stencil_clear.h"
#include "native_stopped_physical32_capture.h"
#include "post_sampled_stencil_fixture.h"

namespace recomp::gpu::test {
inline void checkSampledStencilConnection(ProbeDevice& gpu,const SampledStencilFixture& f,
        const Gta5ClearCapture& clear,const NativeEdram32Bands& producer,
        const std::vector<unsigned>& color,const PostSampledStencilFixture* rejected=nullptr) {
    const char* why=nullptr;const auto plan=planNativeSampledStencil(f.draw,f.program,f.bindings,&why);assert(plan);
    const auto original=linearizeXenosTexture2D(plan->texture,f.draw.textures[0].source);assert(original);
    const auto scissor=nativeTranslatedScissor(f.draw);assert((scissor && *scissor==std::array<unsigned,4>{0,0,1280,384}));
    unsigned actualWritten=0;
    // The original caller restores its scissor and repeats this pass. The
    // second720-row region here is DERIVED state, not a new live capture.
    for(unsigned rows:std::array<unsigned,2>{384,720}) {
      if(rows==720 && !rejected)continue;
      for(unsigned mode=0;mode<3;++mode) {
        auto draw=f.draw;auto linear=*original;
        (*draw.scissorRegisters)[2]=(rows<<16)|1280;
        if(mode) {
            // Authored adversarial textures; original shaders/vertices/state
            // remain actual. These must distinguish sampling from a blanket
            // stencil fill and from an unsupported no-op.
            for(unsigned p=0;p<1280*720;++p)linear[4*p+3]=mode==1?(p%1280+p/1280)%3?255:0:255;
            const auto stored=storeXenosTexture2D(plan->texture,linear,draw.textures[0].source);assert(stored);
            draw.textures[0].source=*stored;
        }
        auto owner=producer;
        {NativeEdram32Mutation mutation(owner);assert(executeNativeStencilClear(clear,mutation,[]{return true;}));}
        const auto before=owner.readDepthWords(0,1280,1280,720);assert(before);
        {NativeEdram32Mutation mutation(owner);
            const bool ok=executeNativeSampledStencil(gpu,f.program,draw,f.bindings,
                f.program.vertexIdentity,f.program.pixelIdentity,mutation,[]{return true;},&why);
            if(!ok)std::printf("SAMPLED_STENCIL_GPU_REFUSED mode=%u stage=%s\n",mode,why);
            assert(ok);}
        const auto after=owner.readDepthWords(0,1280,1280,720);assert(after);unsigned written=0,expectedPassing=0;
        for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x) {
            const auto at=y*1280+x;const bool pass=y<rows && linear[4*at+3]==0;
            const auto expected=pass?(*before)[at]|16:(*before)[at];written+=pass;
            if(y<rows && !(expected&16))++expectedPassing;
            if((*after)[at]!=expected)std::printf("SAMPLED_STENCIL_PIXEL_MISMATCH mode=%u x=%u y=%u alpha=%u before=%08x expected=%08x actual=%08x\n",
                mode,x,y,linear[4*at+3],(*before)[at],expected,(*after)[at]);
            assert((*after)[at]==expected);
        }
        assert(owner.readColorWords(720,1280,1280,720)==color);
        const auto serial=owner.nextSerial();
        const auto snapshot=captureNativeStoppedPhysical32(owner,draw.deviceState);
        assert(snapshot && snapshot->serial==serial-1 && snapshot->color==color && snapshot->depth==*after);
        assert(owner.nextSerial()==serial);
        if(rejected){auto next=*rejected;(*next.draw.scissorRegisters)[2]=(rows<<16)|1280;
            checkPostSampledStencilConnection(next,owner,expectedPassing);
            assert((mode==0)==(expectedPassing==0));
            if(rows==384)assert(expectedPassing==1280*384-written);}
        if(mode==0 && rows==384)actualWritten=written;
        if(mode==1)assert(written==1280*rows/3);
        if(mode==2)assert(written==0);
      }
    }
    for(unsigned mode=0;mode<3;++mode) {
        auto owner=producer;unsigned observations=0;
        if(mode==0)assert(owner.invalidate(0,1));
        {NativeEdram32Mutation mutation(owner);
            assert(!executeNativeSampledStencil(gpu,f.program,f.draw,f.bindings,
                f.program.vertexIdentity,f.program.pixelIdentity,mutation,[&]{++observations;return mode==0 || (mode==2 && observations==1);},&why));}
        assert(!owner.knownTiles());
        if(mode==2)assert(observations==2);
    }
    std::printf("SAMPLED_STENCIL_CONNECTION_PASS actual_alpha_zero_pixels=%u original_gpu_producer=1 partial_clear=1 actual_shaders=1 actual_texture=1 authored_variants=2 preserved_depth_color_outside=1 stale_unknown_refused=1 authored_initial_owner=1 live_draw=0\n",actualWritten);
    std::puts("STOPPED_PHYSICAL_CAPTURE_CONNECTION_PASS words_per_plane=921600 exact_color_depth=1 ownership_unchanged=1");
    if(rejected)std::puts("POST_SAMPLED_STENCIL_CONNECTION_PASS actual_gpu_producer=1 actual_consumer_state=1 examined=491520 actual_passing=0 mixed_passing=327680 opaque_passing=491520 query_unknown_refuse=1 unchanged_color_depth_serial=1");
    if(rejected)std::puts("POST_SAMPLED_RESTORED_SCISSOR_PASS derived_full720=1 original_gpu_producer=1 actual_alpha_zero_rejects_all=1 mixed_opaque_require_renderer=1 preserved=1");
}
}
