#include "color8_clear_execution_fixture.h"
#include <cstdio>
using namespace recomp::gpu;
int main(int argc,char** argv) {
    assert(argc==2);Color8ClearExecutionFixture f(argv[1]);const auto a=auditGta5Color8Clear(f.capture);
    assert(!a.issues && a.plan.width==320 && a.plan.height==320 && a.plan.pitch==320 &&
        a.plan.baseTile==1521 && a.plan.tileCount==80 && a.plan.samples==1 && a.plan.format==0 && !a.plan.depth);
    constexpr unsigned presentation=0x10059AE0;
    // Reproduce the actual failed producer/consumer connection: physical scene
    // rendering invalidates the old display cache; it does not destroy the
    // display owner or prevent a disjoint offscreen surface from being cleared.
    unsigned positives=0;
    for(bool validBefore:{false,true}) {
        bool displayValid=validBefore;NativeEdram32Bands physical;seedColor8ClearPhysical(physical);
        const auto old=physical.readColorWords(720,80,80,16);
        const bool oldAdmission=!a.issues && validBefore && !color8ClearIntersects(a.plan,720,720) && a.plan.object!=presentation;
        assert(oldAdmission==validBefore);
        NativeEdram32Mutation mutation(physical);unsigned currentCalls=0,execCalls=0;
        const bool done=executeNativeColor8ClearWrite(a,true,presentation,mutation,
            [&]{++currentCalls;return true;},[&]{++execCalls;return true;},[&] {
                std::vector<std::uint8_t> b(a.plan.width*a.plan.height*4);
                for(std::size_t i=0;i<b.size();++i)b[i]=a.plan.color[i%4];return std::optional{b};
            });
        if(done)displayValid=validBefore;
        assert(done && currentCalls==2 && execCalls==1 && displayValid==validBefore);
        assert(physical.readColorWords(720,80,80,16)==old);
        const auto output=physical.readColorWords(1521,320,320,320);
        assert(output && std::all_of(output->begin(),output->end(),[](auto v){return v==0;}));++positives;
    }
    unsigned negatives=0;
    for(unsigned fault=0;fault<16;++fault) {
        auto bad=a;bool owned=true;unsigned object=presentation;
        if(fault==0)owned=false;if(fault==1)object=0;if(fault==2)object=bad.plan.object;
        if(fault==3)bad.plan.baseTile=720;if(fault==4)bad.issues=ClearPartial;
        if(fault==5)bad.plan.format=4;if(fault==6)bad.plan.tileCount=0;
        if(fault==7)bad.plan.baseTile=2047;if(fault==8)bad.plan.samples=3;
        if(fault==9)bad.plan.object|=1;
        NativeEdram32Bands physical;seedColor8ClearPhysical(physical);NativeEdram32Mutation mutation(physical);
        unsigned checks=0,exec=0;
        const bool done=executeNativeColor8ClearWrite(bad,owned,object,mutation,
            [&]{++checks;return !((fault==10 && checks==1)||(fault==11 && checks==2));},
            [&]{++exec;return fault!=12;},[&]()->std::optional<std::vector<std::uint8_t>> {
                if(fault==13)return std::nullopt;
                auto b=std::vector<std::uint8_t>(bad.plan.width*bad.plan.height*4,0);
                if(fault==14)b.pop_back();if(fault==15)b[1]=1;return b;
            });
        assert(!done && !physical.knownTiles());if(fault<=10)assert(!exec);++negatives;
    }
    std::printf("COLOR8_CLEAR_EXECUTION_HOST_PASS captured_request=1 positives=%u negatives=%u stale_display_not_revived=1 physical_disjoint_preserved=1 gpu=0\n",positives,negatives);
}
