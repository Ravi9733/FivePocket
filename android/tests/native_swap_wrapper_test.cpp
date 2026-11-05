#include "native_swap_wrapper.h"
#include "native_swap_completion.h"
#include "xenos_command_stream.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main(){
    constexpr unsigned child=0x2000,image=0x100000;
    const auto command=NativeSwapCommand::make({19,child+40,image,37});assert(command);
    std::array<std::uint8_t,NativeSwapWrapper::Bytes> bytes{};
    const auto put=[&](unsigned i,unsigned value){for(unsigned b=0;b<4;++b)bytes[i*4+b]=value>>(24-8*b);};
    const std::array<unsigned,10> prefix{0x5C8,0x20000,0xE00,1,0xE40,1,0xC0025800,0x80000003,0x8000,0xDEADBEEF};
    const std::array<unsigned,16> tail{0xC0025800,0x80000003,0x8004,0xDEADBEEF,
        0xC0003B00,0x7FFF,0xC0001000,0x77777777,0xC0025800,3,0x9002,0x12345678,0xC0025800,3,0xA002,0xABCDEF01};
    for(unsigned i=0;i<prefix.size();++i)put(i,prefix[i]);
    for(unsigned i=0;i<tail.size();++i)put(i+74,tail[i]);
    std::copy(command->bytes().begin(),command->bytes().end(),bytes.begin()+40);
    const auto parse=[&]{return NativeSwapWrapper::parse(*command,child,bytes.data(),bytes.size());};
    const auto good=parse();assert(good);
    assert(good->RequiresCounterSource && good->RequiresIdleCachePair && good->RequiresPostSwapInvalidation);
    const auto& events=good->events();
    for(unsigned i=0;i<4;++i){assert((events[i].wordOffset==std::array<unsigned,4>{6,74,82,86}[i]));
        assert(events[i].endian==(i<2?0u:2u));
        assert(events[i].source==(i<2?NativeSwapWrapper::Source::Counter:NativeSwapWrapper::Source::Literal));
        assert(events[i].phase==(i==0?NativeSwapWrapper::Phase::BeforeSwap:NativeSwapWrapper::Phase::AfterSwap));
        assert(events[i].literal.has_value()==(i>=2));
    }
    assert(events[0].physicalAddress==0x8000 && events[1].physicalAddress==0x8004 &&
        events[2].physicalAddress==0x9000 && events[3].physicalAddress==0xA000 &&
        *events[2].literal==0x12345678 && *events[3].literal==0xABCDEF01);
    unsigned rejected=0;
    for(unsigned i=0;i<bytes.size();++i){const auto w=i/4;
        if(w==8 || w==76 || w==84 || w==85 || w==88 || w==89)continue; // Valid variable addresses/literals.
        for(unsigned bit=0;bit<8;++bit){bytes[i]^=1u<<bit;assert(!parse());bytes[i]^=1u<<bit;++rejected;}
    }
    for(unsigned i:{8u,76u,84u,88u}){const auto saved=i<10?prefix[i]:tail[i-74];
        for(unsigned address:{0x20000000u,child,child+356,image,image+NativeSwapImagePlan::Capacity-4}){
            put(i,address|(i>=84?2u:0u));assert(!parse());}
        put(i,saved^1);assert(!parse());put(i,saved);
    }
    put(76,0x8008);assert(!parse());put(76,0x8004);
    put(84,0x8002);assert(!parse());put(84,0x9002);
    put(88,0x9002);assert(!parse());put(88,0xA002);
    for(unsigned i=0;i<bytes.size();++i)assert(!NativeSwapWrapper::parse(*command,child,bytes.data(),i));
    assert(!NativeSwapWrapper::parse(*command,child,bytes.data(),bytes.size()+4));
    assert(!NativeSwapWrapper::parse(*command,child,nullptr,bytes.size()));
    assert(!NativeSwapWrapper::parse(*command,child+4,bytes.data(),bytes.size()));
    // Normal PM4 still fails on the native marker; a successful request plan
    // is NOT an executable no-op and never assigns the ignored DEADBEEF value.
    Pm4Snapshot snapshot;Pm4Error error;
    assert(!decodePm4(bytes.data(),bytes.size(),{},snapshot,error) && error.offsetWords==10);
    assert(parse());
    NativeSwapGpuResult completion;completion.nativeFences=4;
    completion.beforeCompletionNs=0x1FFFFFFF0ULL;completion.afterCompletionNs=0x200000020ULL;
    for(unsigned i=0;i<4;++i){const auto value=i<2?static_cast<unsigned>((i==0?completion.beforeCompletionNs:completion.afterCompletionNs)/2):*events[i].literal;
        for(unsigned b=0;b<4;++b)completion.eventBytes[i*4+b]=value>>(i<2?b*8:24-b*8);}
    assert(validNativeSwapCompletion(*good,completion));
    for(unsigned i=0;i<16;++i)for(unsigned bit=0;bit<8;++bit){auto bad=completion;bad.eventBytes[i]^=1u<<bit;
        assert(!validNativeSwapCompletion(*good,bad));}
    for(unsigned bit=0;bit<32;++bit){auto bad=completion;bad.nativeFences^=1u<<bit;assert(!validNativeSwapCompletion(*good,bad));}
    auto bad=completion;bad.afterCompletionNs=bad.beforeCompletionNs-1;assert(!validNativeSwapCompletion(*good,bad));
    bad=completion;bad.afterCompletionNs=bad.beforeCompletionNs+8'589'934'592ULL;assert(!validNativeSwapCompletion(*good,bad));
    std::puts("NATIVE_SWAP_COMPLETION_AUDIT: 128 event bits,32 fence bits,wrap/backwards controls PASS; synthetic CPU only");
    std::printf("NATIVE_SWAP_WRAPPER_PLAN: %u immutable-bit controls, four ordered requests, two unresolved counters, no execution PASS\n",rejected);
}
