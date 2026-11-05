#include "native_color_edram_snapshot.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main(){
    using P=NativeColorChainProfile<true,true,true>;using Snapshot=NativeColorEdramSnapshot;
    static_assert(!std::is_default_constructible_v<Snapshot> && !std::is_copy_constructible_v<Snapshot>);
    std::vector<std::uint8_t> half(P::OutputBytes,0x5A),edram(P::BackingBytes,0x5A),tiled(P::CopyBytes,0x5A),coverage(Snapshot::Bytes/4);
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x){
        const auto pixel=y*1280+x;std::uint32_t offset=0,to=0;
        assert(aliasSampleOffset({0x14000500,0xC02D0,1280,720,false},x,y,0,offset) && colorCopyOffset(1280,720,x,y,to));
        assert(offset>=Snapshot::Begin && offset+4<=Snapshot::Begin+Snapshot::Bytes && !coverage[(offset-Snapshot::Begin)/4]++);
        for(unsigned c=0;c<8;++c)half[16+pixel*8+c]=c==7?0x3C:0;
        for(unsigned c=0;c<4;++c){
            edram[16+offset+c]=c==3?0xC0:0; // Packed A2=3.
            tiled[16+to+c]=c==3?0xFF:0; // Converted UNORM8 alpha=255.
        }
    }
    assert(std::all_of(coverage.begin(),coverage.end(),[](auto n){return n==1;}));
    const auto capture=[&](){return Snapshot::capture(half.data(),half.size(),edram.data(),edram.size(),tiled.data(),tiled.size());};
    auto snapshot=capture();assert(snapshot && snapshot->knownBytes().size()==Snapshot::Bytes);
    unsigned rejects=0;
    const auto reject=[&](){assert(!capture());++rejects;std::uint32_t value=0;assert(snapshot->readWord(Snapshot::Begin,value) && value==0xC0000000);};
    for(auto* buffer:{&half,&edram,&tiled})for(auto at:{std::size_t{0},std::size_t{15},buffer->size()-16,buffer->size()-1}){
        (*buffer)[at]^=1;reject();(*buffer)[at]^=1;}
    for(unsigned at:{16u,16+Snapshot::Begin-1,16+Snapshot::Begin+Snapshot::Bytes,P::BackingBytes-17}){
        edram[at]^=1;reject();edram[at]^=1;}
    for(unsigned i:{0u,12345u,921599u}){std::uint32_t at=0,to=0;
        assert(aliasSampleOffset({0x14000500,0xC02D0,1280,720,false},i%1280,i/1280,0,at) && colorCopyOffset(1280,720,i%1280,i/1280,to));
        edram[16+at]^=1;reject();edram[16+at]^=1;
        tiled[16+to]^=1;reject();tiled[16+to]^=1;
        // 1.0 -> 0.5 changes packed alpha; 1.0 -> 1.25 would still saturate.
        half[16+i*8+7]^=4;reject();half[16+i*8+7]^=4;
    }
    for(unsigned mode=0;mode<6;++mode){
        assert(!Snapshot::capture(mode==0?nullptr:half.data(),half.size()-(mode==1?4:0),
            mode==2?nullptr:edram.data(),edram.size()+(mode==3?4:0),mode==4?nullptr:tiled.data(),tiled.size()-(mode==5?4:0)));++rejects;
    }
    for(unsigned offset:{0u,Snapshot::Begin-4,Snapshot::Begin+1,Snapshot::Begin+Snapshot::Bytes,AliasEdramBytes-4,UINT32_MAX}){
        std::uint32_t sentinel=73;assert(!snapshot->readWord(offset,sentinel) && sentinel==73);++rejects;
    }
    // Destroy all source storage; every defined packed word must survive.
    half={};edram={};tiled={};coverage={};auto moved=std::move(*snapshot);
    std::uint32_t sentinel=73;assert(!snapshot->readWord(Snapshot::Begin,sentinel) && sentinel==73);++rejects;
    for(unsigned at=Snapshot::Begin;at<Snapshot::Begin+Snapshot::Bytes;at+=4){std::uint32_t value=0;assert(moved.readWord(at,value) && value==0xC0000000);}
    std::printf("NATIVE_COLOR_EDRAM_SNAPSHOT: %u rejections; 921600 unique known words survive move/source destruction; no canary promotion PASS\n",rejects);
}
