#include "xenos_color_copy.h"
#include "xenos_color_7e3.h"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace recomp::gpu;
int main(){
    const AliasView source{0x14000500,0xC02D0,1280,720,false};ColorCopyPlan plan;
    assert(planColorCopy(source,0x100000,0x1F2D8000,0x2D00500,0x1000300,plan));
    assert(plan.capacity==3768320 && plan.touchedSpan==3766272 && plan.pixelBytes==3686400 && plan.swapRedBlue);
    std::vector<std::uint8_t> visits(plan.capacity/4),edram(AliasEdramBytes,0x5A),dest(plan.capacity+32,0x5A);
    unsigned last=0;
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x){
        std::uint32_t to=0,from=0;assert(colorCopyOffset(1280,720,x,y,to));assert(to+4<=plan.capacity && to%4==0);
        assert(!visits[to/4]);visits[to/4]=1;if(to+4>last)last=to+4;
        assert(aliasSampleOffset(source,x,y,0,from));
        const std::uint32_t pattern=((x+y*1280)&1023)|(((x*7+y)&1023)<<10)|(((x+y*37)&1023)<<20)|((x&3)<<30);
        for(unsigned c=0;c<4;++c)edram[from+c]=static_cast<std::uint8_t>(pattern>>(8*c));
    }
    assert(last==plan.touchedSpan);const auto before=edram;
    assert(copyColorToOwnedBuffer(source,0x100000,0x1F2D8000,0x2D00500,0x1000300,edram.data(),edram.size(),dest.data()+16,plan.capacity));
    assert(edram==before);
    for(unsigned i=0;i<16;++i)assert(dest[i]==0x5A && dest[dest.size()-16+i]==0x5A);
    unsigned holes=0;for(unsigned i=0;i<visits.size();++i)if(!visits[i]){
        ++holes;for(unsigned c=0;c<4;++c)assert(dest[16+i*4+c]==0x5A);
    }
    assert(holes==20480);
    for(unsigned code=0;code<1024;++code){
        std::uint32_t f=0;assert(unpack7e3(code,f));const float v=colorFloat(f);
        volatile float scaled=(v>1?1:v)*255.f;volatile float biased=scaled+.5f;
        assert(color7e3ToUnorm8(code)==static_cast<unsigned>(biased));
    }
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x){
        std::uint32_t at=0;assert(colorCopyOffset(1280,720,x,y,at));
        const std::uint32_t pattern=((x+y*1280)&1023)|(((x*7+y)&1023)<<10)|(((x+y*37)&1023)<<20)|((x&3)<<30);
        const auto expected=copyColorWord(pattern,true);
        for(unsigned c=0;c<4;++c)assert(dest[16+at+c]==static_cast<std::uint8_t>(expected>>(8*c)));
    }
    const auto unchanged=dest;
    assert(!copyColorToOwnedBuffer(source,0x100001,0x1F2D8000,0x2D00500,0x1000300,edram.data(),edram.size(),dest.data()+16,plan.capacity));
    assert(!copyColorToOwnedBuffer(source,0x100000,0x1F2D8000,0x2D00500,0x1000300,edram.data(),edram.size(),dest.data()+16,plan.capacity-1));
    assert(!copyColorToOwnedBuffer(source,0x100000,0x1F2D8000,0x2D00500,0x1000300,edram.data(),edram.size(),edram.data()+4,plan.capacity));
    assert(dest==unchanged && edram==before);
    for(unsigned info:{0x1000301u,0x1000308u,0x1000310u,0x1010300u,0x2000300u})
        assert(!planColorCopy(source,0x100000,0x1F2D8000,0x2D00500,info,plan));
    assert(!planColorCopy(source,0x100000,0x1FFFF000,0x2D00500,0x1000300,plan));
    assert(!planColorCopy(source,0x100000,0x1F2D8004,0x2D00500,0x1000300,plan));
    std::printf("COLOR_COPY_REFERENCE_V1 pixels=921600 padding_bytes=81920 touched_span=%u rgba7e3_to_bgra8=1 gpu_executed=0\n",last);
}
