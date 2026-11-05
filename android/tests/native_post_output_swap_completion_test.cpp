#include "native_post_output_swap_completion.h"
#include "native_post_output_swap_fixture.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
template<unsigned Ticket> void exercise(){
    using Wrapper=NativeSurfaceSwapWrapper<Ticket>;
    const auto command=NativeSwapCommand::make({Ticket,0x2028,0x100000,67});assert(command);
    const auto bytes=test::postOutputSwapBytes(*command);
    const auto plan=Wrapper::parse(*command,0x2000,bytes.data(),bytes.size());assert(plan);
    const auto valid=[&](const auto& result){return validNativeSurfaceSwapCompletion(*plan,result);};
    unsigned rejected=0;
    for(const auto before:{0ULL,1ULL,8'589'934'590ULL,UINT64_MAX-8'589'934'592ULL}){
        for(const auto delta:{0ULL,1ULL,10ULL,8'589'934'591ULL}){
            NativeSurfaceSwapGpuResult<Ticket> good;
            good.beforeCompletionNs=before;good.afterCompletionNs=before+delta;
            good.nativeFences=6;good.interruptIdleFences=1;good.interruptAcknowledged=true;
            for(unsigned i=0;i<4;++i){const auto value=i<2?static_cast<std::uint32_t>((i?good.afterCompletionNs:before)/2):*plan->events()[i].literal;
                for(unsigned b=0;b<4;++b)good.eventBytes[i*4+b]=value>>(i<2?8*b:24-8*b);}
            assert(valid(good));
            for(unsigned bit=0;bit<128;++bit){auto bad=good;bad.eventBytes[bit/8]^=1u<<(bit%8);
                assert(!valid(bad));++rejected;}
            for(unsigned fences=0;fences<10;++fences)if(fences!=6){auto bad=good;bad.nativeFences=fences;
                assert(!valid(bad));++rejected;}
            for(unsigned fences:{0u,2u,6u,UINT32_MAX}){auto bad=good;bad.interruptIdleFences=fences;
                assert(!valid(bad));++rejected;}
            auto bad=good;bad.interruptAcknowledged=false;assert(!valid(bad));++rejected;
            bad=good;bad.afterCompletionNs=before+8'589'934'592ULL;assert(!valid(bad));++rejected;
            bad=good;bad.beforeCompletionNs=bad.afterCompletionNs+1;assert(!valid(bad));++rejected;
        }
    }
    std::printf("%s: valid_intervals=16 refusals=%u synthetic_receipts=1 guest_execution=0 PASS\n",
        Ticket==2?"NATIVE_POST_OUTPUT_SWAP_COMPLETION":"NATIVE_DEFERRED_SWAP_COMPLETION",rejected);
}
int main(){
    static_assert(!std::is_convertible_v<NativePostOutputSwapGpuResult,NativeDeferredSwapGpuResult>);
    exercise<2>();exercise<3>();
}
