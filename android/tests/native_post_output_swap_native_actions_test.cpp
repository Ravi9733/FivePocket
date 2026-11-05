#include "native_post_output_swap_native_actions.h"
#include "native_post_output_swap_fixture.h"
#include <cassert>
#include <cstdio>
#include <stdexcept>
using namespace recomp::gpu;
template<unsigned Ticket> void exercise(){
    using Wrapper=NativeSurfaceSwapWrapper<Ticket>;
    const auto command=NativeSwapCommand::make({Ticket,0x2028,0x100000,67});assert(command);
    const auto bytes=test::postOutputSwapBytes(*command);
    const auto plan=Wrapper::parse(*command,0x2000,bytes.data(),bytes.size());assert(plan);
    using Actions=NativeSurfaceSwapNativeActions<Ticket>;using Journal=typename Actions::Journal;
    const std::array<unsigned,11> order{0,1,2,3,4,5,6,7,9,10,11};
    unsigned refusals=0;
    for(unsigned stage=0;stage<order.size();++stage){
        for(unsigned variant=0;variant<101;++variant){
            Journal journal{{0x579,{0x11223344,0xFFFFFFFF}}};NativeGraphicsState state;
            state.binMask=0xABCD000000000000ULL;state.knownBinWords=2;
            Actions actions(*plan,journal,state);unsigned idles=0;
            const auto idle=[&]{++idles;return true;};
            for(unsigned i=0;i<stage;++i)assert(actions.apply(plan->operations()[order[i]],idle));
            auto changed=plan->operations()[order[stage]];
            if(variant<32)changed.wordOffset^=1u<<variant;
            else if(variant<64)changed.target^=1u<<(variant-32);
            else if(variant<96)changed.value^=1u<<(variant-64);
            else changed.kind=static_cast<typename Wrapper::Kind>((static_cast<unsigned>(changed.kind)+1+variant-96)%6);
            const auto before=journal;const auto oldIdles=idles;
            assert(!actions.apply(changed,idle) && actions.failed() && !actions.complete() && journal==before && idles==oldIdles);
            assert(!actions.apply(plan->operations()[order[stage]],idle) && journal==before && idles==oldIdles);++refusals;
        }
    }
    for(unsigned fault=0;fault<4;++fault){
        Journal journal;NativeGraphicsState state;Actions actions(*plan,journal,state);
        assert(actions.apply(plan->operations()[0],{}));unsigned idles=0;bool caught=false;
        const auto idle=[&]{++idles;
            if(fault==1)throw std::runtime_error("native idle");
            if(fault==2){assert(!actions.apply(plan->operations()[1],{}));return true;}
            return false;
        };
        try{assert(!actions.apply(plan->operations()[1],fault==3?std::function<bool()>{}:idle));}
        catch(const std::runtime_error&){caught=true;}
        assert(caught==(fault==1) && actions.failed() && !actions.complete() && idles==(fault==3?0u:1u));
        assert(!actions.apply(plan->operations()[1],[]{return true;}));
    }
    Journal journal{{0x579,{0x11223344,0xFFFFFFFF}}};NativeGraphicsState state;
    state.binMask=0xABCD000000000000ULL;state.knownBinWords=2;
    Actions actions(*plan,journal,state);const auto count=journal.size();unsigned idles=0;
    for(auto i:order)assert(actions.apply(plan->operations()[i],[&]{++idles;return true;}) && journal.size()==count);
    assert(actions.complete() && idles==1 && state.binMask==0xABCD0000FFFFFFFFULL && state.knownBinWords==3);
    assert(journal.at(0x579).first==0x11223344 && journal.at(0x578).first==4 && journal.at(0x57C).first==0x0BADF00D &&
        journal.at(0x57D).first==plan->callbackContext() && journal.at(0x45E).first==4 && journal.at(0x5C8).first==0x20000 &&
        !state.invalidationEpoch && !state.experimentalIdleCachePairEpoch);
    assert(!actions.apply(plan->operations()[11],{}) && actions.failed());
    std::printf("%s: %ufield/order refusals,4terminal idle controls,11exact actions,preallocated journal,scratch1 preserved; no guest/GPU/commit PASS\n",
        Ticket==2?"POST_OUTPUT_SWAP_NATIVE_ACTIONS":"DEFERRED_SWAP_NATIVE_ACTIONS",refusals);
}
int main(){
    static_assert(!std::is_convertible_v<NativePostOutputSwapWrapper::Operation,NativeDeferredSwapWrapper::Operation>);
    exercise<2>();exercise<3>();
}
