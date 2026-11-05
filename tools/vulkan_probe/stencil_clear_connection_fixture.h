#pragma once
#include "color8_clear_execution_fixture.h"
#include "native_stencil_clear.h"

namespace recomp::gpu::test {
// Real original GPU draw -> paired physical output -> actual captured clear.
// Initial physical contents remain authored, not a live EDRAM capture.
inline void checkStencilClearConnection(const Gta5ClearCapture& capture,
        const NativeEdram32Bands& source,const std::vector<unsigned>& depth,
        const std::vector<unsigned>& color) {
    const auto p=planNativeStencilClear(capture);assert(p && p->left==0 && p->top==0 &&
        p->right==1280 && p->bottom==384 && p->stencil==0);
    assert(depth.size()==1280*720 && color.size()==depth.size());
    auto owner=source;
    {NativeEdram32Mutation mutation(owner);assert(executeNativeStencilClear(capture,mutation,
        [&]{return sameNativeStencilClearInputs(capture,capture);}));}
    const auto after=owner.readDepthWords(0,1280,1280,720);assert(after);
    unsigned changed=0,nonzeroDepth=0;
    for(unsigned i=0;i<depth.size();++i) {
        const unsigned expected=i<1280*384?(depth[i]&0xFFFFFF00u):depth[i];
        assert((*after)[i]==expected);changed+=(*after)[i]!=depth[i];nonzeroDepth+=(depth[i]>>8)!=0;
    }
    assert(changed && nonzeroDepth);
    assert(owner.readColorWords(720,1280,1280,720)==color && owner.nextSerial()==source.nextSerial()+1);
    for(unsigned mode=0;mode<3;++mode) {
        owner=source;auto stale=capture;
        if(mode==0)assert(owner.invalidate(0,1));
        if(mode==1)(*stale.rectangle)[15]^=1;
        if(mode==2)(*stale.surfaces[4].header)[28]^=1;
        {NativeEdram32Mutation mutation(owner);assert(!executeNativeStencilClear(capture,mutation,
            [&]{return sameNativeStencilClearInputs(capture,stale);}));}
        assert(!owner.knownTiles());
    }
    std::puts("INDEXED602_STENCIL_CLEAR_CONNECTION_PASS actual_A0=1 original_gpu_producer=1 depth_words_preserved=921600 color_words_preserved=921600 stencil_inside=491520 outside_preserved=430080 unknown_stale_refused=1 authored_initial_owner=1 live_clear_unproven=1");
}
}
