#include "native_color_chain_profile.h"
#include <cassert>
#include <limits>

using namespace recomp::gpu;
template<bool Copy,bool Init,bool Depth> void profile() {
    using P=NativeColorChainProfile<Copy,Init,Depth>;
    typename P::BufferSizes sizes{};
    sizes[0]=1382720;sizes[1]=4352;sizes[2]=P::OutputBytes;
    if constexpr(Copy){sizes[3]=P::BackingBytes;sizes[4]=P::CopyBytes;sizes[5]=P::EventBytes;}
    assert(P::validBufferSizes(sizes));
    for(unsigned i=0;i<P::BufferCount;++i){
        for(auto value:{0u,1u,3u,std::numeric_limits<unsigned>::max()}){
            auto bad=sizes;bad[i]=value;assert(!P::validBufferSizes(bad));
        }
        if(i>=2)for(auto value:{sizes[i]-4,sizes[i]+4}){
            auto bad=sizes;bad[i]=value;assert(!P::validBufferSizes(bad));
        }
    }
    for(unsigned i=0;i<2;++i){
        const unsigned low=i?4096:1382400,high=i?4608:1382912;
        for(unsigned value=low;value<=high;value+=4){auto good=sizes;good[i]=value;assert(P::validBufferSizes(good));}
        for(auto value:{low-4,high+4,low+1}){auto bad=sizes;bad[i]=value;assert(!P::validBufferSizes(bad));}
    }
    typename P::Code code;
    for(auto& words:code)words={0x07230203,0x10000,0,1,0};
    assert(P::validModuleFraming(code)); // Framing only; this is NOT an executable module.
    for(unsigned i=0;i<P::CodeCount;++i){
        const auto original=code[i];
        code[i][0]^=1;assert(!P::validModuleFraming(code));code[i]=original;
        code[i].resize(4);assert(!P::validModuleFraming(code));code[i]=original;
        code[i].resize(16384);assert(P::validModuleFraming(code));
        code[i].push_back(0);assert(!P::validModuleFraming(code));code[i]=original;
    }
    for(std::size_t n=0;n<=4100;++n)assert(P::validDepthSourceBytes(n)==(n>0 && n<=4096 && n%4==0));
}
int main() {
    profile<false,false,false>();profile<true,false,false>();
    profile<true,true,false>();profile<true,true,true>();
    NativeWordUpdatePlan p;p.bufferBytes=40;
    assert(validNativeColorChainEvents(p));
    const NativeWordRequest requests[]={{16,0,NativeWordPhase::AfterGraphics},{20,0xFFFFFFFF,NativeWordPhase::AfterGraphics}};
    assert(planNativeWordUpdates(requests,2,40,p) && validNativeColorChainEvents(p));
    for(auto count:{1u,3u,64u,65u,std::numeric_limits<unsigned>::max()}){
        auto bad=p;bad.count=count;assert(!validNativeColorChainEvents(bad));
    }
    for(auto bytes:{0u,36u,44u}){auto bad=p;bad.bufferBytes=bytes;assert(!validNativeColorChainEvents(bad));}
    for(unsigned i=0;i<2;++i){
        auto bad=p;bad.words[i].phase=NativeWordPhase::BeforeGraphics;assert(!validNativeColorChainEvents(bad));
        for(auto offset:{0u,12u,17u,24u,40u}){bad=p;bad.words[i].byteOffset=offset;assert(!validNativeColorChainEvents(bad));}
    }
    auto swapped=p;std::swap(swapped.words[0],swapped.words[1]);assert(!validNativeColorChainEvents(swapped));
}
