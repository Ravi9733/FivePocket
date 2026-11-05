#include "native_color_sync_progress.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main(){
    NativeColorSyncProgress model;std::uint32_t value=99;unsigned rejections=0;
    assert(!model.busyBit(value) && value==99 && !model.complete() && !model.depthWaitSatisfied());
    for(unsigned stage=0;stage<4;++stage){
        for(unsigned wrong=0;wrong<6;++wrong){
            if(wrong!=stage){assert(!model.begin(wrong));++rejections;}
            assert(!model.finishAuditedStage(wrong));++rejections;
        }
        assert(model.begin(stage));
        for(unsigned wrong=0;wrong<6;++wrong){
            assert(!model.begin(wrong));++rejections;
            if(wrong!=stage){assert(!model.finishAuditedStage(wrong));++rejections;}
        }
        assert(model.completedStages()==stage && !model.complete());
        assert(model.busyBit(value) && value==(!stage?0x80000000u:0u));
        if(!stage)assert(!model.depthWaitSatisfied() && !model.depthWaits());
        assert(model.finishAuditedStage(stage));
        assert(model.completedStages()==stage+1 && model.depthWaitSatisfied() && model.depthWaits()==1);
        assert(model.busyBit(value) && !value);
        assert(model.invalidations()==(stage<2?1u:stage==2?2u:3u));
        assert(model.idleCleans()==(stage<2?0u:stage==2?1u:3u));
        assert(model.cacheFlushes()==(stage==3?1u:0u) && model.literalEvents()==(stage==3?2u:0u));
    }
    assert(model.complete());
    for(unsigned wrong=0;wrong<6;++wrong){
        assert(!model.begin(wrong) && !model.finishAuditedStage(wrong) && model.complete());rejections+=2;
    }
    // An interrupted/failed first stage has no completion by passage of time
    // or by attempting a later stage. These controls are CPU model tests only.
    NativeColorSyncProgress incomplete;assert(incomplete.begin(0));
    for(unsigned i=0;i<1000;++i){assert(!incomplete.begin(1) && !incomplete.finishAuditedStage(1));
        assert(incomplete.busyBit(value) && value==0x80000000 && !incomplete.depthWaitSatisfied());}
    std::printf("NATIVE_COLOR_SYNC_PROGRESS: %u ordering rejections, 1000 incomplete-wait controls; CPU model only PASS\n",rejections);
}
