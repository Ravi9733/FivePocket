#include "native_deferred_color_sync_progress.h"
#include "native_color_sync_progress.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <type_traits>
using namespace recomp::gpu;
static auto snapshot(const NativeDeferredColorSyncProgress& p){
    unsigned busy=99;const bool known=p.busyBit(busy);
    return std::array<unsigned,12>{p.leadingFences(),p.completedStages(),p.invalidations(),p.copyIdleCleans(),
        p.depthWaits(),p.cacheFlushes(),p.literalEvents(),p.complete(),p.readyForStages(),p.depthWaitSatisfied(),known,busy};
}
int main(){
    static_assert(!std::is_convertible_v<NativeDeferredColorSyncProgress,NativeColorSyncProgress>);
    NativeDeferredColorSyncProgress p;unsigned rejected=0;
    const auto refuse=[&](auto operation){const auto before=snapshot(p);assert(!operation() && snapshot(p)==before);++rejected;};
    for(unsigned stage=0;stage<6;++stage){
        refuse([&]{return p.begin(stage);});refuse([&]{return p.finishAuditedStage(stage);});
    }
    assert(!p.readyForStages() && !p.complete() && !p.depthWaitSatisfied());
    assert(p.finishAuditedLeadingFence() && p.readyForStages());
    for(unsigned stage=0;stage<4;++stage){
        refuse([&]{return p.finishAuditedLeadingFence();});
        for(unsigned wrong=0;wrong<6;++wrong){
            if(wrong!=stage)refuse([&]{return p.begin(wrong);});
            refuse([&]{return p.finishAuditedStage(wrong);});
        }
        assert(p.begin(stage) && !p.readyForStages() && !p.complete());
        unsigned busy=99;assert(p.busyBit(busy) && busy==(stage?0u:0x80000000u));
        for(unsigned wrong=0;wrong<6;++wrong){
            refuse([&]{return p.begin(wrong);});
            if(wrong!=stage)refuse([&]{return p.finishAuditedStage(wrong);});
        }
        refuse([&]{return p.finishAuditedLeadingFence();});
        assert(p.finishAuditedStage(stage) && p.completedStages()==stage+1 && p.depthWaitSatisfied());
        assert(p.busyBit(busy) && !busy && p.depthWaits()==1);
        assert(p.invalidations()==(stage<2?1u:2u) && p.copyIdleCleans()==(stage<2?0u:1u));
        assert(p.cacheFlushes()==unsigned(stage==3) && !p.literalEvents());
    }
    assert(p.complete());
    for(unsigned stage=0;stage<6;++stage){
        refuse([&]{return p.begin(stage);});refuse([&]{return p.finishAuditedStage(stage);});
    }
    refuse([&]{return p.finishAuditedLeadingFence();});
    for(unsigned stop=0;stop<4;++stop){
        NativeDeferredColorSyncProgress failed;assert(failed.finishAuditedLeadingFence());
        for(unsigned stage=0;stage<stop;++stage)assert(failed.begin(stage) && failed.finishAuditedStage(stage));
        assert(failed.begin(stop));const auto before=snapshot(failed);
        for(unsigned n=0;n<1000;++n){
            assert(!failed.begin(stop) && !failed.begin(stop+1) && !failed.finishAuditedStage(stop+1));
            assert(snapshot(failed)==before && !failed.complete());
        }
    }
    std::printf("DEFERRED_COLOR_SYNC_PROGRESS: %u ordering refusals,4000 interrupted-stage controls; no old final events PASS\n",rejected);
}
