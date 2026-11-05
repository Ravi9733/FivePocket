#include "native_deferred_swap_submission.h"
#include "native_post_output_swap_fixture.h"
#include <cassert>
#include <cstdio>
#include <type_traits>
using namespace recomp::gpu;
int main(){
    static_assert(!std::is_same_v<NativePostOutputSwapWrapper,NativeDeferredSwapWrapper>);
    static_assert(!std::is_convertible_v<NativeDeferredSwapWrapper,NativePostOutputSwapWrapper>);
    constexpr unsigned base=0x2000,prefixWords=64;
    const auto command=NativeSwapCommand::make({3,base+prefixWords*4+40,0x100000,73});assert(command);
    const auto wrapper=test::postOutputSwapBytes(*command);
    std::vector<std::uint8_t> bytes((prefixWords+129)*4);
    const auto put=[](auto& b,unsigned word,unsigned value){for(unsigned i=0;i<4;++i)b[word*4+i]=value>>(24-i*8);};
    // Framing-only prefix: deliberately not a four-draw rendering fixture.
    for(unsigned i=0;i<prefixWords;i+=2){put(bytes,i,0x2000+i);put(bytes,i+1,i);}
    std::copy(wrapper.begin(),wrapper.end(),bytes.begin()+prefixWords*4);
    const auto parse=[&](const auto& input){return NativeDeferredSwapSubmission::parse(*command,base,input.data(),input.size());};
    const auto plan=parse(bytes);assert(plan && plan->prefix().words().size()==prefixWords && plan->prefix().packets().size()==32 &&
        plan->swapOffsetWords()==64 && plan->swap().command().identity().ticket==3 && plan->swap().operations().size()==12);
    assert(!NativePostOutputSwapWrapper::parse(*command,base+prefixWords*4,wrapper.data(),wrapper.size()));
    assert(plan->sameRequest(*parse(bytes)));
    unsigned immutable=0;
    for(unsigned word=0;word<129;++word){if(word==123 || word==124 || word==127 || word==128)continue;
        for(unsigned bit=0;bit<32;++bit){auto bad=bytes;bad[(word+prefixWords)*4+3-bit/8]^=1u<<(bit%8);
            assert(!parse(bad));++immutable;}}
    auto changed=bytes;put(changed,1,0x12345678);assert(parse(changed) && !plan->sameRequest(*parse(changed)));
    auto truncated=bytes;put(truncated,prefixWords-2,0xC0011000);assert(!parse(truncated)); // Would consume wrapper header.
    for(unsigned size:{0u,516u,768u,776u,16388u})assert(!NativeDeferredSwapSubmission::parse(*command,base,bytes.data(),size));
    assert(!NativeDeferredSwapSubmission::parse(*command,base,nullptr,bytes.size()));
    for(unsigned address:{base+1,base+4,0x1FFFFFFCu})assert(!NativeDeferredSwapSubmission::parse(*command,address,bytes.data(),bytes.size()));
    const auto old=NativeSwapCommand::make({2,command->identity().commandPhysical,0x100000,73});assert(old);
    assert(!NativeDeferredSwapSubmission::parse(*old,base,bytes.data(),bytes.size()));
    // Whole-child exclusions are stricter than the standalone129word wrapper.
    for(unsigned target:{base,base+prefixWords*4-4}){
        auto aliased=bytes;const auto tail=test::postOutputSwapBytes(*command,target);
        std::copy(tail.begin(),tail.end(),aliased.begin()+prefixWords*4);assert(!parse(aliased));
        for(unsigned event:{123u,127u}){aliased=bytes;put(aliased,prefixWords+event,target|2);assert(!parse(aliased));}
    }
    auto before=plan->prefix().words();std::fill(bytes.begin(),bytes.end(),0);
    assert(plan->prefix().words()==before && plan->swap().command().bytes()==command->bytes() && immutable==4000);
    std::puts("DEFERRED_SWAP_SUBMISSION: 4000immutable bits,typed ticket3,owned prefix/129word tail,framing/whole-child aliases; no rendering/execution/events/retirement PASS");
}
