#include "single_color8_resolve_fixture.h"
#include <cstdio>
using namespace recomp::gpu;
int main(int argc,char** argv) {
    assert(argc==2);SingleColor8ResolveFixture f(argv[1]);const auto p=f.plan();assert(p);
    assert(p->source.width==320 && p->source.height==320 && p->source.baseTile==1521);
    // Reproduce BOTH independent old refusals. Neither is a GPU result.
    unsigned calls=0;assert(!observeNativeResolveAttempt(true,false,[&]{++calls;return true;},[](auto){}));assert(!calls);
    assert(!nativeColor8ResolveSourceMatches(f.audit.plan,f.state,f.bindings));
    assert(!auditNativeColor8ResolveArguments(f.audit.plan,f.request,f.arguments,&f.destination).plan);
    unsigned planNegatives=0;
    for(unsigned fault=0;fault<20;++fault) {
        auto x=f;bool owned=true,uniform=true;unsigned display=0x10059AE0;
        if(fault==0)owned=false;if(fault==1)uniform=false;if(fault==2)display=0;
        if(fault==3)display=x.audit.plan.object;if(fault==4)x.audit.plan.samples=2;
        if(fault==5)x.audit.plan.format=1;if(fault==6)x.audit.plan.baseTile=720;
        if(fault==7)x.audit.plan.tileCount++;if(fault==8)x.state.pop_back();
        if(fault==9)x.header[24]^=1;if(fault==10)x.bindings.surfaces[1].object=4;
        if(fault==11)x.bindings.surfaces[0].tileCacheObject^=4;
        if(fault==12)x.state[11068]|=0x10;if(fault==13)x.state[10372]^=1;
        if(fault==14)x.request.flags=0x100;if(fault==15)x.request.mip=1;
        if(fault==16)x.request.slice=1;if(fault==17)(*x.arguments.rectangle)[0]=1;
        if(fault==18)(*x.arguments.stack100)[0]=1;if(fault==19)x.destination.fetch[1]^=1;
        assert(!planNativeSingleColor8Resolve(x.audit.plan,owned,display,uniform,x.state,x.bindings,x.header,x.destination,x.request,x.arguments));++planNegatives;
    }
    unsigned positives=0,negatives=0;
    for(unsigned fault=0;fault<8;++fault) {
        NativeEdram32Bands physical;seedColor8ClearPhysical(physical);
        std::vector<std::uint8_t> rgba(320*320*4,0);auto before=f.prior(),after=before;
        assert(physical.recordCompletedRawColorRows(1521,320,320,320,rgba,physical.nextSerial()));
        if(fault==1)physical.invalidate(1521,80);if(fault==2)rgba[3]=1;
        if(fault==3)rgba.pop_back();if(fault==4)before.pop_back();
        NativeEdram32Mutation mutation(physical);unsigned checks=0,publishes=0;
        const bool done=executeNativeSingleColor8Resolve(*p,rgba,before,mutation,
            [&]{++checks;return !(fault==5 || (fault==6 && checks==2));},[&](const auto& bytes){++publishes;if(fault==7)return false;after=bytes;return true;});
        if(!fault){assert(done && publishes==1 && checks==2);f.verifyPixels(rgba,before,after);assert(physical.knownTiles()==81);++positives;}
        else {assert(!done && after==f.prior() && !physical.knownTiles());assert(publishes==(fault==7));++negatives;}
    }
    std::printf("SINGLE_COLOR8_RESOLVE_HOST_PASS actual_requests=1 old_gates_reproduced=2 positives=%u plan_negatives=%u execution_negatives=%u padding_preserved=1 gpu=0\n",positives,planNegatives,negatives);
}
