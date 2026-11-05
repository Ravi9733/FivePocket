#include "color8_clear_execution_fixture.h"
#include "native_color8_clear_target.h"
#include <cstdio>
using namespace recomp::gpu;
int main(int argc,char** argv) {
    assert(argc==2);Color8ClearExecutionFixture f(argv[1]);auto a=auditGta5Color8Clear(f.capture);
    assert(!a.issues && a.plan.width==320 && a.plan.height==320 && a.plan.samples==1 && !a.plan.depth);
    ProbeDevice device;assert(device.inspect() && device.create({}));
    auto target=NativeColor8ClearTarget::create(device,a.plan);assert(target);
    unsigned positives=0;
    for(bool displayBefore:{false,true}) {
        bool displayValid=displayBefore;NativeEdram32Bands physical;seedColor8ClearPhysical(physical);
        const auto preserved=physical.readColorWords(720,80,80,16);NativeEdram32Mutation mutation(physical);
        unsigned rechecks=0;
        const bool done=executeNativeColor8ClearWrite(a,true,0x10059AE0,mutation,
            [&]{++rechecks;return f.capture.state==Color8ClearExecutionFixture(argv[1]).capture.state;},
            [&]{return target->clear(a.plan.color);},[&]{return target->readUniformResolved();});
        if(done)displayValid=displayBefore;
        assert(done && rechecks==2 && displayValid==displayBefore && physical.readColorWords(720,80,80,16)==preserved);
        const auto raw=physical.readColorWords(a.plan.baseTile,a.plan.pitch,a.plan.width,a.plan.height);
        assert(raw && raw->size()==320*320 && std::all_of(raw->begin(),raw->end(),[](auto v){return v==0;}));++positives;
    }
    unsigned authoredCoupled=0;
    for(unsigned samples:{1u,2u,4u})for(unsigned format:{0u,1u}) {
        Gta5Color8ClearAudit coupled;
        coupled.plan={0x1000,128,128,160,0,16*samples,samples,format,{17,91,203,255}, {}};
        coupled.plan.depth=Gta5Depth24ClearPlan{0x2000,128,128,160,16*samples,16*samples,samples,1,*planExactXenosFloat24Clear(0,255)};
        auto t=NativeColor8ClearTarget::create(device,coupled.plan);assert(t);
        NativeEdram32Bands bands;seedColor8ClearPhysical(bands);const auto old=bands.readColorWords(720,80,80,16);
        NativeEdram32Mutation transaction(bands);
        assert(executeNativeColor8ClearWrite(coupled,true,0x10059AE0,transaction,[]{return true;},
            [&]{return t->clear(coupled.plan.color);},[&]{return t->readUniformResolved();}));
        assert(bands.readColorWords(720,80,80,16)==old);
        // Native depth remains owned by t, never fabricated from resolved RGB.
        assert(!bands.readDepthWords(16*samples,160,80,16));
        const auto rgba=t->readUniformResolved();assert(rgba);
        for(std::size_t i=0;i<rgba->size();++i)assert((*rgba)[i]==coupled.plan.color[i%4]);
        ++authoredCoupled;
    }
    assert(authoredCoupled==6);
    NativeEdram32Bands physical;seedColor8ClearPhysical(physical);NativeEdram32Mutation mutation(physical);unsigned rechecks=0;
    assert(!executeNativeColor8ClearWrite(a,true,0x10059AE0,mutation,[&]{return ++rechecks==1;},
        [&]{return target->clear(a.plan.color);},[&]{return target->readUniformResolved();}));
    assert(!physical.knownTiles());
    std::printf("COLOR8_CLEAR_EXECUTION_GPU_PASS captured_request=1 positives=%u authored_coupled=%u changed_after_gpu_refuses=1 pixels=102400 physical_disjoint_preserved=1 stale_display_not_revived=1 gameplay=0\n",positives,authoredCoupled);
}
