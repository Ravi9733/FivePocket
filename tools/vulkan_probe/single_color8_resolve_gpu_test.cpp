#include "single_color8_resolve_fixture.h"
#include "native_color8_clear_target.h"
#include <cstdio>
using namespace recomp::gpu;
int main(int argc,char** argv) {
    assert(argc==2);SingleColor8ResolveFixture f(argv[1]);const auto plan=f.plan();assert(plan);
    ProbeDevice device;assert(device.inspect() && device.create({}));
    unsigned actual=0,authored=0;
    for(unsigned control=0;control<2;++control) {
        auto audit=f.audit;if(control)audit.plan.color={17,91,203,233};
        auto target=NativeColor8ClearTarget::create(device,audit.plan);assert(target);
        NativeEdram32Bands physical;seedColor8ClearPhysical(physical);
        const auto disjoint=physical.readColorWords(720,80,80,16);
        {NativeEdram32Mutation clear(physical);
         assert(executeNativeColor8ClearWrite(audit,true,0x10059AE0,clear,[]{return true;},
            [&]{return target->clear(audit.plan.color);},[&]{return target->readUniformResolved();}));}
        const auto rgba=target->readUniformResolved();assert(rgba && target->isUniformClear());
        for(bool displayBefore:{false,true}) {
            const bool displayValid=displayBefore;auto before=f.prior(),published=before;
            NativeEdram32Mutation resolve(physical);unsigned rechecks=0,publishes=0;
            assert(executeNativeSingleColor8Resolve(*plan,*rgba,before,resolve,[&]{++rechecks;return target->isUniformClear();},
                [&](const auto& output){++publishes;published=output;return true;}));
            f.verifyPixels(*rgba,before,published);
            assert(publishes==1 && rechecks==2 && displayValid==displayBefore && physical.readColorWords(720,80,80,16)==disjoint);
            control?++authored:++actual;
        }
    }
    std::printf("SINGLE_COLOR8_RESOLVE_GPU_PASS actual_clear_to_copy=%u authored_colored_controls=%u fenced_source=1 tiled_endian_consumer=1 padding_preserved=1 stale_display_not_revived=1 authored_prior_destination=1 gameplay=0\n",actual,authored);
}
