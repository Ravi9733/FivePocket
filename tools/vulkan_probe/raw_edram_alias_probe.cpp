#include "xenos_edram_alias_clear.h"
#include <array>
#include <charconv>
#include <cstdio>
#include <string_view>
int main(int argc,char** argv){
    if(argc!=12)return 2;std::array<std::uint32_t,11> w{};
    for(unsigned i=0;i<w.size();++i){const std::string_view value(argv[i+1]);
        const auto p=std::from_chars(value.data(),value.data()+value.size(),w[i],16);
        if(p.ec!=std::errc{} || p.ptr!=value.data()+value.size())return 2;}
    if(w[4]>1)return 2;
    recomp::gpu::AliasView view{w[0],w[1],w[2],w[3],w[4]!=0};
    recomp::gpu::AliasFootprint f;std::uint32_t at=0,packed=0,canonical=0;
    if(!recomp::gpu::fullAliasFootprint(view,f) || !recomp::gpu::aliasSampleOffset(view,w[5],w[6],w[7],at) ||
        !recomp::gpu::packCanonicalDepthStencil(w[8],w[9],packed) ||
        !recomp::gpu::canonicalClearDepthBits(w[10],canonical))return 1;
    std::printf("%u %u %u %u %u %u %u %u\n",f.begin,f.bytes,f.pitchTiles,f.tileRows,f.samples,at,packed,canonical);
}
